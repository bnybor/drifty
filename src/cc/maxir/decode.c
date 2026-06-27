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
 * MAXIR (max-log-MAP / forward-backward) decoder - drift-tolerant.
 *
 * The max-product (max-log-MAP) sibling of a Viterbi decoder: instead of a single
 * most-likely traceback it runs a forward (alpha) AND a backward (beta) recursion
 * and combines them per step to weigh each input bit. Both a hard decision and a
 * soft per-bit output fall out of the same arithmetic.
 *
 * Drift (insertions / deletions). The trellis is a (state x drift) super-trellis:
 * a node carries an encoder state and a running drift (net insertions - deletions)
 * in [-max_drift, +max_drift] (drift index 0..drift_width-1, drift_width =
 * 2*max_drift+1, centre = zero drift). Each step emits one input bit's n coded
 * bits, scored by a per-edge bit-level alignment DP (align_fill_into) that may
 * insert or delete individual received bits anywhere within the group: a branch
 * consuming n + Delta received bits moves drift by Delta, so indels at arbitrary
 * bit positions - not just group boundaries - are tracked exactly. Re-anchoring
 * shifts the window by sigma in {-1,0,+1} each step (folded into the read cursor)
 * so the net cumulative drift can grow without bound while each node's stored
 * drift stays inside the window; the shift is recorded per step so the backward
 * pass can translate the drift frame (dest = nd - sigma).
 *
 * Windowing. The forward pass runs continuously, snapshotting the normalised state
 * metric (alpha_t), the per-(pattern, drift) branch rows, the re-anchor shift, the
 * DT_INVALID-group count, and the smoothed lock cost into rings. Decisions are
 * produced in BATCHES: once decision_depth + batch steps accumulate past the
 * oldest uncommitted bit, one backward sweep over that window emits batch bits at
 * once. Gammas are taken from the snapshot (NOT recomputed), so forward and
 * backward agree exactly across re-anchors.
 *
 * Combine. For input bit b at step t, m_b is the cost of the best complete path
 * whose step-t edge carries bit b: alpha_t[src] + gamma_t(src,b -> dst) +
 * beta_{t+1}[dst], minimised over (state, drift, ending drift). mmin = min(m0, m1)
 * is the global best. Then c_true = exp(mmin - m1), c_false = exp(mmin - m0): the
 * winning value reads 1 and the loser exp(-gap), so c_lost = min(c_true, c_false)
 * is 1 - |c_true - c_false|. The value is recoverable unless m0 == m1 exactly (a
 * true tie - all evidence of this bit's value is missing or symmetric).
 *
 * Lock & re-acquisition. A code-specific lock consistency reads the smoothed
 * per-step cost (lock_from); when it stays low for a sustained run after a prior
 * lock, the forward metric is re-seeded (init_metric) so the decoder re-acquires
 * sync downstream from wherever the stream now is. The unlocked stretch reads as
 * DT_ABSENT via the cascade below.
 *
 * Symbol contract (see decode.h). Output is the argmax projection of the
 * consistencies via a value-recoverability-first cascade: DT_ABSENT while not
 * locked (c_lock low), else the resolved DT_TRUE/DT_FALSE when the value is
 * recoverable, else - for an unrecoverable position - DT_INVALID when the slot's
 * coded group was itself DT_INVALID (the encoder's poison marker) or DT_ERASURE
 * otherwise. This relies on a property of the encoder: a DT_INVALID input poisons
 * exactly the coded bits that carry its value, so the originating poisoned step has
 * no clean value evidence and is a genuine m0 == m1 tie (-> DT_INVALID). See
 * finalize_emit.
 */
/* clang-format on */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h>

#include <math.h> /* INFINITY (a macro - no libc link dependency) */

#include "../ccode_internal.h"

/* Hard-decision thresholds (consistencies are in [0, 1]); tunable. */
#define DT_MAXIR_LOCK_MIN 0.5f    /* c_lock below this -> DT_ABSENT              */
#define DT_MAXIR_INVALID_MIN 0.5f /* c_invalid above this -> DT_INVALID on a tie */

/* ------------------------------------------------------------------------- */
/* Streaming (sliding-window) drift-tolerant max-log-MAP decoder              */
/* ------------------------------------------------------------------------- */

struct dt_maxir_stream_decoder {
  const dt_ccode *code; /* borrowed; must outlive the decoder */

  int n, num_states, decision_depth;
  int max_drift, drift_width; /* drift_width = 2*max_drift + 1               */
  int batch, ring_len;        /* decisions per sweep; ring depth             */
  int reacquire_after;        /* sustained-unlock steps before a re-seed     */

  /* Channel model (NLL costs). cost_bit[expected][received] is the per-bit
   * match/mismatch cost (overwrite-aware, so asymmetric); insertion cost depends
   * on the consumed bit's value (cost_ins_t/f/e for a 1 / 0 / DT_ERASURE). */
  float cost_bit[2][2];
  float cost_erase, cost_keep, cost_del;
  float cost_ins_t, cost_ins_f, cost_ins_e;
  /* Lock detection reference costs (channel-derived). */
  float expected_lock, expected_unlock;

  long long steps;     /* forward trellis steps processed                    */
  long long committed; /* decisions emitted to the FIFO (next step)          */

  /* Received-bit buffer, kept as raw dt_bit symbols (so DT_INVALID stays distinct
   * from DT_ERASURE). read_base is the buffer index of the current step's
   * zero-drift read base; drift is measured relative to it. */
  dt_bit *received;
  int received_capacity, received_length, read_base;

  /* Forward trellis working state over the num_states*drift_width nodes. */
  float *metric;       /* [count] node costs (alpha at the frontier)         */
  float *next_metric;  /* [count] scratch (also re-anchor scratch)           */
  float smoothed_cost; /* EWMA of best-path per-step cost (lock signal)      */
  int unlock_run;      /* consecutive low-lock steps (re-acquisition)        */
  int locked_once;     /* set after the first lock; gates re-acquisition     */

  /* Backward sweep working vectors [count]. */
  float *beta_tilde; /* beta of the next (frontier-ward) step                */
  float *beta_cur;   /* beta of the current step (scratch)                   */

  /* Per-step alignment scratch. An edge's alignment depends only on its output
   * pattern and source drift, so the per-(pattern, drift) final rows are computed
   * once per step and indexed by pattern. */
  uint8_t *pattern_bits; /* [n_patterns * n] distinct output rows            */
  int n_patterns, pattern_cap;
  int *group_of;       /* [2*num_states] edge -> pattern index               */
  float *alignment;    /* [(n+1)*(max_consume+1)] alignment DP scratch       */
  float *align_shared; /* [n_patterns*drift_width*stride] this step's rows    */
  float *match_cost0;  /* [window] expected-0 per-position match cost         */
  float *match_cost1;  /* [window] expected-1 per-position match cost         */
  float *ins_cost;     /* [window] per-position insertion cost                */
  signed char *in_range; /* [window] 1 if the position is buffered           */
  int match_lo;          /* absolute received index of match position 0      */

  /* Per-step rings, indexed (step % ring_len). */
  float *alpha_ring;  /* [ring_len * count] alpha_t snapshots                 */
  float *branch_ring; /* [ring_len * n_patterns*drift_width*stride] gammas    */
  int *shift;         /* [ring_len] re-anchor sigma per step                  */
  float *lock_ring;   /* [ring_len] smoothed_cost snapshot per step           */
  int *inv_ring;      /* [ring_len] DT_INVALID count in step's zero-drift grp */

  /* Output FIFO: decisions a sweep produced but the caller has not taken yet.
   * A sweep only runs when this is empty, so it is filled in step order and
   * drained from the head. */
  dt_bit *fifo_sym;
  dt_maxir_decode_details *fifo_det;
  int fifo_head, fifo_count;
};

/* True iff the received symbol is the encoder's DT_INVALID poison marker. */
static int dt_is_invalid_sym(dt_bit s) {
  return (s & DT_BOUND) && !(s & DT_BOOLEAN);
}

/* Flat index of the node at encoder state `state` and drift index `drift_index`
 * (0..drift_width-1; drift_index == max_drift means zero drift). */
static size_t node_at(int state, int drift_index, int drift_width) {
  return (size_t)state * drift_width + drift_index;
}

/* -- received buffer ------------------------------------------------------- */

/* Drop the dead prefix. Keep 2*max_drift bits of history below read_base so a
 * re-anchor that steps the cursor back still has its window buffered. The
 * backward sweep reads only the snapshot rings, never `received`, so nothing
 * older is needed. */
static void compact_received(dt_maxir_stream_decoder *d) {
  const int keep_from = d->read_base - 2 * d->max_drift;
  if (keep_from <= 0) {
    return;
  }
  dt_memmove(d->received, d->received + keep_from,
             (size_t)(d->received_length - keep_from));
  d->received_length -= keep_from;
  d->read_base -= keep_from;
}

/* Ensure room for `extra` more bits, compacting then growing as needed. */
static int reserve_received(dt_maxir_stream_decoder *d, int extra) {
  compact_received(d);
  if (d->received_length + extra > d->received_capacity) {
    int new_capacity = d->received_capacity * 2;
    if (new_capacity < d->received_length + extra) {
      new_capacity = d->received_length + extra;
    }
    dt_bit *new_buffer = dt_realloc(d->received, (size_t)new_capacity);
    if (!new_buffer) {
      return DT_ERR_ALLOC;
    }
    d->received = new_buffer;
    d->received_capacity = new_capacity;
  }
  return DT_OK;
}

/* -- core trellis ---------------------------------------------------------- */

/* Flat index of the lowest-cost node at the current frontier. */
static int frontier(const dt_maxir_stream_decoder *d) {
  const size_t count = (size_t)d->num_states * d->drift_width;
  float best_cost = INFINITY;
  int best = 0;
  for (size_t i = 0; i < count; ++i) {
    if (d->metric[i] < best_cost) {
      best_cost = d->metric[i];
      best = (int)i;
    }
  }
  return best;
}

/* Choose this step's re-anchor shift: nudge the window one step toward centre
 * when the best node's drift leaves a deadband, so its drift stays well inside
 * the window even as cumulative drift grows. */
static int pick_shift(const dt_maxir_stream_decoder *d) {
  if (d->max_drift == 0) {
    return 0;
  }
  const int best_drift_index = frontier(d) % d->drift_width;
  const int drift = best_drift_index - d->max_drift;
  const int deadband = (d->max_drift + 1) / 2;
  if (drift >= deadband) {
    return +1;
  }
  if (drift <= -deadband) {
    return -1;
  }
  return 0;
}

/* clang-format off */
/* Fill the alignment DP for one edge: align the `n` expected output bits against
 * the received bits starting at buffer index `base`, allowing per-bit insertion,
 * deletion, and substitution. cost_table[j][consumed] (row-major at
 * j*(max_consume+1)+consumed) is the min cost to align the first j expected bits
 * while consuming `consumed` received bits; max_consume = n + 2*max_drift. Reads
 * outside the buffered region are infeasible, so only deletion is available there.
 * The branch cost into ending drift di' is then cost_table[n][n + (di' - di)]. */
/* clang-format on */
static void align_fill_into(const dt_maxir_stream_decoder *d,
                            const uint8_t *expected, int base, float *final_out) {
  const int n = d->n, max_consume = d->n + 2 * d->max_drift,
            stride = max_consume + 1;
  const int off = base - d->match_lo;
  const signed char *in_range = d->in_range;
  const float *match_cost0 = d->match_cost0, *match_cost1 = d->match_cost1;
  const float *ins_cost = d->ins_cost;
  const float cost_del = d->cost_del;
  float *scratch = d->alignment;

  scratch[0] = 0.0f;
  for (int consumed = 1; consumed <= max_consume; ++consumed) {
    scratch[consumed] = in_range[off + consumed - 1]
                            ? scratch[consumed - 1] + ins_cost[off + consumed - 1]
                            : INFINITY;
  }
  for (int j = 1; j <= n; ++j) {
    float *cost_row = (j == n) ? final_out : scratch + (size_t)j * stride;
    const float *prev_row = scratch + (size_t)(j - 1) * stride;
    const float *match_cost = expected[j - 1] ? match_cost1 : match_cost0;
    cost_row[0] = prev_row[0] + cost_del; /* delete expected[j-1], consume 0 */
    for (int consumed = 1; consumed <= max_consume; ++consumed) {
      float best = prev_row[consumed] + cost_del; /* deletion always avail.  */
      const int position = off + consumed - 1;
      if (in_range[position]) {
        const float align_cost =
            prev_row[consumed - 1] + match_cost[position]; /* match / sub     */
        const float insert_cost =
            cost_row[consumed - 1] + ins_cost[position]; /* extra received bit */
        if (align_cost < best) best = align_cost;
        if (insert_cost < best) best = insert_cost;
      }
      cost_row[consumed] = best;
    }
  }
}

/* Precompute the step's received-window match costs once, shared by every
 * align_fill this step. Position p maps to absolute received index match_lo + p.
 * A DT_INVALID symbol (the encoder's poison marker) is value-FREE on both
 * expected values (cost_keep only) so a poisoned group stays a value-symmetric
 * tie; it is deliberately NOT folded into the erasure branch (which is +inf when
 * p_ovr_erase == 0 and would make poisoned groups unalignable). */
static void fill_match_costs(dt_maxir_stream_decoder *d) {
  const int window = d->n + 4 * d->max_drift;
  const float keep = d->cost_keep, erase = d->cost_erase;
  d->match_lo = d->read_base - d->max_drift;
  for (int p = 0; p < window; ++p) {
    const int absolute = d->match_lo + p;
    if (absolute >= 0 && absolute < d->received_length) {
      const dt_bit received_bit = d->received[absolute];
      if (received_bit == DT_ERASURE) {
        d->match_cost0[p] = keep + erase;
        d->match_cost1[p] = keep + erase;
        d->ins_cost[p] = d->cost_ins_e;
      } else if (dt_is_invalid_sym(received_bit)) {
        d->match_cost0[p] = keep; /* value-free: poison favours no value */
        d->match_cost1[p] = keep;
        d->ins_cost[p] = d->cost_ins_e;
      } else {
        const int rv = DT_BIT(received_bit);
        d->match_cost0[p] = keep + d->cost_bit[0][rv];
        d->match_cost1[p] = keep + d->cost_bit[1][rv];
        d->ins_cost[p] = rv ? d->cost_ins_t : d->cost_ins_f;
      }
      d->in_range[p] = 1;
    } else {
      d->in_range[p] = 0;
    }
  }
}

/* Compute the per-(pattern, source drift) alignment rows the forward pass and the
 * branch snapshot then read. A drift is computed only when the trellis has a live
 * node there. */
static void align_precompute(dt_maxir_stream_decoder *d) {
  const int n = d->n, max_drift = d->max_drift, num_states = d->num_states,
            drift_width = d->drift_width;
  const int stride = n + 2 * max_drift + 1;

  fill_match_costs(d);

  for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
    int live = 0;
    for (int state = 0; state < num_states; ++state) {
      if (d->metric[node_at(state, drift_index, drift_width)] != INFINITY) {
        live = 1;
        break;
      }
    }
    if (!live) {
      continue;
    }
    const int base = d->read_base + (drift_index - max_drift);
    for (int p = 0; p < d->n_patterns; ++p) {
      align_fill_into(
          d, &d->pattern_bits[(size_t)p * n], base,
          d->align_shared + ((size_t)p * drift_width + drift_index) * stride);
    }
  }
}

/* Subtract the lowest node cost from every node so the best sits at 0, and return
 * the subtracted amount (the best path's per-step cost increment - the lock
 * signal). Adding a constant to a whole metric layer changes no min decision. */
static float normalize(float *metric, size_t count) {
  float lowest = INFINITY;
  for (size_t i = 0; i < count; ++i) {
    if (metric[i] < lowest) {
      lowest = metric[i];
    }
  }
  if (lowest == INFINITY || lowest <= 0.0f) {
    return 0.0f;
  }
  for (size_t i = 0; i < count; ++i) {
    if (metric[i] != INFINITY) {
      metric[i] -= lowest;
    }
  }
  return lowest;
}

/* Shift the drift window by `sigma` (each node's drift index drift_index ->
 * drift_index - sigma); the read cursor is advanced once by forward_step to
 * match. Nodes shifted out of the window are dropped (INFINITY). Uses
 * next_metric as scratch. */
static void reanchor_metric(dt_maxir_stream_decoder *d, int sigma) {
  const int num_states = d->num_states, drift_width = d->drift_width;
  for (int state = 0; state < num_states; ++state) {
    for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
      const int source = drift_index + sigma;
      d->next_metric[node_at(state, drift_index, drift_width)] =
          (source >= 0 && source < drift_width)
              ? d->metric[node_at(state, source, drift_width)]
              : INFINITY;
    }
  }
  dt_memcpy(d->metric, d->next_metric,
            (size_t)num_states * drift_width * sizeof(float));
}

