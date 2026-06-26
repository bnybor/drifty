/* clang-format off */
/*
 * MIT License
 *
 * Copyright (c) 2026 Robyn Kirkman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* clang-format on */

/* clang-format off */
/*
 * BCJR (max-log-MAP / forward-backward) decoder.
 *
 * The channel is synchronous: the received stream is bit-aligned with the
 * encoder, so the trellis is just the encoder-state trellis and step t's coded
 * group is exactly the n received bits at offset t*n. Substitution noise and
 * erasures are handled.
 *
 * It is the max-product (max-log-MAP) sibling of a Viterbi decoder: instead of a
 * single most-likely traceback it runs a forward (alpha) AND a backward (beta)
 * recursion and combines them per step to weigh each input bit. Both a hard
 * decision and a soft per-bit output fall out of the same arithmetic.
 *
 * Windowing. The forward pass runs continuously, snapshotting the normalised
 * state metric (alpha_t) and the smoothed lock cost into rings. A bit is
 * committed once the frontier is at least decision_depth steps ahead of it.
 * Rather than sweep beta per bit, decisions are produced in BATCHES: once
 * decision_depth + batch steps have accumulated past the oldest uncommitted bit,
 * one backward sweep over that window emits batch bits at once (O(1) amortised
 * backward work per bit). Branch metrics (gamma) are NOT ringed - they are
 * recomputed from the retained received buffer during the sweep, trading a little
 * recompute for a lot less memory on a small target. batch == decision_depth here
 * (largest batch, least recompute); shrink it to trade memory for recompute.
 *
 * Combine. For input bit b at step t, m_b is the cost of the best complete path
 * whose step-t edge carries bit b: alpha_t[src] + gamma_t(src,b -> dst) +
 * beta_{t+1}[dst], minimised over states. mmin = min(m0, m1) is the global best.
 * Then c_true = exp(mmin - m1), c_false = exp(mmin - m0): the winning value reads
 * exactly 1 and the loser exp(-gap), so c_lost = min(c_true, c_false) is exactly
 * 1 - |c_true - c_false|. The value is recoverable unless m0 == m1 exactly (a true
 * tie - all evidence of this bit's value is missing or symmetric); a sustained
 * erasure burst drives both costs together and reads as lost, while a single
 * erasure is corrected by the surrounding parity (m0 != m1).
 *
 * Symbol contract (see decode.h). Output is the argmax projection of the
 * consistencies via a value-recoverability-first cascade: DT_ABSENT while not
 * locked (c_lock low), else the resolved DT_TRUE/DT_FALSE when the value is
 * recoverable, else - for an unrecoverable position - DT_INVALID when the slot's
 * coded group was itself DT_INVALID (the encoder's poison marker) or DT_ERASURE
 * otherwise. This relies on a property of the encoder: a non-boolean input
 * poisons exactly the coded bits that carry its value, so the originating
 * poisoned step has no clean value evidence and is a genuine m0 == m1 tie (->
 * DT_INVALID), whereas a clean bit merely contaminated downstream keeps clean
 * parity elsewhere and resolves to its real value. See finalize_emit.
 */
/* clang-format on */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h>

#include <math.h> /* INFINITY (a macro - no libc link dependency) */

#include "../ccode_internal.h"

/* Hard-decision thresholds (consistencies are in [0, 1]); tunable. */
#define DT_BCJR_LOCK_MIN 0.5f    /* c_lock below this -> DT_ABSENT              */
#define DT_BCJR_INVALID_MIN 0.5f /* c_invalid above this -> DT_INVALID on a tie */

/* ------------------------------------------------------------------------- */
/* Streaming (sliding-window) max-log-MAP decoder, synchronous channel        */
/* ------------------------------------------------------------------------- */

struct dt_bcjr_stream_decoder {
  const dt_ccode *code; /* borrowed; must outlive the decoder */

  int n, num_states, decision_depth;
  int batch, ring_len; /* decisions emitted per sweep; ring depth */

