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

/*
 * Viterbi hard-decision decoder.
 *
 * A plain sliding-window Viterbi decoder over a dt_ccode. Unlike vindel/hybrid
 * it tracks no drift: received index == transmitted index, so each group of n
 * received bits is exactly one trellis step. It takes no channel-model
 * parameters - the trellis is the code's alone, the branch metric is Hamming
 * distance, and the decision depth is derived from the code (6*K). The state
 * register is known to start at 0 (the encoder's begin does the same), so output
 * is reliable from the first bit, trailing input by the fixed decision-depth
 * latency.
 *
 * Erasures fall out for free. An erased received symbol (DT_ERASURE, or any
 * non-boolean) gives no evidence for or against any branch, so it contributes a
 * neutral 0 to every branch metric and drops out of the compare-select - the
 * zero-reliability limit of a soft bit. (A deletion or insertion, by contrast,
 * would break the index == index assumption this decoder is built on; that is
 * what vindel is for.)
 *
 * Layout mirrors vindel's streaming decoder so the cadence reads the same:
 *   - a received-bit buffer, consumed a group of n at a time;
 *   - `metric`/`next_metric` over the 2^(K-1) encoder states (Hamming-distance
 *     path costs), renormalised each step so they stay bounded on a long stream;
 *   - a `decision_depth`-deep backpointer ring, one layer per step, each layer
 *     one packed (prev_state, bit) per destination state;
 *   - the (steps, decided) cadence: a step's decision is committed decision_depth
 *     steps after it is first seen, by which point the survivors have merged.
 */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h>

#include <limits.h> /* INT_MAX - a freestanding header (macros only, no libc) */

#include "../ccode_internal.h"

/* Unreachable-node sentinel for the integer path metrics. Predecessors holding
 * it are skipped before any add, so it never participates in arithmetic and
 * cannot overflow. */
#define VIT_INF INT_MAX

/* viterbi speaks dt_t bit symbols at its boundary; decisions go out as the
 * two bound booleans (the decoder always commits a 0 or 1, never an erasure). */
static dt_t vit_to_dt(unsigned int bit) { return bit ? DT_TRUE : DT_FALSE; }

/* Backpointer for one destination node: the predecessor state it came from and
 * the input bit of the winning edge, packed into one 16-bit word (layout
 * bit:1 | prev_state:15). dt_ccode_create caps K <= 9, so prev_state < 256 and
 * the packed value stays well under 16 bits; uint16_t halves the ring vs a
 * 32-bit word, which matters on the Cortex-M7 target. */
typedef uint16_t vit_backpointer;

static inline vit_backpointer vit_bp_pack(int prev_state, unsigned int bit) {
  return (vit_backpointer)((bit & 1u) | ((unsigned int)prev_state << 1));
}
static inline unsigned int vit_bp_bit(vit_backpointer b) { return b & 1u; }
static inline int vit_bp_state(vit_backpointer b) { return (int)(b >> 1); }

struct dt_viterbi_stream_decoder {
  const dt_ccode *code; /* borrowed; must outlive the decoder */

  int n;              /* code->n: coded bits per step / per group       */
  int num_states;     /* code->n_states: 1 << (K-1)                     */
  int decision_depth; /* look-ahead and traceback depth (6*K)           */

  /* Path metrics over encoder states (Hamming distance), plus per-step scratch
   * for the add-compare-select; swapped at the end of each step. */
  int *metric;      /* [num_states] cost to reach each state at frontier */
  int *next_metric; /* [num_states] scratch for the step being built     */

  /* Backpointer ring: decision_depth layers of num_states entries. The layer
   * for step t is index (t % decision_depth); each slot holds the winning
   * (prev_state, bit) for that destination state. */
  vit_backpointer *backpointers;

  /* Received-bit buffer: valid symbols live in received[0 .. received_length);
   * read_base is the index of the current step's first unconsumed bit. Symbols
   * are stored as dt_t so erasures survive buffering and are interpreted (as
   * neutral) only in the branch metric. */
  uint8_t *received;
  int received_capacity, received_length, read_base;

  long long steps;   /* trellis steps processed                       */
  long long decided; /* decisions emitted (next step index to emit)   */
};

/* -- received buffer ------------------------------------------------------- */