/* The forward pass: scatter the alignment rows (align_precompute, already run
 * this step) from metric into next_metric, then normalise, update the smoothed
 * cost, and swap. */
static void forward_pass(dt_maxir_stream_decoder *d) {
  const dt_ccode *code = d->code;
  const int n = d->n, num_states = d->num_states, drift_width = d->drift_width;
  const size_t count = (size_t)num_states * drift_width;
  const int stride = n + 2 * d->max_drift + 1;

  for (size_t i = 0; i < count; ++i) {
    d->next_metric[i] = INFINITY;
  }
  for (int state = 0; state < num_states; ++state) {
    const size_t source_row = (size_t)state * drift_width;
    for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
      const float current_cost = d->metric[source_row + drift_index];
      if (current_cost == INFINITY) {
        continue;
      }
      int first = drift_index - n;
      if (first < 0) {
        first = 0;
      }
      for (int bit = 0; bit <= 1; ++bit) {
        const int edge = state * 2 + bit;
        const int next_state = code->next_state[edge];
        const float *restrict final_row =
            d->align_shared +
            ((size_t)d->group_of[edge] * drift_width + drift_index) * stride;
        float *restrict dst = d->next_metric + (size_t)next_state * drift_width;
        const int off = n - drift_index;
        /* Min-plus relaxation over the contiguous ending-drift range. An
         * unreachable edge (branch cost +INF) gives +INF and never wins the min,
         * so no INFINITY guard is needed - dropping it lets this vectorize while
         * staying identical to the per-edge `<` form. */
        for (int nd = first; nd < drift_width; ++nd) {
          const float cost = current_cost + final_row[off + nd];
          dst[nd] = dst[nd] < cost ? dst[nd] : cost;
        }
      }
    }
  }

  const float increment = normalize(d->next_metric, count);
  const float alpha = 2.0f / (d->decision_depth + 1.0f);
  d->smoothed_cost += alpha * (increment - d->smoothed_cost);

  float *temp = d->metric;
  d->metric = d->next_metric;
  d->next_metric = temp;
}