  /* Branch-metric constants, in cost (negative-log-likelihood) units. */
  float cost_match, cost_miss, cost_erase;
  /* Lock detection reference costs (channel-derived). */
  float expected_lock, expected_unlock;

  long long steps;           /* forward trellis steps processed              */
  long long committed;       /* decisions emitted to the FIFO (next step)    */
  long long received_origin; /* absolute stream offset of received[0]        */

  /* Received-bit buffer, kept as raw dt_t symbols (so DT_INVALID stays distinct
   * from DT_ERASURE, and c_invalid can be counted). read_base is the buffer
   * index of the next forward step's group; received_origin + read_base is the
   * absolute bit offset of that group, which equals steps * n. */
  dt_t *received;
  int received_capacity, received_length, read_base;

  /* Forward trellis working state over the num_states nodes. */
  float *metric;       /* [num_states] node costs (alpha at the frontier)    */
  float *next_metric;  /* [num_states] scratch                               */
  float smoothed_cost; /* EWMA of best-path per-step cost (lock signal)      */

  /* Backward sweep working vectors. */
  float *beta_tilde; /* [num_states] beta of the next step                  */
  float *beta_cur;   /* [num_states] beta of the current step (scratch)     */
  float *branch;     /* [2*num_states] branch cost per edge, current step   */

  /* Per-step rings, indexed (step % ring_len). */
  float *alpha_ring; /* [ring_len * num_states] alpha_t snapshots           */
  float *lock_ring;  /* [ring_len] smoothed_cost snapshot per step          */

  /* Output FIFO: decisions a sweep produced but the caller has not taken yet.
   * A sweep only runs when this is empty, so it is filled in step order and
   * drained from the head. */
  dt_t *fifo_sym;
  dt_bcjr_decode_details *fifo_det;
  int fifo_head, fifo_count;
};

/* Cost of one received symbol `s` against an expected 0/1 bit, in NLL units. A
 * clean boolean is scored match/miss; a DT_INVALID (bound, non-boolean - the
 * encoder's "no clean parity here" marker) is FREE so a poisoned run keeps lock
 * without favouring a value; anything else (DT_ERASURE, or a stray non-transmit
 * symbol) is the neutral but PENALISED erasure cost so a sustained burst reads
 * as a lost lock. */
static float dt_bit_cost(const dt_bcjr_stream_decoder *d, dt_t s, int expected) {
  if (s & DT_BOOLEAN) {
    return ((int)(s & DT_VALUE) == expected) ? d->cost_match : d->cost_miss;
  }
  if (s & DT_BOUND) {
    return 0.0f; /* DT_INVALID: neutral and free */
  }
  return d->cost_erase; /* DT_ERASURE / other: neutral, penalised */
}

/* True iff the received symbol is the encoder's DT_INVALID poison marker. */
static int dt_is_invalid_sym(dt_t s) {
  return (s & DT_BOUND) && !(s & DT_BOOLEAN);
}

/* Branch cost of edge (state*2+bit) reading the n received bits at buffer index
 * `base`; INFINITY if that group is not fully buffered (guards the stream ends).
 * This is the single definition of gamma used by BOTH the forward pass and the
 * backward sweep, so they agree exactly even at the boundaries. */
static float branch_cost(const dt_bcjr_stream_decoder *d, int edge, int base) {
  if (base < 0 || base + d->n > d->received_length) {
    return INFINITY;
  }
  const uint8_t *expected = &d->code->output[(size_t)edge * d->n];
  float cost = 0.0f;
  for (int j = 0; j < d->n; ++j) {
    cost += dt_bit_cost(d, d->received[base + j], expected[j]);
  }
  return cost;
}

/* -- received buffer ------------------------------------------------------- */

/* Buffer index of the oldest received bit any retained step still needs: the
 * group base of the oldest uncommitted step (committed * n, in absolute terms).
 * Once every step is committed, only the live forward cursor matters. */
