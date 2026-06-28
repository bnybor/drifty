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

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h>

#include <math.h>

#include "../ccode_internal.h"

/* vindel speaks dt_bit bit symbols (bit.h) at its public boundary, but the
 * internal engine works in a 0 / 1 / 0xFF convention (a coded/received bit is 0
 * or 1, an erasure is the 0xFF sentinel).
 * Convert at the two edges: received bits on the way in (decode_feed), decided
 * bits on the way out (run / flush). Expected codewords (code->output) are
 * already raw 0/1, so the cost model is untouched. */
#define VIN_ERASURE 0xFFu

static uint8_t vin_from_dt(dt_bit s) {
  /* DT_TRUE/DT_FALSE carry the value in their low bit; anything else (DT_ERASURE,
   * DT_INVALID, ...) is treated as a lost bit. */
  return DT_IS_BIT(s) ? (uint8_t)DT_BIT(s) : (uint8_t)VIN_ERASURE;
}

static dt_bit vin_to_dt(unsigned char bit) { return bit ? DT_TRUE : DT_FALSE; }

/*
 * Opt-in internal assertions. The core is built -ffreestanding -fno-builtin
 * -nostdlib, so it cannot use <assert.h> (that would pull in __assert_fail /
 * abort from libc). __builtin_trap is a compiler intrinsic - not a libc symbol
 * and not suppressed by -fno-builtin - so it traps without breaking the
 * freestanding link. Off by default (zero cost); define VIN_DEBUG_ASSERT to arm
 * the load-bearing invariants in the decoder (see trace). */
#if defined(VIN_DEBUG_ASSERT)
#define VIN_ASSERT(cond) \
  do {                   \
    if (!(cond)) {       \
      __builtin_trap();  \
    }                    \
  } while (0)
#else
#define VIN_ASSERT(cond) ((void)0)
#endif

/* Backpointer for one node: where it came from (prev_state, prev_drift_index)
 * and the input bit that got there, packed into one 32-bit word to shrink the
 * backpointer ring (~3x vs a struct) and make each forward-pass write a single
 * store. Layout: bit:1 | prev_drift_index:15 | prev_state:16. The field widths
 * dwarf the validated ranges (dt_cc_code_create caps K <= 9, so prev_state < 256;
 * decoder_init guards prev_drift_index/prev_state against overflow). */
typedef uint32_t vin_backpointer;

static inline vin_backpointer vin_bp_pack(int prev_state, int prev_drift_index,
                                          unsigned int bit) {
  return (bit & 1u) | ((unsigned int)prev_drift_index << 1) |
         ((unsigned int)prev_state << 16);
}
static inline unsigned int vin_bp_bit(vin_backpointer b) { return b & 1u; }
static inline int vin_bp_drift(vin_backpointer b) {
  return (int)((b >> 1) & 0x7FFFu);
}
static inline int vin_bp_state(vin_backpointer b) { return (int)(b >> 16); }

/* Field capacities for the packing above (used by decoder_init's guard). */
#define VIN_BP_MAX_STATE 0xFFFF
#define VIN_BP_MAX_DRIFT 0x7FFF

/* Re-acquisition lock floor: a lock estimate below this for reacquire_after
 * consecutive steps, after a prior lock, re-seeds the trellis so the decoder
 * re-acquires sync downstream from a sustained loss. */
#define VIN_LOCK_MIN 0.5f

/* ------------------------------------------------------------------------- */
/* Streaming (sliding-window) decoder                                        */
/* ------------------------------------------------------------------------- */

/* clang-format off */
/*
 * Runs the forward add-compare-select pass continuously over a buffer of
 * received bits, keeping only `decision_depth` steps of backpointers. A bit is
 * committed decision_depth steps after it is first seen, by which point the
 * candidate paths have merged. Output emerges at fixed latency decision_depth
 * with bounded memory and no frame boundaries.
 *
 * Each step emits one input bit's group of n coded bits, scored by a per-edge
 * bit-level alignment (see align_fill_into) that may insert or delete individual
 * received bits anywhere within the group. A node's drift is its running
 * net (insertions - deletions); a branch that consumes n + Delta received bits
 * moves drift by Delta, so indels at arbitrary bit positions - not just group
 * boundaries - are tracked exactly.
 *
 * Re-anchoring keeps the drift window centred on the committed timing: each
 * step the window may be shifted by sigma in {-1,0,+1} (folded into the read
 * cursor), so the net cumulative drift can grow without bound while each node's
 * stored drift stays inside +/- max_drift. The shift is recorded per step so
 * traceback can translate node indices across the moving coordinate frame.
 *
 * All decoder state - the received buffer and cadence, the channel-derived cost
 * constants, and the trellis working arrays over the [num_states][drift_width]
 * node grid - lives in one dt_cc_vindel_stream_decoder. Many of a code's 2*num_states
 * edges share the same n-bit output row, so the per-step alignment DP is computed
 * once per distinct output pattern (align_precompute) and the forward pass scatters
 * those shared rows, indexing them by each edge's pattern (group_of).
 */