/* Map a smoothed per-step cost to a lock consistency in [0, 1]: linear from
 * expected_lock (clean lock -> ~1) to expected_unlock (clearly not this code ->
 * ~0). */
static float lock_from(const dt_maxir_stream_decoder *d, float smoothed) {
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

/* Blind-acquisition init: every encoder state equally likely at zero drift, so
 * the decoder locks whether it starts at the head of the stream or taps in
 * mid-stream. Also used to re-seed on a sustained loss of lock. */
static void init_metric(dt_maxir_stream_decoder *d) {
  const size_t count = (size_t)d->num_states * d->drift_width;
  for (size_t i = 0; i < count; ++i) {
    d->metric[i] = INFINITY;
  }
  for (int state = 0; state < d->num_states; ++state) {
    d->metric[node_at(state, d->max_drift, d->drift_width)] = 0.0f;
  }
}

/* Advance one forward step: re-anchor, record the shift, precompute alignments,
 * snapshot alpha / branch / lock / invalid into the rings (before the forward
 * pass overwrites metric), run the forward pass, then re-acquire if lock has
 * been lost for a sustained run. */
static void forward_step(dt_maxir_stream_decoder *d) {
  const int slot = (int)(d->steps % d->ring_len);
  const size_t count = (size_t)d->num_states * d->drift_width;
  const size_t shared =
      (size_t)d->n_patterns * d->drift_width * (size_t)(d->n + 2 * d->max_drift + 1);

  const int sigma = pick_shift(d);
  if (sigma != 0) {
    reanchor_metric(d, sigma);
    d->read_base += sigma;
  }
  d->shift[slot] = sigma;

  align_precompute(d);

  /* Snapshot this step (before forward_pass overwrites metric). */
  dt_memcpy(d->alpha_ring + (size_t)slot * count, d->metric,
            count * sizeof(float));
  dt_memcpy(d->branch_ring + (size_t)slot * shared, d->align_shared,
            shared * sizeof(float));
  int inv = 0;
  for (int j = 0; j < d->n; ++j) {
    const int p = d->read_base + j;
    if (p >= 0 && p < d->received_length && dt_is_invalid_sym(d->received[p])) {
      ++inv;
    }
  }
  d->inv_ring[slot] = inv;

  forward_pass(d);
  d->lock_ring[slot] = d->smoothed_cost;

  /* Re-acquisition: after a prior lock, a sustained low-lock run (the decoder is
   * tracking garbage - e.g. a long corruption burst or drift it could not follow)
   * re-seeds the trellis at the current read position, so it re-acquires sync
   * downstream instead of staying stuck on a wrong path. The unlocked stretch
   * reads DT_ABSENT via the finalize cascade. */
  if (lock_from(d, d->smoothed_cost) >= DT_MAXIR_LOCK_MIN) {
    d->locked_once = 1;
    d->unlock_run = 0;
  } else if (d->locked_once) {
    if (++d->unlock_run >= d->reacquire_after) {
      init_metric(d);
      d->smoothed_cost = d->expected_unlock;
      d->unlock_run = 0;
      d->locked_once = 0;
    }
  }

  d->steps++;
  d->read_base += d->n;
  if (d->read_base - d->max_drift >= d->received_capacity / 2) {
    compact_received(d);
  }
}

/* -- backward sweep -------------------------------------------------------- */

/* Project the combined costs (m0, m1) for step t into the six-field soft output
 * and the hard symbol, and push them onto the FIFO at this step's slot. */
static void finalize_emit(dt_maxir_stream_decoder *d, long long t, float m0,
                          float m1) {
  const int slot = (int)(t % d->ring_len);
  const int idx = (int)(t - d->committed); /* FIFO empty -> linear, in order */
  dt_maxir_decode_details det;

  const float mmin = (m0 < m1) ? m0 : m1;
  const float c_lock = lock_from(d, d->lock_ring[slot]);
  /* c_invalid: fraction of the step's zero-drift coded group received as
   * DT_INVALID (snapshotted at forward time; exact when local drift is 0). */
  const float c_invalid = (float)d->inv_ring[slot] / (float)d->n;

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

  /* Hard decision - value-recoverability-first cascade. A RECOVERABLE value is
   * resolved first (DT_TRUE/DT_FALSE); only an unrecoverable slot (an exact
   * m0 == m1 tie) splits into DT_INVALID (its group was the encoder's poison) vs
   * DT_ERASURE. */
  dt_bit sym;
  if (c_lock < DT_MAXIR_LOCK_MIN) {
    sym = DT_ABSENT;
  } else if (m0 != m1) {
    sym = (m1 < m0) ? DT_TRUE : DT_FALSE; /* recoverable value */
  } else if (c_invalid > DT_MAXIR_INVALID_MIN) {
    sym = DT_INVALID; /* unrecoverable: group was the encoder's poison */
  } else {
    sym = DT_ERASURE; /* unrecoverable: value lost on a tracked stream */
  }

  d->fifo_sym[idx] = sym;
  d->fifo_det[idx] = det;
}

/* Compute beta for step t from beta_tilde (== beta of step t+1, stored in the
 * post-re-anchor frame of step t+1) and, when t is in the emit range, the
 * combined (m0, m1). `sigma` is step t+1's re-anchor shift, so an ending drift
 * nd produced at step t indexes beta_tilde at dest = nd - sigma. Leaves beta_cur
 * holding (normalised) beta_t. */
static void compute_beta_step(dt_maxir_stream_decoder *d, long long t,
                              const float *alpha_t, const float *bslot, int sigma,
                              long long emit_hi) {
  const dt_ccode *code = d->code;
  const int n = d->n, num_states = d->num_states, drift_width = d->drift_width;
  const int stride = n + 2 * d->max_drift + 1;
  const size_t count = (size_t)num_states * drift_width;
  const int emit = (t <= emit_hi);
  float m0 = INFINITY, m1 = INFINITY;

  for (size_t i = 0; i < count; ++i) {
    d->beta_cur[i] = INFINITY;
  }
  for (int state = 0; state < num_states; ++state) {
    for (int di = 0; di < drift_width; ++di) {
      const float a = alpha_t[(size_t)state * drift_width + di];
      if (a == INFINITY) {
        continue;
      }
      int first = di - n;
      if (first < 0) {
        first = 0;
      }
      float beta_src = INFINITY;
      for (int bit = 0; bit <= 1; ++bit) {
        const int edge = state * 2 + bit;
        const size_t drow = (size_t)code->next_state[edge] * drift_width;
        const float *restrict frow =
            bslot + ((size_t)d->group_of[edge] * drift_width + di) * stride;
        const float *restrict beta = d->beta_tilde + drow;
        /* Fold the dest = nd - sigma window bounds into the loop range and drop
         * the +INF guard (an +INF candidate never wins): a contiguous min
         * reduction, identical to the guarded per-edge form. */
        int lo = first, hi = drift_width;
        if (sigma > lo) lo = sigma;
        if (drift_width + sigma < hi) hi = drift_width + sigma;
        float best_edge = INFINITY;
        for (int nd = lo; nd < hi; ++nd) {
          const float cand = frow[n + (nd - di)] + beta[nd - sigma];
          if (cand < best_edge) {
            best_edge = cand;
          }
        }
        if (best_edge < beta_src) {
          beta_src = best_edge;
        }
        if (emit && best_edge != INFINITY) {
          const float total = a + best_edge;
          if (bit) {
            if (total < m1) m1 = total;
          } else {
            if (total < m0) m0 = total;
          }
        }
      }
      d->beta_cur[(size_t)state * drift_width + di] = beta_src;
    }
  }

  normalize(d->beta_cur, count); /* bound beta; never changes a decision */

  if (emit) {
    finalize_emit(d, t, m0, m1);
  }
}

/* One backward sweep over the window [committed, steps): beta seeded at the
 * frontier (finite where the live metric is finite, else INF), sweep down reading
 * snapshotted gammas, emit decisions for [committed, emit_hi] into the FIFO, and
 * advance committed. The FIFO is empty on entry. Never disturbs read_base. */
static void sweep_window(dt_maxir_stream_decoder *d, long long emit_hi) {
  const size_t count = (size_t)d->num_states * d->drift_width;
  const size_t shared =
      (size_t)d->n_patterns * d->drift_width * (size_t)(d->n + 2 * d->max_drift + 1);
  if (d->steps <= d->committed) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    d->beta_tilde[i] = (d->metric[i] == INFINITY) ? INFINITY : 0.0f;
  }

  for (long long t = d->steps - 1; t >= d->committed; --t) {
    const int slot = (int)(t % d->ring_len);
    const float *alpha_t = d->alpha_ring + (size_t)slot * count;
    const float *bslot = d->branch_ring + (size_t)slot * shared;
    const int sigma =
        (t + 1 == d->steps) ? 0 : d->shift[(int)((t + 1) % d->ring_len)];
    compute_beta_step(d, t, alpha_t, bslot, sigma, emit_hi);
    float *temp = d->beta_tilde;
    d->beta_tilde = d->beta_cur;
    d->beta_cur = temp;
  }

  d->fifo_head = 0;
  d->fifo_count = (int)(emit_hi - d->committed + 1);
  d->committed = emit_hi + 1;
}