static int oldest_needed_index(const dt_bcjr_stream_decoder *d) {
  long long base;
  if (d->committed < d->steps) {
    base = d->committed * (long long)d->n - d->received_origin;
  } else {
    base = d->read_base; /* == steps * n - received_origin */
  }
  if (base < 0) {
    return 0;
  }
  return (int)base;
}

/* Drop the dead prefix below the oldest still-needed bit, advancing the buffer
 * origin so absolute bit offsets stay consistent. Preserves the invariant
 * received_origin + read_base == steps * n. */
static void compact_received(dt_bcjr_stream_decoder *d) {
  const int keep_from = oldest_needed_index(d);
  if (keep_from <= 0) {
    return;
  }
  dt_memmove(d->received, d->received + keep_from,
             (size_t)(d->received_length - keep_from));
  d->received_length -= keep_from;
  d->read_base -= keep_from;
  d->received_origin += keep_from;
}

/* Ensure room for `extra` more bits, compacting then growing as needed. */
static int reserve_received(dt_bcjr_stream_decoder *d, int extra) {
  compact_received(d);
  if (d->received_length + extra > d->received_capacity) {
    int new_capacity = d->received_capacity * 2;
    if (new_capacity < d->received_length + extra) {
      new_capacity = d->received_length + extra;
    }
    dt_t *new_buffer = dt_realloc(d->received, (size_t)new_capacity);
    if (!new_buffer) {
      return DT_ERR_ALLOC;
    }
    d->received = new_buffer;
    d->received_capacity = new_capacity;
  }
  return DT_OK;
}

/* -- core trellis ---------------------------------------------------------- */

/* Subtract the lowest node cost from every node so the best sits at 0, and
 * return the subtracted amount. Over an unbounded stream this keeps costs
 * bounded; adding a constant to a whole metric layer changes no min decision.
 * The returned min is the best path's per-step cost increment (the lock signal).
 */
static float normalize(float *metric, int count) {
  float lowest = INFINITY;
  for (int i = 0; i < count; ++i) {
    if (metric[i] < lowest) {
      lowest = metric[i];
    }
  }
  if (lowest == INFINITY || lowest <= 0.0f) {
    return 0.0f;
  }
  for (int i = 0; i < count; ++i) {
    if (metric[i] != INFINITY) {
      metric[i] -= lowest;
    }
  }
  return lowest;
}

/* Forward pass: a plain Viterbi min-sum butterfly over the encoder states for
 * the group at buffer index `base`. No backpointers - the decision comes from
 * the alpha/beta combine, not a traceback. Folds the per-step cost increment
 * into the smoothed lock cost. */
static void forward_pass(dt_bcjr_stream_decoder *d, int base) {
  const dt_ccode *code = d->code;
  const int num_states = d->num_states;

  for (int state = 0; state < num_states; ++state) {
    d->next_metric[state] = INFINITY;
  }
  for (int state = 0; state < num_states; ++state) {
    const float current = d->metric[state];
    if (current == INFINITY) {
      continue;
    }
    for (int bit = 0; bit <= 1; ++bit) {
      const int edge = state * 2 + bit;
      const float bc = branch_cost(d, edge, base);
      if (bc == INFINITY) {
        continue;
      }
      const float cost = current + bc;
      const int next_state = code->next_state[edge];
      if (cost < d->next_metric[next_state]) {
        d->next_metric[next_state] = cost;
      }
    }
  }

  const float increment = normalize(d->next_metric, num_states);
  const float alpha = 2.0f / (d->decision_depth + 1.0f);
  d->smoothed_cost += alpha * (increment - d->smoothed_cost);

  float *temp = d->metric;
  d->metric = d->next_metric;
  d->next_metric = temp;
}

/* Advance one forward step: snapshot alpha_t (the state metric ENTERING step t)
 * and run the forward pass over step t's group, then snapshot the lock cost. */