/* Append `n_in` received symbols. No drift means no history is needed below
 * read_base, so the consumed prefix is dropped first; then grow if needed. */
static int vit_feed(dt_viterbi_stream_decoder *d, const uint8_t *in, int n_in) {
  if (d->read_base > 0) {
    dt_memmove(d->received, d->received + d->read_base,
               (size_t)(d->received_length - d->read_base));
    d->received_length -= d->read_base;
    d->read_base = 0;
  }
  if (d->received_length + n_in > d->received_capacity) {
    int new_capacity = d->received_capacity ? d->received_capacity * 2 : 64;
    while (new_capacity < d->received_length + n_in) {
      new_capacity *= 2;
    }
    uint8_t *new_buffer = dt_realloc(d->received, (size_t)new_capacity);
    if (!new_buffer) {
      return DT_ERR_ALLOC;
    }
    d->received = new_buffer;
    d->received_capacity = new_capacity;
  }
  for (int i = 0; i < n_in; ++i) {
    d->received[d->received_length++] = in[i];
  }
  return DT_OK;
}

/* -- forward pass ---------------------------------------------------------- */

/* Branch metric for the edge whose expected codeword is `expected` (n raw 0/1
 * bits) against the received group `group` (n dt_t symbols): the Hamming
 * distance over the bits that carry a value. An erasure / non-boolean is
 * evidence-free and adds nothing, so it is neutral across every competing
 * edge. */
static int branch_metric(const uint8_t *expected, const uint8_t *group, int n) {
  int cost = 0;
  for (int j = 0; j < n; ++j) {
    uint8_t r = group[j];
    if (DT_IS_BIT(r)) {
      cost += (DT_BIT(r) != expected[j]);
    }
  }
  return cost;
}

/* Advance the trellis one step over the n-bit received group at `group`: the
 * add-compare-select scatter from each (state, bit) edge to its successor,
 * recording backpointers into this step's ring layer, then renormalise the new
 * frontier so the metrics stay bounded. */
static void vit_step(dt_viterbi_stream_decoder *d, const uint8_t *group) {
  const dt_ccode *code = d->code;
  const int ns = d->num_states, n = d->n;
  vit_backpointer *layer =
      d->backpointers + (size_t)(d->steps % d->decision_depth) * ns;

  for (int s = 0; s < ns; ++s) {
    d->next_metric[s] = VIT_INF;
  }

  for (int s = 0; s < ns; ++s) {
    const int m = d->metric[s];
    if (m == VIT_INF) {
      continue; /* unreachable predecessor: contributes nothing, never added */
    }
    for (int bit = 0; bit <= 1; ++bit) {
      const int edge = s * 2 + bit;
      const uint8_t *expected = &code->output[(size_t)edge * n];
      const int candidate = m + branch_metric(expected, group, n);
      const int dest = code->next_state[edge];
      if (candidate < d->next_metric[dest]) {
        d->next_metric[dest] = candidate;
        layer[dest] = vit_bp_pack(s, (unsigned int)bit);
      }
    }
  }

  /* Renormalise: subtract the frontier minimum from every reachable node. This
   * preserves all metric differences (hence every argmin and backpointer) while
   * keeping the values from growing without bound on a long stream. */
  int best = VIT_INF;
  for (int s = 0; s < ns; ++s) {
    if (d->next_metric[s] < best) {
      best = d->next_metric[s];
    }
  }
  if (best != VIT_INF) {
    for (int s = 0; s < ns; ++s) {
      if (d->next_metric[s] != VIT_INF) {
        d->next_metric[s] -= best;
      }
    }
  }

  int *tmp = d->metric;
  d->metric = d->next_metric;
  d->next_metric = tmp;
  d->steps++;
}

/* Lowest-cost state at the current frontier. */
static int vit_frontier(const dt_viterbi_stream_decoder *d) {
  int best_state = 0, best = d->metric[0];
  for (int s = 1; s < d->num_states; ++s) {
    if (d->metric[s] < best) {
      best = d->metric[s];
      best_state = s;
    }
  }
  return best_state;
}

/* Input bit decided at step `target`, traced from frontier node `frontier`
 * (the state after the most recent step). Walks the backpointer ring from the
 * frontier back to `target`; the edge into step `target` carries the bit. */