/* clang-format on */

/* The decoder is one shared context plus one trellis, merged: a received buffer
 * and step cadence, the channel-derived cost constants, the per-step alignment
 * scratch, and the trellis metric/backpointers over the node grid. */
struct dt_cc_vindel_stream_decoder {
  const dt_cc_code *code; /* borrowed; must outlive the decoder */

  int n, max_drift, num_states, drift_width, decision_depth;
  /* Branch-metric constants, in cost (negative-log-likelihood) units. */
  float cost_match, cost_miss, cost_erase, cost_keep, cost_ins, cost_del;
  /* Lock detection reference costs (channel-derived). */
  float expected_lock, expected_unlock;

  long long steps;   /* trellis steps processed                      */
  long long decided; /* decisions emitted (next step index to emit)  */

  /* Received-bit buffer: valid bits live in received[0 .. received_length).
   * read_base is the buffer index of the current step's zero-drift read base. */
  uint8_t *received;
  int received_capacity, received_length, read_base;

  int *shift; /* [decision_depth] re-anchor sigma per step */

  /* Trellis working state over the [num_states][drift_width] node grid. */
  float *metric;                /* [num_states*drift_width] node costs      */
  float *next_metric;           /* [num_states*drift_width] scratch         */
  vin_backpointer *backpointers; /* [decision_depth*num_states*drift_width]  */
  float smoothed_cost;          /* EWMA of best-path per-step cost          */
  int reacquire_after;          /* sustained-unlock steps before a re-seed  */
  int unlock_run;               /* consecutive low-lock steps (reacquire)   */
  int locked_once;              /* set after first lock; gates reacquire    */

  /* The code's distinct output patterns. Each edge emits an n-bit output row,
   * but many edges share a row (at most 2^n distinct rows over 2*num_states
   * edges), and an edge's alignment DP depends only on that row and the source
   * drift - not on which edge it is. So the per-step alignment is computed once
   * per (pattern, drift) into align_shared, and the forward pass reads it by
   * group_of[edge] rather than recomputing per edge. */
  uint8_t *pattern_bits; /* [n_patterns * n] distinct output rows           */
  int n_patterns;        /* distinct output patterns across the code's edges */
  int *group_of;         /* [2*num_states] (state*2+bit) -> pattern index    */

  /* Per-step alignment scratch, recomputed each step. */
  float *alignment;     /* [(n+1)*(max_consume+1)] rows 0..n-1 scratch      */
  float *align_shared;  /* [n_patterns*drift_width*(max_consume+1)] final rows */
  /* Per-step received-window match costs, indexed by position - match_lo:
   * cost of aligning an expected 0/1 bit there, with in_range gating the ends. */
  float *match_cost0;   /* [n+4*max_drift] expected-bit-0 costs             */
  float *match_cost1;   /* [n+4*max_drift] expected-bit-1 costs             */
  signed char *in_range; /* [n+4*max_drift] 1 if position buffered           */
  int match_lo;          /* absolute received index of match table position 0 */
};

/* Flat index into the [num_states][drift_width] metric and backpointer arrays
 * for the node at encoder state `state` and drift index `drift_index`
 * (0..drift_width-1; drift_index == max_drift means zero drift). */
static size_t node_at(int state, int drift_index, int drift_width) {
  return (size_t)state * drift_width + drift_index;
}

/* -- received buffer ------------------------------------------------------- */

/* Drop the dead prefix. Keep 2*max_drift bits of history below read_base so a
 * re-anchor that steps the cursor back still has its window buffered. */
static void compact_received(dt_cc_vindel_stream_decoder *d) {
  int keep_from = d->read_base - 2 * d->max_drift;
  if (keep_from <= 0) {
    return;
  }
  dt_memmove(d->received, d->received + keep_from,
             (size_t)(d->received_length - keep_from));
  d->received_length -= keep_from;
  d->read_base -= keep_from;
}