static void forward_step(dt_bcjr_stream_decoder *d) {
  const int slot = (int)(d->steps % d->ring_len);

  dt_memcpy(d->alpha_ring + (size_t)slot * d->num_states, d->metric,
            (size_t)d->num_states * sizeof(float));

  forward_pass(d, d->read_base);
  d->lock_ring[slot] = d->smoothed_cost;

  d->steps++;
  d->read_base += d->n;
  if (d->read_base >= d->received_capacity / 2) {
    compact_received(d);
  }
}

/* Map a smoothed per-step cost to a lock consistency in [0, 1]: linear from
 * expected_lock (clean lock -> ~1) to expected_unlock (clearly not this code ->
 * ~0). A confidently decoded WRONG code is dominant but expensive, so it reads
 * as unlocked. */
static float lock_from(const dt_bcjr_stream_decoder *d, float smoothed) {
  const float gap = d->expected_unlock - d->expected_lock;
  if (gap <= 0.0f) {
    return 0.0f;
  }
  float p = (d->expected_unlock - smoothed) / gap;
  if (p < 0.0f) {
    p = 0.0f;
  } else if (p > 1.0f) {
    p = 1.0f;
  }
  return p;
}

/* Blind-acquisition init: every encoder state equally likely, so the decoder
 * locks whether it starts at the head of the stream or taps in mid-stream. */
static void init_metric(dt_bcjr_stream_decoder *d) {
  for (int state = 0; state < d->num_states; ++state) {
    d->metric[state] = 0.0f;
  }
}

/* -- backward sweep -------------------------------------------------------- */

/* Recompute step t's branch metrics (gamma) into d->branch from the retained
 * received buffer, using a LOCAL base - this must never touch d->read_base,
 * which is the live forward cursor. */
static void compute_branches(dt_bcjr_stream_decoder *d, int base) {
  const int edges = d->num_states * 2;
  for (int edge = 0; edge < edges; ++edge) {
    d->branch[edge] = branch_cost(d, edge, base);
  }
}

/* Project the combined costs (m0, m1) for step t into the six-field soft output
 * and the hard symbol, and push them onto the FIFO at this step's slot. */
static void finalize_emit(dt_bcjr_stream_decoder *d, long long t, float m0,
                          float m1) {
  const int slot = (int)(t % d->ring_len);
  const int idx = (int)(t - d->committed); /* FIFO empty -> linear, in order */
  dt_bcjr_decode_details det;

  const float mmin = (m0 < m1) ? m0 : m1;
  const float c_lock = lock_from(d, d->lock_ring[slot]);

  /* c_invalid: fraction of the step's coded group received as DT_INVALID. */
  const int base = (int)(t * (long long)d->n - d->received_origin);
  int inv = 0;
  for (int j = 0; j < d->n; ++j) {
    const int p = base + j;
    if (p >= 0 && p < d->received_length && dt_is_invalid_sym(d->received[p])) {
      ++inv;
    }
  }
  const float c_invalid = (float)inv / (float)d->n;

  if (mmin == INFINITY) {
    /* No complete path through this step survived: nothing here is backed by a
     * valid codeword. */
    det.c_true = 0.0f;
    det.c_false = 0.0f;
    det.c_lost = 0.0f;
    det.c_invalid = c_invalid;
    det.c_lock = c_lock;
    det.c_absent = 1.0f - c_lock;
    d->fifo_sym[idx] = DT_ABSENT;
    d->fifo_det[idx] = det;
    return;
  }

  const float c_true = dt_exp(mmin - m1); /* winner == 1, loser == exp(-gap) */
  const float c_false = dt_exp(mmin - m0);
  const float c_lost = (c_true < c_false) ? c_true : c_false;

  det.c_true = c_true;
  det.c_false = c_false;
  det.c_lost = c_lost;
  det.c_invalid = c_invalid;
  det.c_lock = c_lock;
  det.c_absent = 1.0f - c_lock;

  /* Hard decision - value-recoverability-first cascade. Ordering note: decode.h
   * lists DT_INVALID before DT_ERASURE for an unrecoverable slot; here a
   * RECOVERABLE value is resolved first (DT_TRUE/DT_FALSE), and only an
   * unrecoverable slot (an exact m0 == m1 tie - all of this bit's value evidence
   * is missing or symmetric) splits into DT_INVALID (its group was the encoder's
   * poison) vs DT_ERASURE. The originating poisoned step is a genuine tie (the
   * poison masks its own value) -> DT_INVALID; a clean bit merely contaminated
   * downstream keeps clean parity elsewhere and resolves to its value. A strict
   * left-to-right reading of decode.h (test c_invalid before the tie) would
   * instead mark such downstream clean bits DT_INVALID and lose them. */
  dt_t sym;
  if (c_lock < DT_BCJR_LOCK_MIN) {
    sym = DT_ABSENT;
  } else if (m0 != m1) {
    sym = (m1 < m0) ? DT_TRUE : DT_FALSE; /* recoverable value */
  } else if (c_invalid > DT_BCJR_INVALID_MIN) {
    sym = DT_INVALID; /* unrecoverable: group was the encoder's poison */
  } else {
    sym = DT_ERASURE; /* unrecoverable: value lost on a tracked stream */
  }

  d->fifo_sym[idx] = sym;
  d->fifo_det[idx] = det;
}