/* -- forward pumping / output draining ------------------------------------- */

/* Can the forward pass take another step? Non-draining needs the next group plus
 * drift headroom (+max_drift for an ending-drift branch, +1 for a pending
 * re-anchor); draining needs only the next group's n bits. */
static int can_forward(const dt_maxir_stream_decoder *d, int draining) {
  if (draining) {
    return d->received_length - d->read_base >= d->n;
  }
  return d->received_length >= d->read_base + d->n + d->max_drift + 1;
}

/* Drain the FIFO into the caller's buffers, then advance the forward pass and
 * sweep to refill it, until the caller's buffer is full or there is no more
 * work. `out` and `details` are independently optional. */
static int produce(dt_maxir_stream_decoder *d, uint8_t *out,
                   dt_maxir_decode_details *details, int max_out, int draining) {
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
    if (can_forward(d, draining)) {
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

/* Register the code's output patterns (deduping) and fill group_of[edge] with
 * each edge's pattern index, so the per-step alignment is computed once per
 * distinct output pattern, not once per edge. */
static int register_patterns(dt_maxir_stream_decoder *d) {
  const int n = d->n, edges = d->num_states * 2;
  d->n_patterns = 0;
  for (int edge = 0; edge < edges; ++edge) {
    const uint8_t *row = &d->code->output[(size_t)edge * n];
    int idx = -1;
    for (int p = 0; p < d->n_patterns; ++p) {
      int same = 1;
      for (int j = 0; j < n; ++j) {
        if (d->pattern_bits[(size_t)p * n + j] != row[j]) {
          same = 0;
          break;
        }
      }
      if (same) {
        idx = p;
        break;
      }
    }
    if (idx < 0) {
      if (d->n_patterns == d->pattern_cap) {
        const int newcap = d->pattern_cap * 2;
        uint8_t *grown = dt_realloc(d->pattern_bits, (size_t)newcap * n);
        if (!grown) {
          return DT_ERR_ALLOC;
        }
        d->pattern_bits = grown;
        d->pattern_cap = newcap;
      }
      idx = d->n_patterns++;
      for (int j = 0; j < n; ++j) {
        d->pattern_bits[(size_t)idx * n + j] = row[j];
      }
    }
    d->group_of[edge] = idx;
  }
  return DT_OK;
}

static void decoder_free(dt_maxir_stream_decoder *d) {
  dt_free(d->received);
  dt_free(d->metric);
  dt_free(d->next_metric);
  dt_free(d->beta_tilde);
  dt_free(d->beta_cur);
  dt_free(d->pattern_bits);
  dt_free(d->group_of);
  dt_free(d->alignment);
  dt_free(d->align_shared);
  dt_free(d->match_cost0);
  dt_free(d->match_cost1);
  dt_free(d->ins_cost);
  dt_free(d->in_range);
  dt_free(d->alpha_ring);
  dt_free(d->branch_ring);
  dt_free(d->shift);
  dt_free(d->lock_ring);
  dt_free(d->inv_ring);
  dt_free(d->fifo_sym);
  dt_free(d->fifo_det);
}

static int decoder_init(dt_maxir_stream_decoder *d,
                        const dt_maxir_stream_params *params,
                        const dt_ccode *code) {
  const int decision_depth = params->decision_depth;
  const int max_drift = params->max_drift;
  const float p_flip = params->p_flip;
  const float p_ins_true = params->p_ins_true;
  const float p_ins_false = params->p_ins_false;
  const float p_ins_erase = params->p_ins_erase;
  const float p_del = params->p_del;
  const float p_ovr_true = params->p_ovr_true;
  const float p_ovr_false = params->p_ovr_false;
  const float p_ovr_erase = params->p_ovr_erase;
  const float p_ins = p_ins_true + p_ins_false + p_ins_erase;
  const float p_ovr = p_ovr_true + p_ovr_false + p_ovr_erase;

  if (decision_depth < 1 || max_drift < 0) {
    return DT_ERR_ARG;
  }
  if (!(p_flip > 0.0f && p_flip < 1.0f) || p_ins_true < 0.0f ||
      p_ins_false < 0.0f || p_ins_erase < 0.0f || p_del < 0.0f ||
      p_ovr_true < 0.0f || p_ovr_false < 0.0f || p_ovr_erase < 0.0f ||
      !(p_ovr < 1.0f) || !(p_ins + p_del < 1.0f)) {
    return DT_ERR_ARG;
  }
  /* Indel probabilities are only consulted when tracking drift; with
   * max_drift == 0 they may be left 0 (correct flips only). */
  if (max_drift > 0 && (p_ins <= 0.0f || p_del <= 0.0f)) {
    return DT_ERR_ARG;
  }

  d->code = code;
  d->n = code->n;
  d->num_states = code->n_states;
  d->decision_depth = decision_depth;
  d->max_drift = max_drift;
  d->drift_width = 2 * max_drift + 1;

  /* Channel model: a coded bit is overwritten - DT_TRUE/DT_FALSE/DT_ERASURE at
   * probs p_ovr_* regardless of what was sent - else transmitted (prob
   * pn = 1 - p_ovr) and flipped with prob p_flip. p_ovr_erase doubles as the
   * plain erasure rate. */
  const float pn = 1.0f - p_ovr;
  d->cost_bit[0][0] = -dt_log(p_ovr_false + pn * (1.0f - p_flip));
  d->cost_bit[0][1] = -dt_log(p_ovr_true + pn * p_flip);
  d->cost_bit[1][0] = -dt_log(p_ovr_false + pn * p_flip);
  d->cost_bit[1][1] = -dt_log(p_ovr_true + pn * (1.0f - p_flip));
  d->cost_erase = -dt_log(p_ovr_erase); /* +inf when 0 (never read) */
  d->cost_keep = -dt_log(1.0f - p_ins - p_del);
  /* An inserted data bit's value is scored against a uniform 0/1 prior (the 2x);
   * an inserted erasure carries no value, taken at its own rate. */
  d->cost_ins_t = -dt_log(2.0f * p_ins_true);
  d->cost_ins_f = -dt_log(2.0f * p_ins_false);
  d->cost_ins_e = -dt_log(p_ins_erase);
  d->cost_del = -dt_log(p_del);

  /* Lock anchors (per step = n coded bits); see lock_from. */
  const float avg_match = 0.5f * (d->cost_bit[0][0] + d->cost_bit[1][1]);
  const float avg_miss = 0.5f * (d->cost_bit[0][1] + d->cost_bit[1][0]);
  const float misfit_lock =
      (pn * p_flip + 0.5f * (p_ovr_true + p_ovr_false)) / (1.0f - p_ovr_erase);
  const float misfit_unlock = 0.5f * (misfit_lock + 0.5f);
  const float erase_term = p_ovr_erase > 0.0f ? p_ovr_erase * d->cost_erase : 0.0f;
  const float kept = 1.0f - p_ovr_erase;
  d->expected_lock =
      d->n * (d->cost_keep + erase_term +
              kept * ((1.0f - misfit_lock) * avg_match + misfit_lock * avg_miss));
  d->expected_unlock =
      d->n *
      (d->cost_keep + erase_term +
       kept * ((1.0f - misfit_unlock) * avg_match + misfit_unlock * avg_miss));

  /* Batched windowed MAP: emit `batch` decisions per sweep over a window of
   * decision_depth (look-ahead) + batch (emitted). batch == decision_depth. */
  d->batch = decision_depth;
  d->ring_len = 2 * decision_depth + 2;
  d->reacquire_after = 2 * decision_depth;

  d->steps = 0;
  d->committed = 0;
  d->read_base = 0;
  d->received_length = 0;
  d->received_capacity = (d->ring_len + 2) * d->n + 8 * d->max_drift + 64;
  d->smoothed_cost = d->expected_unlock; /* assume unlocked until proven */
  d->unlock_run = 0;
  d->locked_once = 0;
  d->fifo_head = 0;
  d->fifo_count = 0;

  const int num_states = d->num_states;
  const int edges = num_states * 2;
  const int rl = d->ring_len;
  const int drift_width = d->drift_width;
  const size_t count = (size_t)num_states * drift_width;
  const int max_consume = d->n + 2 * d->max_drift;
  const int window = d->n + 4 * d->max_drift;

  d->n_patterns = 0;
  d->pattern_cap = edges;
  d->align_shared = NULL;
  d->branch_ring = NULL;

  d->received = dt_malloc((size_t)d->received_capacity);
  d->metric = dt_malloc(count * sizeof(float));
  d->next_metric = dt_malloc(count * sizeof(float));
  d->beta_tilde = dt_malloc(count * sizeof(float));
  d->beta_cur = dt_malloc(count * sizeof(float));
  d->pattern_bits = dt_malloc((size_t)d->pattern_cap * d->n);
  d->group_of = dt_malloc((size_t)edges * sizeof(int));
  d->alignment = dt_malloc((size_t)(d->n + 1) * (max_consume + 1) * sizeof(float));
  d->match_cost0 = dt_malloc((size_t)window * sizeof(float));
  d->match_cost1 = dt_malloc((size_t)window * sizeof(float));
  d->ins_cost = dt_malloc((size_t)window * sizeof(float));
  d->in_range = dt_malloc((size_t)window * sizeof(signed char));
  d->alpha_ring = dt_malloc((size_t)rl * count * sizeof(float));
  d->shift = dt_malloc((size_t)rl * sizeof(int));
  d->lock_ring = dt_malloc((size_t)rl * sizeof(float));
  d->inv_ring = dt_malloc((size_t)rl * sizeof(int));
  d->fifo_sym = dt_malloc((size_t)rl * sizeof(dt_bit));
  d->fifo_det = dt_malloc((size_t)rl * sizeof(dt_maxir_decode_details));
  if (!d->received || !d->metric || !d->next_metric || !d->beta_tilde ||
      !d->beta_cur || !d->pattern_bits || !d->group_of || !d->alignment ||
      !d->match_cost0 || !d->match_cost1 || !d->ins_cost || !d->in_range ||
      !d->alpha_ring || !d->shift || !d->lock_ring || !d->inv_ring ||
      !d->fifo_sym || !d->fifo_det) {
    return DT_ERR_ALLOC;
  }

  if (register_patterns(d) < 0) {
    return DT_ERR_ALLOC;
  }

  /* Now n_patterns is known, size the per-step alignment rows and their ring. */
  const int stride = d->n + 2 * d->max_drift + 1;
  const size_t shared = (size_t)d->n_patterns * drift_width * (size_t)stride;
  d->align_shared = dt_malloc(shared * sizeof(float));
  d->branch_ring = dt_malloc((size_t)rl * shared * sizeof(float));
  if (!d->align_shared || !d->branch_ring) {
    return DT_ERR_ALLOC;
  }

  init_metric(d);
  return DT_OK;
}

static int decode_feed(dt_maxir_stream_decoder *d, const uint8_t *in, int n_in) {
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

dt_maxir_stream_decoder *dt_maxir_stream_decoder_create(
    const dt_ccode *code, const dt_maxir_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_maxir_stream_decoder *d = dt_calloc(1, sizeof(*d));
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

void dt_maxir_stream_decoder_destroy(dt_maxir_stream_decoder *d) {
  if (!d) {
    return;
  }
  decoder_free(d);
  dt_free(d);
}

int dt_maxir_stream_decode(dt_maxir_stream_decoder *d, const uint8_t *in, int n_in,
                          uint8_t *out, dt_maxir_decode_details *details,
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

int dt_maxir_stream_decode_flush(dt_maxir_stream_decoder *d, uint8_t *out,
                                dt_maxir_decode_details *details, int max_out) {
  if (!d || max_out < 0 || (max_out > 0 && !out && !details)) {
    return DT_ERR_ARG;
  }
  return produce(d, out, details, max_out, /*draining=*/1);
}