/* Ensure room for `extra` more bits, compacting then growing as needed. */
static int reserve_received(dt_cc_vindel_stream_decoder *d, int extra) {
  compact_received(d);
  if (d->received_length + extra > d->received_capacity) {
    int new_capacity = d->received_capacity * 2;
    if (new_capacity < d->received_length + extra) {
      new_capacity = d->received_length + extra;
    }
    uint8_t *new_buffer = dt_realloc(d->received, (size_t)new_capacity);
    if (!new_buffer) {
      return DT_ERR_ALLOC;
    }
    d->received = new_buffer;
    d->received_capacity = new_capacity;
  }
  return DT_OK;
}

/* -- core trellis ---------------------------------------------------------- */

/* Flat index of the lowest-cost node at the current frontier (the node states
 * one step past the last one processed). */
static int frontier(const dt_cc_vindel_stream_decoder *d) {
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
static int pick_shift(const dt_cc_vindel_stream_decoder *d) {
  if (d->max_drift == 0) {
    return 0; /* no drift tracking: window is one wide, nothing to shift */
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
 * deletion, and substitution. cost_table[j][consumed] (stored row-major at
 * j*(max_consume+1)+consumed) is the min cost to align the first j expected bits
 * while consuming `consumed` received bits; max_consume = n + 2*max_drift is the
 * most bits any branch can consume. Reads outside the buffered region
 * (buffer_index < 0 or >= received_length) are infeasible, so only the deletion
 * move is available there - this is what keeps the ends of the stream safe.
 *
 * The branch cost into a node at ending drift di' is then
 * cost_table[n][n + (di' - di)]: consuming n + Delta received bits to emit the
 * group shifts drift by Delta. A matched/substituted bit also pays cost_keep (the
 * per-bit "not an indel" cost), so the model is fully bit-level rather than
 * per-group. */
/* clang-format on */
static void align_fill_into(const dt_cc_vindel_stream_decoder *d,
                            const uint8_t *expected, int base,
                            float *final_out) {
  const int n = d->n, max_consume = d->n + 2 * d->max_drift,
            stride = max_consume + 1;
  /* The per-bit match cost and in-range gate for each received position are
   * precomputed once per step (fill_match_costs) and shared by every alignment;
   * `off` maps this edge's read base into that table. The arithmetic is identical
   * to deriving them per cell. Rows 0..n-1 use d->alignment as scratch; the
   * final row (row n - the branch cost by total consumption) is written straight
   * into final_out, which every edge sharing this output pattern reads. */
  const int off = base - d->match_lo;
  const signed char *in_range = d->in_range;
  const float *match_cost0 = d->match_cost0, *match_cost1 = d->match_cost1;
  const float cost_ins = d->cost_ins, cost_del = d->cost_del;
  float *scratch = d->alignment;

  scratch[0] = 0.0f;
  for (int consumed = 1; consumed <= max_consume; ++consumed) {
    scratch[consumed] = in_range[off + consumed - 1]
                            ? scratch[consumed - 1] + cost_ins
                            : INFINITY;
  }
  for (int j = 1; j <= n; ++j) {
    float *cost_row = (j == n) ? final_out : scratch + (size_t)j * stride;
    const float *prev_row = scratch + (size_t)(j - 1) * stride;
    const float *match_cost = expected[j - 1] ? match_cost1 : match_cost0;
    cost_row[0] =
        prev_row[0] + cost_del; /* delete expected[j-1], consume nothing */
    for (int consumed = 1; consumed <= max_consume; ++consumed) {
      float best = prev_row[consumed] + cost_del; /* deletion always avail. */
      const int position = off + consumed - 1;
      if (in_range[position]) {
        const float align_cost =
            prev_row[consumed - 1] + match_cost[position]; /* match / sub */
        const float insert_cost =
            cost_row[consumed - 1] + cost_ins; /* extra received bit */
        if (align_cost < best) best = align_cost;
        if (insert_cost < best) best = insert_cost;
      }
      cost_row[consumed] = best;
    }
  }
}

/* Precompute the step's received-window match costs once, shared by every
 * align_fill_into this step (they all read the same buffer with the same channel
 * cost constants). Position p maps to absolute received index match_lo + p;
 * match_cost{0,1}[p] is the cost of aligning an expected 0/1 bit there (an erased
 * bit costs cost_erase either way), and in_range[p] gates positions off the ends
 * of the buffer (where only deletion is available - see align_fill_into). The
 * window spans every (source drift, consumed) an edge can touch: n + 4*max_drift
 * wide. */
static void fill_match_costs(dt_cc_vindel_stream_decoder *d) {
  const int window = d->n + 4 * d->max_drift;
  const float keep = d->cost_keep, match = d->cost_match, miss = d->cost_miss,
               erase = d->cost_erase;
  d->match_lo = d->read_base - d->max_drift;
  for (int p = 0; p < window; ++p) {
    const int absolute = d->match_lo + p;
    if (absolute >= 0 && absolute < d->received_length) {
      const uint8_t received_bit = d->received[absolute];
      if (received_bit == VIN_ERASURE) {
        d->match_cost0[p] = keep + erase;
        d->match_cost1[p] = keep + erase;
      } else {
        d->match_cost0[p] = keep + (received_bit == 0 ? match : miss);
        d->match_cost1[p] = keep + (received_bit == 1 ? match : miss);
      }
      d->in_range[p] = 1;
    } else {
      d->in_range[p] = 0;
    }
  }
}

/* Subtract the lowest node cost from every node, so the best one sits at 0.
 * Over an unbounded stream this keeps the costs from growing without limit.
 * Returns the amount subtracted: the best path's cost increment this step. */
static float normalize(float *metric, size_t count) {
  float lowest = INFINITY;
  for (size_t i = 0; i < count; ++i) {
    if (metric[i] < lowest) {
      lowest = metric[i];
    }
  }
  if (lowest == INFINITY) {
    return 0.0f;
  }
  if (lowest > 0.0f) {
    for (size_t i = 0; i < count; ++i) {
      if (metric[i] != INFINITY) {
        metric[i] -= lowest;
      }
    }
  }
  return lowest;
}

/* Shift the drift window by `sigma` (each node's drift index drift_index ->
 * drift_index - sigma); the read cursor is advanced once, by decode_step, to
 * match. Nodes shifted out of the window are dropped. Uses next_metric as
 * scratch.
 *
 * The dropped edge slot is set to INFINITY (sigma > 0 empties the top slot,
 * sigma < 0 the bottom). forward_pass then skips those slots as sources, so the
 * emptied edge can never be recorded as a backpointer's prev_drift_index - which
 * is exactly what keeps trace's `prev_drift_index + sigma` inside
 * [0, drift_width) when it steps back across this re-anchor. */
static void reanchor_metric(dt_cc_vindel_stream_decoder *d, int sigma) {
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

/* Compute, once per step, the per-(pattern, source drift) alignment rows that
 * the forward pass then reads. Alignment depends only on the output pattern and
 * the source drift, so it is computed once over the code's distinct output
 * patterns rather than once per edge. A drift is computed only when some state is
 * live there; dead-drift rows are never read by the scatter (it skips dead
 * nodes). */
static void align_precompute(dt_cc_vindel_stream_decoder *d) {
  const int n = d->n, max_drift = d->max_drift, num_states = d->num_states,
            drift_width = d->drift_width;
  const int stride = d->n + 2 * d->max_drift + 1;

  fill_match_costs(d);

  for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
    int live = 0;
    for (int state = 0; state < num_states; ++state) {
      if (d->metric[(size_t)state * drift_width + drift_index] != INFINITY) {
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

/* The forward pass: scatter the precomputed alignment rows (align_precompute,
 * already run this step) from d->metric into d->next_metric, recording this
 * step's backpointers, then normalise, update the smoothed cost, and swap. The
 * node/edge order (state, drift, bit, ending drift) matches the per-edge form, so
 * the `<` tie-break - and the decoded bits - are identical. */
static void forward_pass(dt_cc_vindel_stream_decoder *d) {
  const dt_cc_code *code = d->code;
  const int n = d->n, num_states = d->num_states, drift_width = d->drift_width;
  const size_t count = (size_t)num_states * drift_width;
  const int stride = d->n + 2 * d->max_drift + 1;

  for (size_t i = 0; i < count; ++i) {
    d->next_metric[i] = INFINITY;
  }
  vin_backpointer *layer =
      d->backpointers + (size_t)(d->steps % d->decision_depth) * count;

  for (int state = 0; state < num_states; ++state) {
    const size_t source_row = (size_t)state * drift_width;
    for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
      const float current_cost = d->metric[source_row + drift_index];
      if (current_cost == INFINITY) {
        continue;
      }
      /* Ending drift next_drift_index consumes n + (next_drift_index -
       * drift_index) bits; that must stay in [0, max_consume]. The high end is
       * always satisfied (next_drift_index <= drift_width-1 = 2*max_drift), so
       * only the low bound clamps the loop start. */
      int first = drift_index - n;
      if (first < 0) {
        first = 0;
      }
      for (int bit = 0; bit <= 1; ++bit) {
        const int edge = state * 2 + bit;
        const int next_state = code->next_state[edge];
        const float *final_row =
            d->align_shared +
            ((size_t)d->group_of[edge] * drift_width + drift_index) * stride;
        const size_t dest_row = (size_t)next_state * drift_width;
        for (int next_drift_index = first; next_drift_index < drift_width;
             ++next_drift_index) {
          const float branch_cost =
              final_row[n + (next_drift_index - drift_index)];
          if (branch_cost == INFINITY) {
            continue;
          }
          const float cost = current_cost + branch_cost;
          const size_t destination = dest_row + next_drift_index;
          if (cost < d->next_metric[destination]) {
            d->next_metric[destination] = cost;
            layer[destination] =
                vin_bp_pack(state, drift_index, (unsigned int)bit);
          }
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

/* Fast path for max_drift == 0 with indels disabled (cost_ins == cost_del ==
 * +inf, i.e. p_ins == p_del == 0). The drift window is one wide and the
 * alignment is forced diagonal, so an edge's cost is just the sum of its n
 * per-bit match costs - a plain Viterbi butterfly with no alignment DP and no
 * drift loops. This yields exactly the general path's branch cost (final_row[n])
 * in the same (state, bit) order, so output is identical. */
static void forward_pass_nodrift(dt_cc_vindel_stream_decoder *d) {
  const dt_cc_code *code = d->code;
  const int n = d->n, num_states = d->num_states;
  const int base = d->read_base;
  const int in_range = base >= 0 && base + n <= d->received_length;
  const float keep_total = (float)n * d->cost_keep;

  for (int state = 0; state < num_states; ++state) {
    d->next_metric[state] = INFINITY;
  }
  vin_backpointer *layer =
      d->backpointers +
      (size_t)(d->steps % d->decision_depth) * (size_t)num_states;

  for (int state = 0; state < num_states; ++state) {
    const float current_cost = d->metric[state];
    if (current_cost == INFINITY) {
      continue;
    }
    for (int bit = 0; bit <= 1; ++bit) {
      const int edge = state * 2 + bit;
      float branch_cost = INFINITY;
      if (in_range) {
        const uint8_t *expected = &code->output[(size_t)edge * n];
        branch_cost = keep_total;
        for (int j = 0; j < n; ++j) {
          const uint8_t received_bit = d->received[base + j];
          branch_cost += received_bit == VIN_ERASURE   ? d->cost_erase
                         : received_bit == expected[j] ? d->cost_match
                                                       : d->cost_miss;
        }
      }
      if (branch_cost == INFINITY) {
        continue;
      }
      const float cost = current_cost + branch_cost;
      const int next_state = code->next_state[edge];
      if (cost < d->next_metric[next_state]) {
        d->next_metric[next_state] = cost;
        layer[next_state] = vin_bp_pack(state, 0, (unsigned int)bit);
      }
    }
  }

  const float increment = normalize(d->next_metric, (size_t)num_states);
  const float alpha = 2.0f / (d->decision_depth + 1.0f);
  d->smoothed_cost += alpha * (increment - d->smoothed_cost);

  float *temp = d->metric;
  d->metric = d->next_metric;
  d->next_metric = temp;
}

/* Forward declarations: decode_step re-seeds the trellis (init_metric) and reads
 * the lock estimate on a sustained loss; both are defined further below. */
static void init_metric(dt_cc_vindel_stream_decoder *d);
static float lock_estimate(const dt_cc_vindel_stream_decoder *d);

/* Advance one step: decide and apply this step's re-anchor sigma (folded into
 * the window and the read cursor), then run the forward pass. With no drift
 * window and indels disabled the alignment is forced diagonal; a dedicated
 * butterfly skips the DP entirely (identical result). */
static void decode_step(dt_cc_vindel_stream_decoder *d) {
  const int sigma = pick_shift(d);
  if (sigma != 0) {
    reanchor_metric(d, sigma);
    d->read_base += sigma;
  }
  d->shift[d->steps % d->decision_depth] = sigma;

  const int nodrift = d->max_drift == 0 && d->cost_ins == INFINITY &&
                      d->cost_del == INFINITY;
  if (nodrift) {
    forward_pass_nodrift(d);
  } else {
    align_precompute(d);
    forward_pass(d);
  }

  /* Re-acquisition: after a prior lock, a sustained low-lock run (the decoder is
   * tracking garbage - e.g. a long corruption burst or drift it could not follow)
   * re-seeds the trellis at the current read position, so it re-acquires sync
   * downstream instead of staying stuck on a wrong path. (Hard-decision output
   * carries no DT_ABSENT marker, so the unlocked stretch reads as ordinary bits.) */
  if (lock_estimate(d) >= VIN_LOCK_MIN) {
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

/* Walk the backpointers from frontier node `frontier_node` back to step `target`
 * and return the input bit decided there. Each step's backpointer is stored in
 * that step's drift frame, so when stepping back across a re-anchor we shift the
 * predecessor's drift index by that step's recorded sigma. */
static unsigned char trace(const dt_cc_vindel_stream_decoder *d, int frontier_node,
                           long long target) {
  const size_t count = (size_t)d->num_states * d->drift_width;
  /* Traceback only reaches back through retained ring layers; a deeper target
   * would read a backpointer/shift slot the next step has already overwritten. */
  VIN_ASSERT(d->steps - target <= d->decision_depth);
  int node = frontier_node;
  unsigned char bit = 0;
  for (long long i = d->steps - 1; i >= target; --i) {
    const vin_backpointer *layer =
        d->backpointers + (size_t)(i % d->decision_depth) * count;
    const vin_backpointer entry = layer[node];
    if (i == target) {
      bit = (unsigned char)vin_bp_bit(entry);
      break;
    }
    const int prev_drift_index =
        vin_bp_drift(entry) + d->shift[i % d->decision_depth];
    /* In-bounds by construction: reanchor_metric empties the slot that would
     * translate out of the window, so it is never a recorded predecessor. */
    VIN_ASSERT(prev_drift_index >= 0 && prev_drift_index < d->drift_width);
    node = vin_bp_state(entry) * d->drift_width + prev_drift_index;
  }
  return bit;
}

/* Probability that the decoder is locked onto a valid stream of THIS specific
 * code, from the best surviving path's cost rate (a smoothed average, see
 * forward_pass). A path can only stay cheap if the received bits actually fit
 * this code's codewords; a different code's stream - even one of the same rate
 * and constraint length - forces mismatches that raise the cost. Map the
 * smoothed cost linearly from expected_lock (a clean lock -> ~1) to
 * expected_unlock (enough misfit that it clearly is not this code -> ~0).
 *
 * This is what makes the value code-specific rather than a generic "is some
 * path dominant?" confidence: a confidently decoded WRONG code is dominant but
 * expensive, so it reads as unlocked. (Two encoders for the SAME code are not
 * "wrong" - they share codewords - and correctly read as locked.) */
static float lock_estimate(const dt_cc_vindel_stream_decoder *d) {
  const float gap = d->expected_unlock - d->expected_lock;
  if (gap <= 0.0f) {
    return 0.0f;
  }
  float probability = (d->expected_unlock - d->smoothed_cost) / gap;
  if (probability < 0.0f) {
    probability = 0.0f;
  } else if (probability > 1.0f) {
    probability = 1.0f;
  }
  return probability;
}

/* Process steps, emitting forced decisions, until input/output limits hit. Each
 * emitted bit's lock probability is written to lock_out[] when non-NULL.
 * `draining` relaxes the look-ahead requirement for end-of-stream. */
static int run(dt_cc_vindel_stream_decoder *d, uint8_t *out, float *lock_out,
               int max_out, int draining) {
  int output_count = 0;
  for (;;) {
    if (!draining) {
      /* +1 of slack covers a re-anchor stepping the cursor forward. */
      if (d->received_length < d->read_base + d->n + d->max_drift + 1) {
        break; /* not enough look-ahead yet */
      }
    } else {
      if (d->received_length - d->read_base < d->n) {
        break; /* less than one group left  */
      }
    }

    /* Processing the next step overwrites the backpointer layer of step
     * (steps - decision_depth), so its decision must be emitted first. */
    if (d->steps >= d->decision_depth) {
      if (output_count >= max_out) {
        break;
      }
      if (lock_out) {
        lock_out[output_count] = lock_estimate(d);
      }
      out[output_count++] = vin_to_dt(trace(d, frontier(d), d->decided));
      d->decided++;
    }
    decode_step(d);
  }
  return output_count;
}

/* Initialise the metric for blind acquisition: every encoder state is equally
 * likely (all at zero drift), so the decoder locks on whether it starts at the
 * stream's beginning or is tapped partway through. */
static void init_metric(dt_cc_vindel_stream_decoder *d) {
  const size_t count = (size_t)d->num_states * d->drift_width;
  for (size_t i = 0; i < count; ++i) {
    d->metric[i] = INFINITY;
  }
  for (int state = 0; state < d->num_states; ++state) {
    d->metric[node_at(state, d->max_drift, d->drift_width)] =
        0.0f; /* zero drift, cost 0 */
  }
}

/* Build the code's distinct output-pattern table: dedupe the 2*num_states edges'
 * output rows into pattern_bits, and fill group_of[edge] with each edge's pattern
 * index. The pattern list cannot exceed the edge count, so pattern_bits (sized to
 * 2*num_states rows in decoder_init) never needs to grow. */
static void register_patterns(dt_cc_vindel_stream_decoder *d) {
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
      idx = d->n_patterns++;
      for (int j = 0; j < n; ++j) {
        d->pattern_bits[(size_t)idx * n + j] = row[j];
      }
    }
    d->group_of[edge] = idx;
  }
}

/* -- lifecycle ------------------------------------------------------------- */

/* Validate `params`, take dimensions from `code`, derive the cost constants, and
 * allocate every buffer. Returns DT_OK or a negative DT_ERR_*. A decoder that
 * failed (or was zero-initialised) is safe to pass to decoder_free. */
static int decoder_init(dt_cc_vindel_stream_decoder *d,
                        const dt_cc_vindel_stream_params *params,
                        const dt_cc_code *code) {
  const int decision_depth = params->decision_depth;
  const int max_drift = params->max_drift;
  const float p_sub = params->p_sub;
  const float p_ins = params->p_ins;
  const float p_del = params->p_del;
  const float p_erase = params->p_erase;

  if (decision_depth < 1 || max_drift < 0) {
    return DT_ERR_ARG;
  }
  if (!(p_sub > 0.0f && p_sub < 1.0f) || !(p_erase >= 0.0f && p_erase < 1.0f) ||
      !(p_ins + p_del < 1.0f) || p_ins < 0.0f || p_del < 0.0f) {
    return DT_ERR_ARG;
  }
  /* Insertion/deletion probabilities are only consulted when tracking drift;
   * with max_drift == 0 they may be left 0 (correct flips only). */
  if (max_drift > 0 && (p_ins <= 0.0f || p_del <= 0.0f)) {
    return DT_ERR_ARG;
  }

  d->code = code;
  d->n = code->n;
  d->max_drift = max_drift;
  d->num_states = code->n_states;
  d->drift_width = 2 * max_drift + 1;
  d->decision_depth = decision_depth;

  /* The packed backpointer (above) carries prev_state in 16 bits and
   * prev_drift_index in 15. dt_cc_code_create caps K (num_states <= 256) and real
   * max_drift is tiny, but guard the packing invariant rather than silently
   * truncating. */
  if (d->num_states - 1 > VIN_BP_MAX_STATE ||
      d->drift_width - 1 > VIN_BP_MAX_DRIFT) {
    return DT_ERR_ARG;
  }

  /* Channel model: a coded bit is erased with prob p_erase; otherwise it is
   * received and flipped with prob p_sub. The common (1 - p_erase) factor is
   * kept explicit so paths reading different erasure counts compare correctly
   * (p_erase = 0 reduces these to the plain hard-decision metric). */
  d->cost_match = -dt_log((1.0f - p_erase) * (1.0f - p_sub));
  d->cost_miss = -dt_log((1.0f - p_erase) * p_sub);
  d->cost_erase = -dt_log(p_erase); /* +inf when p_erase == 0 (never read) */
  d->cost_keep = -dt_log(1.0f - p_ins - p_del);
  d->cost_ins = -dt_log(p_ins);
  d->cost_del = -dt_log(p_del);

  /* Lock anchors (per step = n coded bits). A "kept" coded bit costs cost_keep
   * plus a match/miss term; the expected misfit fraction is p_sub when locked,
   * and we call it "unlocked" once misfit reaches the midpoint between p_sub
   * and 0.5 (random). Erased bits contribute cost_erase to both. */
  const float misfit_lock = p_sub;
  const float misfit_unlock = 0.5f * (p_sub + 0.5f);
  const float erase_term = p_erase > 0.0f ? p_erase * d->cost_erase : 0.0f;
  const float kept = 1.0f - p_erase;
  d->expected_lock = d->n * (d->cost_keep + erase_term +
                             kept * ((1.0f - misfit_lock) * d->cost_match +
                                     misfit_lock * d->cost_miss));
  d->expected_unlock = d->n * (d->cost_keep + erase_term +
                               kept * ((1.0f - misfit_unlock) * d->cost_match +
                                       misfit_unlock * d->cost_miss));

  d->steps = 0;
  d->decided = 0;
  d->read_base = 0;
  d->received_length = 0;
  d->received_capacity =
      (d->decision_depth + 2) * d->n + 8 * d->max_drift + 64;
  d->smoothed_cost = d->expected_unlock; /* assume unlocked until proven */
  d->reacquire_after = 2 * d->decision_depth;
  d->unlock_run = 0;
  d->locked_once = 0;

  const int max_consume = d->n + 2 * d->max_drift;
  const int window = d->n + 4 * d->max_drift;
  const int stride = max_consume + 1;
  const size_t count = (size_t)d->num_states * d->drift_width;
  const int edges = d->num_states * 2;

  d->shift = dt_malloc((size_t)d->decision_depth * sizeof(int));
  d->received = dt_malloc((size_t)d->received_capacity);
  d->alignment =
      dt_malloc((size_t)(d->n + 1) * (max_consume + 1) * sizeof(float));
  d->match_cost0 = dt_malloc((size_t)window * sizeof(float));
  d->match_cost1 = dt_malloc((size_t)window * sizeof(float));
  d->in_range = dt_malloc((size_t)window * sizeof(signed char));
  d->metric = dt_malloc(count * sizeof(float));
  d->next_metric = dt_malloc(count * sizeof(float));
  d->backpointers =
      dt_malloc((size_t)d->decision_depth * count * sizeof(vin_backpointer));
  d->group_of = dt_malloc((size_t)edges * sizeof(int));
  d->pattern_bits = dt_malloc((size_t)edges * d->n);
  if (!d->shift || !d->received || !d->alignment || !d->match_cost0 ||
      !d->match_cost1 || !d->in_range || !d->metric || !d->next_metric ||
      !d->backpointers || !d->group_of || !d->pattern_bits) {
    return DT_ERR_ALLOC;
  }

  register_patterns(d);

  d->align_shared = dt_malloc((size_t)d->n_patterns * d->drift_width *
                              (size_t)stride * sizeof(float));
  if (!d->align_shared) {
    return DT_ERR_ALLOC;
  }

  init_metric(d);
  return DT_OK;
}

static void decoder_free(dt_cc_vindel_stream_decoder *d) {
  dt_free(d->shift);
  dt_free(d->received);
  dt_free(d->alignment);
  dt_free(d->match_cost0);
  dt_free(d->match_cost1);
  dt_free(d->in_range);
  dt_free(d->metric);
  dt_free(d->next_metric);
  dt_free(d->backpointers);
  dt_free(d->group_of);
  dt_free(d->pattern_bits);
  dt_free(d->align_shared);
}

static int decode_feed(dt_cc_vindel_stream_decoder *d, const uint8_t *in,
                       int n_in) {
  int status = reserve_received(d, n_in);
  if (status < 0) {
    return status;
  }
  /* Normalise the dt_bit received symbols into the engine's 0/1/0xFF convention. */
  for (int i = 0; i < n_in; ++i) {
    d->received[d->received_length + i] = vin_from_dt(in[i]);
  }
  d->received_length += n_in;
  return DT_OK;
}

/* -- public single-stream decoder ------------------------------------------ */

dt_cc_vindel_stream_decoder *dt_cc_vindel_stream_decoder_create(
    const dt_cc_code *code, const dt_cc_vindel_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_cc_vindel_stream_decoder *d = dt_calloc(1, sizeof(*d));
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

void dt_cc_vindel_stream_decoder_destroy(dt_cc_vindel_stream_decoder *d) {
  if (!d) {
    return;
  }
  decoder_free(d);
  dt_free(d);
}

int dt_cc_vindel_stream_decode(dt_cc_vindel_stream_decoder *d, const uint8_t *in,
                            int n_in, uint8_t *out, float *lock_probability,
                            int max_out) {
  if (!d || (n_in > 0 && !in) || n_in < 0 || (max_out > 0 && !out) ||
      max_out < 0) {
    return DT_ERR_ARG;
  }
  int status = decode_feed(d, in, n_in);
  if (status < 0) {
    return status;
  }
  return run(d, out, lock_probability, max_out, /*draining=*/0);
}

int dt_cc_vindel_stream_decode_flush(dt_cc_vindel_stream_decoder *d, uint8_t *out,
                                  int max_out) {
  if (!d || (max_out > 0 && !out) || max_out < 0) {
    return DT_ERR_ARG;
  }
  int output_count = run(d, out, /*lock=*/NULL, max_out, /*draining=*/1);

  /* Drain the pipeline: decide the remaining buffered steps from the final
   * frontier (reduced traceback depth for the last <= decision_depth bits). */
  if (d->decided < d->steps && output_count < max_out) {
    int frontier_node = frontier(d);
    while (d->decided < d->steps && output_count < max_out) {
      out[output_count++] = vin_to_dt(trace(d, frontier_node, d->decided));
      d->decided++;
    }
  }
  return output_count;
}