/* Compute beta for step t from beta_tilde (== beta of step t+1) and, when t is
 * in the emit range, the combined (m0, m1) for the decision. branch[] must hold
 * step t's gamma. Leaves beta_cur holding (normalised) beta_t. */
static void compute_beta_step(dt_bcjr_stream_decoder *d, long long t,
                              const float *alpha_t, long long emit_hi) {
  const dt_ccode *code = d->code;
  const int num_states = d->num_states;
  const int emit = (t <= emit_hi);
  float m0 = INFINITY, m1 = INFINITY;

  for (int state = 0; state < num_states; ++state) {
    d->beta_cur[state] = INFINITY;
  }
  for (int state = 0; state < num_states; ++state) {
    const float a = alpha_t[state];
    for (int bit = 0; bit <= 1; ++bit) {
      const int edge = state * 2 + bit;
      const float g = d->branch[edge];
      if (g == INFINITY) {
        continue;
      }
      const float bt = d->beta_tilde[code->next_state[edge]];
      if (bt == INFINITY) {
        continue;
      }
      const float inner = g + bt;
      if (inner < d->beta_cur[state]) {
        d->beta_cur[state] = inner;
      }
      if (emit && a != INFINITY) {
        const float cand = a + inner;
        if (bit) {
          if (cand < m1) m1 = cand;
        } else {
          if (cand < m0) m0 = cand;
        }
      }
    }
  }

  normalize(d->beta_cur, num_states); /* bound beta; never changes a decision */

  if (emit) {
    finalize_emit(d, t, m0, m1);
  }
}

/* One backward sweep over the window [committed, steps): beta open at the
 * frontier, sweep down recomputing gamma and beta, emit decisions for
 * [committed, emit_hi] into the FIFO, and advance committed. The FIFO is empty
 * on entry (the caller drains before sweeping). Never disturbs d->read_base. */
static void sweep_window(dt_bcjr_stream_decoder *d, long long emit_hi) {
  const int num_states = d->num_states;
  if (d->steps <= d->committed) {
    return;
  }

  for (int state = 0; state < num_states; ++state) {
    d->beta_tilde[state] = 0.0f; /* beta of the open frontier */
  }

  for (long long t = d->steps - 1; t >= d->committed; --t) {
    const float *alpha_t = d->alpha_ring + (size_t)(t % d->ring_len) * num_states;
    const int base = (int)(t * (long long)d->n - d->received_origin);
    compute_branches(d, base);
    compute_beta_step(d, t, alpha_t, emit_hi);
    /* Hand beta_t to the next (older) step by swapping the working vectors. */
    float *temp = d->beta_tilde;
    d->beta_tilde = d->beta_cur;
    d->beta_cur = temp;
  }

  d->fifo_head = 0;
  d->fifo_count = (int)(emit_hi - d->committed + 1);
  d->committed = emit_hi + 1;
}