static unsigned int vit_trace(const dt_viterbi_stream_decoder *d, int frontier,
                              long long target) {
  const int ns = d->num_states, dd = d->decision_depth;
  int cur = frontier;
  unsigned int bit = 0;
  for (long long i = d->steps - 1; i >= target; --i) {
    vit_backpointer entry =
        d->backpointers[(size_t)(i % dd) * ns + cur];
    bit = vit_bp_bit(entry);
    cur = vit_bp_state(entry);
  }
  return bit;
}

/* Process whole received groups, emitting each step's decision decision_depth
 * steps after it is seen (once the survivors have merged), until input or
 * output runs out. Returns the number of decoded bits written. */
static int vit_run(dt_viterbi_stream_decoder *d, uint8_t *out, int max_out) {
  int output_count = 0;
  while (d->received_length - d->read_base >= d->n) {
    /* Processing the next step overwrites the backpointer layer of step
     * (steps - decision_depth), so its decision must be emitted first. During
     * the first decision_depth steps there is nothing committed yet. */
    if (d->steps >= d->decision_depth) {
      if (output_count >= max_out) {
        break;
      }
      out[output_count++] =
          vit_to_dt(vit_trace(d, vit_frontier(d), d->decided));
      d->decided++;
    }
    vit_step(d, d->received + d->read_base);
    d->read_base += d->n;
  }
  return output_count;
}

/* -- public API ------------------------------------------------------------ */

dt_viterbi_stream_decoder *dt_viterbi_stream_decoder_create(
    const dt_ccode *code) {
  if (!code) {
    return NULL;
  }
  dt_viterbi_stream_decoder *d = dt_calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->code = code;
  d->n = code->n;
  d->num_states = code->n_states;
  d->decision_depth = 6 * code->K; /* ccode.h's recommended ~6*K */

  d->metric = dt_malloc((size_t)d->num_states * sizeof(int));
  d->next_metric = dt_malloc((size_t)d->num_states * sizeof(int));
  d->backpointers = dt_malloc((size_t)d->decision_depth * d->num_states *
                              sizeof(vit_backpointer));
  if (!d->metric || !d->next_metric || !d->backpointers) {
    dt_viterbi_stream_decoder_destroy(d);
    return NULL;
  }

  /* The encoder starts at state 0, so the decoder does too: only state 0 is
   * reachable at the frontier before any input, the rest unreachable. */
  d->metric[0] = 0;
  for (int s = 1; s < d->num_states; ++s) {
    d->metric[s] = VIT_INF;
  }
  return d;
}

void dt_viterbi_stream_decoder_destroy(dt_viterbi_stream_decoder *d) {
  if (!d) {
    return;
  }
  dt_free(d->backpointers);
  dt_free(d->next_metric);
  dt_free(d->metric);
  dt_free(d->received);
  dt_free(d); /* dt_free(NULL) is a no-op */
}

int dt_viterbi_stream_decode(dt_viterbi_stream_decoder *d, const uint8_t *in,
                             int n_in, uint8_t *out, int max_out) {
  if (!d || n_in < 0 || (n_in > 0 && !in) || max_out < 0 ||
      (max_out > 0 && !out)) {
    return DT_ERR_ARG;
  }
  int status = vit_feed(d, in, n_in);
  if (status < 0) {
    return status;
  }
  return vit_run(d, out, max_out);
}

int dt_viterbi_stream_decode_flush(dt_viterbi_stream_decoder *d, uint8_t *out,
                                   int max_out) {
  if (!d || max_out < 0 || (max_out > 0 && !out)) {
    return DT_ERR_ARG;
  }

  /* First commit anything that crossed the decision-depth threshold but did not
   * fit in a prior call. */
  int output_count = vit_run(d, out, max_out);

  /* Then drain the pipeline: the last <= decision_depth steps never reached the
   * commit threshold, so decide them from the final frontier (the survivors are
   * as merged as they will get at end of stream). */
  if (d->decided < d->steps && output_count < max_out) {
    int frontier = vit_frontier(d);
    while (d->decided < d->steps && output_count < max_out) {
      out[output_count++] = vit_to_dt(vit_trace(d, frontier, d->decided));
      d->decided++;
    }
  }
  return output_count;
}