/* -- forward pumping / output draining ------------------------------------- */

/* Can the forward pass take another step? It just needs the next group's n bits
 * buffered; commit latency is enforced by the sweep trigger. */
static int can_forward(const dt_bcjr_stream_decoder *d) {
  return d->received_length - d->read_base >= d->n;
}

/* Drain the FIFO into the caller's buffers, then advance the forward pass and
 * sweep to refill it, until the caller's buffer is full or there is no more
 * work. `out` and `details` are independently optional. */
static int produce(dt_bcjr_stream_decoder *d, uint8_t *out,
                   dt_bcjr_decode_details *details, int max_out, int draining) {
  const long long span = (long long)d->decision_depth + d->batch;
  int written = 0;

  for (;;) {
    while (d->fifo_count > 0 && written < max_out) {
      const int h = d->fifo_head;
      if (out) {
        out[written] = d->fifo_sym[h];
      }
      if (details) {
        details[written] = d->fifo_det[h];
      }
      ++d->fifo_head;
      --d->fifo_count;
      ++written;
    }
    if (written >= max_out) {
      return written;
    }
    /* FIFO now empty. Make more decisions, or stop. */
    if (can_forward(d)) {
      forward_step(d);
      if (d->steps - d->committed >= span) {
        sweep_window(d, d->committed + d->batch - 1);
      }
      continue;
    }
    if (draining && d->committed < d->steps) {
      sweep_window(d, d->steps - 1); /* flush the remaining window */
      continue;
    }
    return written;
  }
}

/* -- lifecycle ------------------------------------------------------------- */

static int decoder_init(dt_bcjr_stream_decoder *d,
                        const dt_bcjr_stream_params *params,
                        const dt_ccode *code) {
  const int decision_depth = params->decision_depth;
  const float p_flip = params->p_flip;
  const float p_erase = params->p_erase;

  if (decision_depth < 1) {
    return DT_ERR_ARG;
  }
  if (!(p_flip > 0.0f && p_flip < 1.0f) ||
      !(p_erase >= 0.0f && p_erase < 1.0f)) {
    return DT_ERR_ARG;
  }

  d->code = code;
  d->n = code->n;
  d->num_states = code->n_states;
  d->decision_depth = decision_depth;

  /* Channel model (NLL costs). The (1 - p_erase) factor is kept explicit so
   * paths reading different erasure counts compare correctly. */
  d->cost_match = -dt_log((1.0f - p_erase) * (1.0f - p_flip));
  d->cost_miss = -dt_log((1.0f - p_erase) * p_flip);
  d->cost_erase = -dt_log(p_erase); /* +inf when p_erase == 0 (never read) */

  /* Lock anchors (per step = n coded bits); see lock_from. */
  const float misfit_lock = p_flip;
  const float misfit_unlock = 0.5f * (p_flip + 0.5f);
  const float erase_term = p_erase > 0.0f ? p_erase * d->cost_erase : 0.0f;
  const float kept = 1.0f - p_erase;
  d->expected_lock =
      d->n * (erase_term + kept * ((1.0f - misfit_lock) * d->cost_match +
                                   misfit_lock * d->cost_miss));
  d->expected_unlock =
      d->n * (erase_term + kept * ((1.0f - misfit_unlock) * d->cost_match +
                                   misfit_unlock * d->cost_miss));

  /* Batched windowed MAP: emit `batch` decisions per sweep over a window of
   * decision_depth (look-ahead) + batch (emitted). batch == decision_depth. */
  d->batch = decision_depth;
  d->ring_len = decision_depth + d->batch + 2;

  d->steps = 0;
  d->committed = 0;
  d->received_origin = 0;
  d->read_base = 0;
  d->received_length = 0;
  d->received_capacity = (decision_depth + d->batch + 4) * d->n + 64;
  d->smoothed_cost = d->expected_unlock; /* assume unlocked until proven */
  d->fifo_head = 0;
  d->fifo_count = 0;

  const int num_states = d->num_states;
  const int edges = num_states * 2;
  const int rl = d->ring_len;

  d->received = dt_malloc((size_t)d->received_capacity);
  d->metric = dt_malloc((size_t)num_states * sizeof(float));
  d->next_metric = dt_malloc((size_t)num_states * sizeof(float));
  d->beta_tilde = dt_malloc((size_t)num_states * sizeof(float));
  d->beta_cur = dt_malloc((size_t)num_states * sizeof(float));
  d->branch = dt_malloc((size_t)edges * sizeof(float));
  d->alpha_ring = dt_malloc((size_t)rl * num_states * sizeof(float));
  d->lock_ring = dt_malloc((size_t)rl * sizeof(float));
  d->fifo_sym = dt_malloc((size_t)rl * sizeof(dt_t));
  d->fifo_det = dt_malloc((size_t)rl * sizeof(dt_bcjr_decode_details));
  if (!d->received || !d->metric || !d->next_metric || !d->beta_tilde ||
      !d->beta_cur || !d->branch || !d->alpha_ring || !d->lock_ring ||
      !d->fifo_sym || !d->fifo_det) {
    return DT_ERR_ALLOC;
  }

  init_metric(d);
  return DT_OK;
}

static void decoder_free(dt_bcjr_stream_decoder *d) {
  dt_free(d->received);
  dt_free(d->metric);
  dt_free(d->next_metric);
  dt_free(d->beta_tilde);
  dt_free(d->beta_cur);
  dt_free(d->branch);
  dt_free(d->alpha_ring);
  dt_free(d->lock_ring);
  dt_free(d->fifo_sym);
  dt_free(d->fifo_det);
}

static int decode_feed(dt_bcjr_stream_decoder *d, const uint8_t *in, int n_in) {
  int status = reserve_received(d, n_in);
  if (status < 0) {
    return status;
  }
  if (n_in > 0) { /* in may be NULL on a feed-free drain/pump call */
    dt_memcpy(d->received + d->received_length, in, (size_t)n_in);
    d->received_length += n_in;
  }
  return DT_OK;
}

/* -- public engine API ----------------------------------------------------- */

dt_bcjr_stream_decoder *dt_bcjr_stream_decoder_create(
    const dt_ccode *code, const dt_bcjr_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_bcjr_stream_decoder *d = dt_calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  if (decoder_init(d, params, code) < 0) {
    decoder_free(d);
    dt_free(d);
    return NULL;
  }
  return d;
}

void dt_bcjr_stream_decoder_destroy(dt_bcjr_stream_decoder *d) {
  if (!d) {
    return;
  }
  decoder_free(d);
  dt_free(d);
}

int dt_bcjr_stream_decode(dt_bcjr_stream_decoder *d, const uint8_t *in, int n_in,
                          uint8_t *out, dt_bcjr_decode_details *details,
                          int max_out) {
  if (!d || n_in < 0 || (n_in > 0 && !in) || max_out < 0 ||
      (max_out > 0 && !out && !details)) {
    return DT_ERR_ARG;
  }
  int status = decode_feed(d, in, n_in);
  if (status < 0) {
    return status;
  }
  return produce(d, out, details, max_out, /*draining=*/0);
}

int dt_bcjr_stream_decode_flush(dt_bcjr_stream_decoder *d, uint8_t *out,
                                dt_bcjr_decode_details *details, int max_out) {
  if (!d || max_out < 0 || (max_out > 0 && !out && !details)) {
    return DT_ERR_ARG;
  }
  return produce(d, out, details, max_out, /*draining=*/1);
}
