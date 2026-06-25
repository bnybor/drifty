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

#include <drifty/decode.h>

#include <drifty/stdlib.h>

#include <math.h>

#include "decode_internal.h"
#include "dt_internal.h"

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
 * bit-level alignment (see align_fill) that may insert or delete individual
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
 * The state splits into a shared dt_decode_ctx (received buffer, cadence, cost
 * constants, dimensions) and a per-code dt_trellis (metric/backpointers/etc).
 * Every step function takes (ctx, trellis); the shared dt_decode_step() advances
 * one or many trellises in lockstep over the one buffer with one re-anchor.
 */
/* clang-format on */

/* Flat index into the [num_states][drift_width] metric and backpointer arrays
 * for the node at encoder state `state` and drift index `drift_index`
 * (0..drift_width-1; drift_index == max_drift means zero drift). */
static size_t node_at(int state, int drift_index, int drift_width) {
  return (size_t)state * drift_width + drift_index;
}

/* -- received buffer ------------------------------------------------------- */

/* Drop the dead prefix. Keep 2*max_drift bits of history below read_base so a
 * re-anchor that steps the cursor back still has its window buffered. */
static void compact_received(dt_decode_ctx *ctx) {
  int keep_from = ctx->read_base - 2 * ctx->max_drift;
  if (keep_from <= 0) {
    return;
  }
  dt_memmove(ctx->received, ctx->received + keep_from,
          (size_t)(ctx->received_length - keep_from));
  ctx->received_length -= keep_from;
  ctx->read_base -= keep_from;
}

/* Ensure room for `extra` more bits, compacting then growing as needed. */
static int reserve_received(dt_decode_ctx *ctx, int extra) {
  compact_received(ctx);
  if (ctx->received_length + extra > ctx->received_capacity) {
    int new_capacity = ctx->received_capacity * 2;
    if (new_capacity < ctx->received_length + extra) {
      new_capacity = ctx->received_length + extra;
    }
    uint8_t *new_buffer = dt_realloc(ctx->received, (size_t)new_capacity);
    if (!new_buffer) {
      return DT_ERR_ALLOC;
    }
    ctx->received = new_buffer;
    ctx->received_capacity = new_capacity;
  }
  return DT_OK;
}

/* -- core trellis ---------------------------------------------------------- */

/* Flat index of the lowest-cost node at the current frontier (the node states
 * one step past the last one processed). */
int dt_trellis_frontier(const dt_decode_ctx *ctx, const dt_trellis *tr) {
  const size_t count = (size_t)ctx->num_states * ctx->drift_width;
  double best_cost = INFINITY;
  int best = 0;
  for (size_t i = 0; i < count; ++i) {
    if (tr->metric[i] < best_cost) {
      best_cost = tr->metric[i];
      best = (int)i;
    }
  }
  return best;
}

/* Choose this step's re-anchor shift: nudge the window one step toward centre
 * when the best node's drift leaves a deadband, so its drift stays well inside
 * the window even as cumulative drift grows. */
static int pick_shift(const dt_decode_ctx *ctx, const dt_trellis *tr) {
  if (ctx->max_drift == 0) {
    return 0; /* no drift tracking: window is one wide, nothing to shift */
  }
  const int best_drift_index = dt_trellis_frontier(ctx, tr) % ctx->drift_width;
  const int drift = best_drift_index - ctx->max_drift;
  const int deadband = (ctx->max_drift + 1) / 2;
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
static void align_fill_into(const dt_decode_ctx *ctx, const uint8_t *expected,
                            int base, double *final_out) {
  const int n = ctx->n, max_consume = ctx->n + 2 * ctx->max_drift,
            stride = max_consume + 1;
  /* The per-bit match cost and in-range gate for each received position are
   * precomputed once per step (fill_match_costs) and shared by every alignment;
   * `off` maps this edge's read base into that table. The arithmetic is identical
   * to deriving them per cell. Rows 0..n-1 use ctx->alignment as scratch; the
   * final row (row n - the branch cost by total consumption) is written straight
   * into final_out, which the fused decoder shares across codes by pattern. */
  const int off = base - ctx->match_lo;
  const signed char *in_range = ctx->in_range;
  const double *match_cost0 = ctx->match_cost0, *match_cost1 = ctx->match_cost1;
  const double *ins_cost = ctx->ins_cost;
  const double cost_del = ctx->cost_del;
  double *scratch = ctx->alignment;

  scratch[0] = 0.0;
  for (int consumed = 1; consumed <= max_consume; ++consumed) {
    scratch[consumed] = in_range[off + consumed - 1]
                            ? scratch[consumed - 1] + ins_cost[off + consumed - 1]
                            : INFINITY;
  }
  for (int j = 1; j <= n; ++j) {
    double *cost_row = (j == n) ? final_out : scratch + (size_t)j * stride;
    const double *prev_row = scratch + (size_t)(j - 1) * stride;
    const double *match_cost = expected[j - 1] ? match_cost1 : match_cost0;
    cost_row[0] =
        prev_row[0] + cost_del; /* delete expected[j-1], consume nothing */
    for (int consumed = 1; consumed <= max_consume; ++consumed) {
      double best = prev_row[consumed] + cost_del; /* deletion always avail. */
      const int position = off + consumed - 1;
      if (in_range[position]) {
        const double align_cost =
            prev_row[consumed - 1] + match_cost[position]; /* match / sub */
        const double insert_cost =
            cost_row[consumed - 1] + ins_cost[position]; /* extra received bit */
        if (align_cost < best) best = align_cost;
        if (insert_cost < best) best = insert_cost;
      }
      cost_row[consumed] = best;
    }
  }
}

/* Precompute the step's received-window match costs once, shared by every
 * align_fill this step (they all read the same buffer with the same channel
 * cost constants). Position p maps to absolute received index match_lo + p;
 * match_cost{0,1}[p] is the cost of aligning an expected 0/1 bit there (an erased
 * bit costs cost_erase either way), and in_range[p] gates positions off the ends
 * of the buffer (where only deletion is available - see align_fill). The window
 * spans every (source drift, consumed) an edge can touch: n + 4*max_drift wide. */
static void fill_match_costs(dt_decode_ctx *ctx) {
  const int window = ctx->n + 4 * ctx->max_drift;
  const double keep = ctx->cost_keep, erase = ctx->cost_erase;
  ctx->match_lo = ctx->read_base - ctx->max_drift;
  for (int p = 0; p < window; ++p) {
    const int absolute = ctx->match_lo + p;
    if (absolute >= 0 && absolute < ctx->received_length) {
      const uint8_t received_bit = ctx->received[absolute];
      if (received_bit == DT_ERASURE) {
        ctx->match_cost0[p] = keep + erase;
        ctx->match_cost1[p] = keep + erase;
        ctx->ins_cost[p] = ctx->cost_ins_e;
      } else {
        ctx->match_cost0[p] = keep + ctx->cost_bit[0][received_bit];
        ctx->match_cost1[p] = keep + ctx->cost_bit[1][received_bit];
        ctx->ins_cost[p] = received_bit ? ctx->cost_ins_t : ctx->cost_ins_f;
      }
      ctx->in_range[p] = 1;
    } else {
      ctx->in_range[p] = 0;
    }
  }
}

/* Subtract the lowest node cost from every node, so the best one sits at 0.
 * Over an unbounded stream this keeps the costs from growing without limit.
 * Returns the amount subtracted: the best path's cost increment this step. */
static double normalize(double *metric, size_t count) {
  double lowest = INFINITY;
  for (size_t i = 0; i < count; ++i) {
    if (metric[i] < lowest) {
      lowest = metric[i];
    }
  }
  if (lowest == INFINITY) {
    return 0.0;
  }
  if (lowest > 0.0) {
    for (size_t i = 0; i < count; ++i) {
      if (metric[i] != INFINITY) {
        metric[i] -= lowest;
      }
    }
  }
  return lowest;
}

/* Shift one trellis's drift window by `sigma` (each node's drift index
 * drift_index -> drift_index - sigma); the read cursor is advanced once, by
 * dt_decode_step, to match. Nodes shifted out of the window are dropped. Uses
 * next_metric as scratch.
 *
 * The dropped edge slot is set to INFINITY (sigma > 0 empties the top slot,
 * sigma < 0 the bottom). forward_pass then skips those slots as sources, so the
 * emptied edge can never be recorded as a backpointer's prev_drift_index - which
 * is exactly what keeps dt_trellis_trace's `prev_drift_index + sigma` inside
 * [0, drift_width) when it steps back across this re-anchor. */
static void reanchor_metric(const dt_decode_ctx *ctx, dt_trellis *tr,
                            int sigma) {
  const int num_states = ctx->num_states, drift_width = ctx->drift_width;
  for (int state = 0; state < num_states; ++state) {
    for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
      const int source = drift_index + sigma;
      tr->next_metric[node_at(state, drift_index, drift_width)] =
          (source >= 0 && source < drift_width)
              ? tr->metric[node_at(state, source, drift_width)]
              : INFINITY;
    }
  }
  dt_memcpy(tr->metric, tr->next_metric,
         (size_t)num_states * drift_width * sizeof(double));
}

/* One trellis's forward pass for the current step: run the add-compare-select
 * from tr->metric into tr->next_metric (recording backpointers in this step's
 * layer), normalise, update the smoothed cost, and swap. Reads ctx->read_base
 * (already re-anchored for this step) and the shared received buffer. Branches
 * that would read outside the buffered window are skipped. */
/* Compute, once per fused step, the per-(pattern, source drift) alignment rows
 * that every trellis's scatter then reads. Alignment depends only on the output
 * pattern and drift - not the code - so the fused decoder computes it once over
 * the union of the codes' patterns instead of once per code. A drift is computed
 * only when some fused trellis has a live node there (union liveness); dead-drift
 * rows are never read by the scatter (it skips dead nodes). */
static void align_precompute(dt_decode_ctx *ctx, const dt_trellis *trs,
                             size_t n_tr) {
  const int n = ctx->n, max_drift = ctx->max_drift, num_states = ctx->num_states,
            drift_width = ctx->drift_width;
  const int stride = ctx->n + 2 * ctx->max_drift + 1;

  fill_match_costs(ctx);

  for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
    int live = 0;
    for (size_t t = 0; t < n_tr && !live; ++t) {
      const double *metric = trs[t].metric;
      for (int state = 0; state < num_states; ++state) {
        if (metric[(size_t)state * drift_width + drift_index] != INFINITY) {
          live = 1;
          break;
        }
      }
    }
    if (!live) {
      continue;
    }
    const int base = ctx->read_base + (drift_index - max_drift);
    for (int p = 0; p < ctx->n_patterns; ++p) {
      align_fill_into(
          ctx, &ctx->pattern_bits[(size_t)p * n], base,
          ctx->align_shared + ((size_t)p * drift_width + drift_index) * stride);
    }
  }
}

/* One trellis's forward pass: scatter the shared alignment rows (align_precompute,
 * already run this step) from tr->metric into tr->next_metric, recording this
 * step's backpointers, then normalise, update the smoothed cost, and swap. The
 * node/edge order (state, drift, bit, ending drift) matches the per-edge form, so
 * the `<` tie-break - and the decoded bits - are identical. */
static void forward_pass(dt_decode_ctx *ctx, dt_trellis *tr) {
  const dt_code *code = tr->code;
  const int n = ctx->n, num_states = ctx->num_states,
            drift_width = ctx->drift_width;
  const size_t count = (size_t)num_states * drift_width;
  const int stride = ctx->n + 2 * ctx->max_drift + 1;

  for (size_t i = 0; i < count; ++i) {
    tr->next_metric[i] = INFINITY;
  }
  dt_backpointer *layer =
      tr->backpointers + (size_t)(ctx->steps % ctx->ring_len) * count;

  for (int state = 0; state < num_states; ++state) {
    const size_t source_row = (size_t)state * drift_width;
    for (int drift_index = 0; drift_index < drift_width; ++drift_index) {
      const double current_cost = tr->metric[source_row + drift_index];
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
        const double *final_row =
            ctx->align_shared +
            ((size_t)tr->group_of[edge] * drift_width + drift_index) * stride;
        const size_t dest_row = (size_t)next_state * drift_width;
        for (int next_drift_index = first; next_drift_index < drift_width;
             ++next_drift_index) {
          const double branch_cost =
              final_row[n + (next_drift_index - drift_index)];
          if (branch_cost == INFINITY) {
            continue;
          }
          const double cost = current_cost + branch_cost;
          const size_t destination = dest_row + next_drift_index;
          if (cost < tr->next_metric[destination]) {
            tr->next_metric[destination] = cost;
            layer[destination] =
                dt_bp_pack(state, drift_index, (unsigned int)bit);
          }
        }
      }
    }
  }

  const double increment = normalize(tr->next_metric, count);
  const double alpha = 2.0 / (ctx->decision_depth + 1.0);
  tr->smoothed_cost += alpha * (increment - tr->smoothed_cost);

  double *temp = tr->metric;
  tr->metric = tr->next_metric;
  tr->next_metric = temp;
}

/* Fast path for max_drift == 0 with indels disabled (all insertion costs and
 * cost_del == +inf, i.e. p_ins == p_del == 0). The drift window is one wide and the
 * alignment is forced diagonal, so an edge's cost is just the sum of its n
 * per-bit match costs - a plain Viterbi butterfly with no alignment DP and no
 * drift loops. This yields exactly the general path's branch cost (final_row[n])
 * in the same (state, bit) order, so output is identical. */
static void forward_pass_nodrift(dt_decode_ctx *ctx, dt_trellis *tr) {
  const dt_code *code = tr->code;
  const int n = ctx->n, num_states = ctx->num_states;
  const int base = ctx->read_base;
  const int in_range = base >= 0 && base + n <= ctx->received_length;
  const double keep_total = (double)n * ctx->cost_keep;

  for (int state = 0; state < num_states; ++state) {
    tr->next_metric[state] = INFINITY;
  }
  dt_backpointer *layer =
      tr->backpointers +
      (size_t)(ctx->steps % ctx->ring_len) * (size_t)num_states;

  for (int state = 0; state < num_states; ++state) {
    const double current_cost = tr->metric[state];
    if (current_cost == INFINITY) {
      continue;
    }
    for (int bit = 0; bit <= 1; ++bit) {
      const int edge = state * 2 + bit;
      double branch_cost = INFINITY;
      if (in_range) {
        const uint8_t *expected = &code->output[(size_t)edge * n];
        branch_cost = keep_total;
        for (int j = 0; j < n; ++j) {
          const uint8_t received_bit = ctx->received[base + j];
          branch_cost += received_bit == DT_ERASURE
                             ? ctx->cost_erase
                             : ctx->cost_bit[expected[j]][received_bit];
        }
      }
      if (branch_cost == INFINITY) {
        continue;
      }
      const double cost = current_cost + branch_cost;
      const int next_state = code->next_state[edge];
      if (cost < tr->next_metric[next_state]) {
        tr->next_metric[next_state] = cost;
        layer[next_state] = dt_bp_pack(state, 0, (unsigned int)bit);
      }
    }
  }

  const double increment = normalize(tr->next_metric, (size_t)num_states);
  const double alpha = 2.0 / (ctx->decision_depth + 1.0);
  tr->smoothed_cost += alpha * (increment - tr->smoothed_cost);

  double *temp = tr->metric;
  tr->metric = tr->next_metric;
  tr->next_metric = temp;
}

/* Snapshot this step's forward metrics (alpha) and branch costs into the BCJR
 * rings before the forward pass overwrites them, so a later decision can run the
 * backward recursion (dt_trellis_soft_batch) over the retained window. alpha is each
 * trellis's metric in this step's re-anchored frame (the input to forward_pass);
 * the branch costs are the shared per-(pattern, drift) rows align_precompute just
 * produced (drift path), or the per-pattern group costs the nodrift butterfly
 * uses, computed once here for all fused trellises. */
static void snapshot_step(dt_decode_ctx *ctx, dt_trellis *trs, size_t n,
                          int nodrift) {
  const int stride = ctx->n + 2 * ctx->max_drift + 1;
  const size_t shared = (size_t)ctx->n_patterns * ctx->drift_width * stride;
  const size_t slot = (size_t)(ctx->steps % ctx->ring_len);
  const size_t count = (size_t)ctx->num_states * ctx->drift_width;

  for (size_t j = 0; j < n; ++j) {
    dt_memcpy(trs[j].alpha_ring + slot * count, trs[j].metric,
              count * sizeof(double));
  }

  double *bslot = ctx->branch_ring + slot * shared;
  if (nodrift) {
    /* drift_width == 1, stride == n + 1; branch for pattern p sits at row p's
     * final cell (the n-consumed entry the backward pass reads). */
    const int base = ctx->read_base, nn = ctx->n;
    const int in_range = base >= 0 && base + nn <= ctx->received_length;
    const double keep_total = (double)nn * ctx->cost_keep;
    for (int p = 0; p < ctx->n_patterns; ++p) {
      double branch = INFINITY;
      if (in_range) {
        const uint8_t *pat = &ctx->pattern_bits[(size_t)p * nn];
        branch = keep_total;
        for (int jj = 0; jj < nn; ++jj) {
          const uint8_t rb = ctx->received[base + jj];
          branch += rb == DT_ERASURE ? ctx->cost_erase
                                     : ctx->cost_bit[pat[jj]][rb];
        }
      }
      bslot[(size_t)p * stride + nn] = branch;
    }
  } else {
    dt_memcpy(bslot, ctx->align_shared, shared * sizeof(double));
  }
}

/* Advance every trellis one fused step. The re-anchor sigma is decided once -
 * from the best-fitting trellis (lowest smoothed cost), so a non-matching code
 * never steers the shared timing - and applied to every trellis's window plus
 * the one shared read cursor, after which each runs its own forward pass. With
 * a single trellis this is the ordinary per-code step. */
void dt_decode_step(dt_decode_ctx *ctx, dt_trellis *trs, size_t n) {
  size_t lead = 0;
  for (size_t j = 1; j < n; ++j) {
    if (trs[j].smoothed_cost < trs[lead].smoothed_cost) {
      lead = j;
    }
  }

  const int sigma = pick_shift(ctx, &trs[lead]);
  if (sigma != 0) {
    for (size_t j = 0; j < n; ++j) {
      reanchor_metric(ctx, &trs[j], sigma);
    }
    ctx->read_base += sigma;
  }
  ctx->shift[ctx->steps % ctx->ring_len] = sigma;

  /* With no drift window and indels disabled the alignment is forced diagonal;
   * a dedicated butterfly skips the DP entirely (identical result). Otherwise the
   * per-(pattern, drift) alignment rows are computed once for all fused trellises,
   * then each scatters them - so decoding N codes runs the DP once, not N times. */
  const int nodrift = ctx->max_drift == 0 && ctx->cost_ins_t == INFINITY &&
                      ctx->cost_ins_f == INFINITY && ctx->cost_ins_e == INFINITY &&
                      ctx->cost_del == INFINITY;
  if (!nodrift) {
    align_precompute(ctx, trs, n);
  }
  snapshot_step(ctx, trs, n, nodrift); /* before forward_pass overwrites metric */
  if (nodrift) {
    for (size_t j = 0; j < n; ++j) {
      forward_pass_nodrift(ctx, &trs[j]);
    }
  } else {
    for (size_t j = 0; j < n; ++j) {
      forward_pass(ctx, &trs[j]);
    }
  }

  ctx->steps++;
  ctx->read_base += ctx->n;
  /* Snapshot each trellis's smoothed cost and best frontier node at the new
   * frontier, so a batched decision reads the lock and traces the vote bit at
   * the bit's own (earlier) decision time. */
  for (size_t j = 0; j < n; ++j) {
    trs[j].smoothed_ring[ctx->steps % ctx->ring_len] = trs[j].smoothed_cost;
    trs[j].frontier_ring[ctx->steps % ctx->ring_len] =
        dt_trellis_frontier(ctx, &trs[j]);
  }
  if (ctx->read_base - ctx->max_drift >= ctx->received_capacity / 2) {
    compact_received(ctx);
  }
}

/* Walk the backpointers from frontier node `frontier` back to step `target` and
 * return the input bit decided there. Each step's backpointer is stored in that
 * step's drift frame, so when stepping back across a re-anchor we shift the
 * predecessor's drift index by that step's recorded sigma. */
unsigned char dt_trellis_trace(const dt_decode_ctx *ctx, const dt_trellis *tr,
                               long long from_step, int frontier,
                               long long target) {
  const size_t count = (size_t)ctx->num_states * ctx->drift_width;
  /* Traceback only reaches back through retained ring layers; a deeper target
   * would read a backpointer/shift slot the next step has already overwritten.
   * The batched multi vote traces up to a full ring_len behind the frontier. */
  DT_ASSERT(from_step - target <= ctx->ring_len);
  int node = frontier;
  unsigned char bit = 0;
  for (long long i = from_step - 1; i >= target; --i) {
    const dt_backpointer *layer =
        tr->backpointers + (size_t)(i % ctx->ring_len) * count;
    const dt_backpointer entry = layer[node];
    if (i == target) {
      bit = (unsigned char)dt_bp_bit(entry);
      break;
    }
    const int prev_drift_index =
        dt_bp_drift(entry) + ctx->shift[i % ctx->ring_len];
    /* In-bounds by construction: reanchor_metric empties the slot that would
     * translate out of the window, so it is never a recorded predecessor. */
    DT_ASSERT(prev_drift_index >= 0 && prev_drift_index < ctx->drift_width);
    node = dt_bp_state(entry) * ctx->drift_width + prev_drift_index;
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
double dt_lock_from_cost(const dt_decode_ctx *ctx, double cost) {
  const double gap = ctx->expected_unlock - ctx->expected_lock;
  if (gap <= 0.0) {
    return 0.0;
  }
  double probability = (ctx->expected_unlock - cost) / gap;
  if (probability < 0.0) {
    probability = 0.0;
  } else if (probability > 1.0) {
    probability = 1.0;
  }
  return probability;
}

/* Branch cost of edge `edge`, source drift `di`, into ending drift `nd`, read
 * from a step's snapshotted branch row (align_shared layout): final_row[n + (nd -
 * di)] - the same value forward_pass scatters. INFINITY if that ending drift is
 * infeasible for this edge. */
static double branch_cost(const dt_decode_ctx *ctx, const dt_trellis *tr,
                          const double *bslot, int edge, int di, int nd) {
  const int stride = ctx->n + 2 * ctx->max_drift + 1;
  const double *final_row =
      bslot + ((size_t)tr->group_of[edge] * ctx->drift_width + di) * stride;
  return final_row[ctx->n + (nd - di)];
}

/* Map a target's (m0, m1) - the costs of the best complete path forced to each
 * value - and the lock-time smoothed cost onto the four consistencies, filling
 * *out (when non-NULL) and returning the decoded value (the most consistent of
 * true/false/lost).
 *
 * The consistencies are goodness-of-fit of three propositions, each in [0,1] and
 * NOT a probability split (they need not sum to 1). c_true / c_false ask "could
 * this bit have been true / false?" - exp(-(m_b - mmin)), how well the cheapest
 * path forced to that value fits relative to the unconstrained best. The winning
 * value pays no penalty and is fully consistent (1); the loser is discounted by
 * its reliability gap. A clean bit forces several codeword mismatches to flip, so
 * its loser ~0; when the evidence is absent (a sustained erasure/deletion burst)
 * flipping is free, the gap ~0, and BOTH values stay fully consistent - so c_true
 * and c_false both approach 1.
 *
 * c_lost is the consistency that the bit's information was lost - that the data
 * cannot tell true from false. It is the agreement of the other two,
 * 1 - |c_true - c_false|: high whenever the values are indistinguishable,
 * whether the evidence is absent (both ~1) or the decoder has lost lock on random
 * data (m0 ~ m1, so both consistencies match). It is deliberately NOT scaled by
 * c_lock, so it stays high through a lock collapse. A bit with only one feasible
 * value is determined, not lost (the winner is 1 and the infeasible side is 0, so
 * the agreement is 0).
 *
 * c_lock is the independent lock consistency (is this the right code at all?). */
static uint8_t finalize_soft(const dt_decode_ctx *ctx, double m0, double m1,
                             double smoothed, dt_decode_details *out) {
  const double mmin = m0 < m1 ? m0 : m1;
  const double c_true = m1 == INFINITY ? 0.0 : dt_exp(mmin - m1);
  const double c_false = m0 == INFINITY ? 0.0 : dt_exp(mmin - m0);
  const double diff = c_true - c_false;
  const double c_lost = 1.0 - (diff < 0.0 ? -diff : diff);
  if (out) {
    out->c_true = c_true;
    out->c_false = c_false;
    out->c_lost = c_lost;
    out->c_lock = dt_lock_from_cost(ctx, smoothed);
  }
  /* Most consistent of the three. c_lost is the agreement of the other two, so it
   * only leads on a tie (m0 == m1 - genuinely undeterminable), abstaining. */
  if (c_lost >= c_true && c_lost >= c_false) {
    return DT_ERASURE;
  }
  return c_true >= c_false ? DT_TRUE : DT_FALSE;
}

/* Core soft-output routine: forward-backward (BCJR) min-sum over the retained
 * window, producing soft output for the n targets [base, base+n) from ONE
 * backward sweep. bits_out[k] / details_out[k] (either may be NULL) receive
 * target base+k.
 *
 * For the bit at step t we want m_b = cost of the best COMPLETE path that takes
 * input bit b there - not just whatever survived to the frontier (compare-select
 * discards the losing hypothesis, which is exactly what must be recovered when
 * the evidence is absent). Combine the snapshotted forward metric, branch cost,
 * and a backward metric beta:
 *
 *   m_b = min over edges with input b of alpha[t] + branch(t) + beta[t+1]
 *   beta[steps][v] = 0 where the frontier alpha is finite, else INF
 *   beta[t][src]   = min over bit, ending drift of branch(t) + beta[t+1][dest]
 *
 * One sweep from the frontier down to base yields beta[t+1] for every t in the
 * window, so all n targets are combined as the sweep passes them - the warmup
 * steps above base+n converge beta for the whole batch (a path may end at any
 * reachable frontier node, so beta[steps]=0). Per-step re-anchoring shifts the
 * drift frame; a dest produced at step t (frame t = frame t+1 pre-anchor) indexes
 * beta[t+1] (stored post-anchor) at nd - sigma, sigma = the step t+1 shift (0 at
 * the frontier) - the mirror of dt_trellis_trace's `+ shift`. Only finite-alpha
 * nodes are visited; any edge out of a reachable node lands on a reachable node,
 * so the restriction is exact. Per-step normalisation subtracts a path-independent
 * constant from every alpha, cancelling in the m0/m1 differences. */
void dt_trellis_soft_batch(const dt_decode_ctx *ctx, const dt_trellis *tr,
                           long long base, int n, uint8_t *bits_out,
                           dt_decode_details *details_out, int detail_stride) {
  const dt_code *code = tr->code;
  const int nn = ctx->n, num_states = ctx->num_states,
            drift_width = ctx->drift_width, rl = ctx->ring_len,
            dd = ctx->decision_depth;
  const size_t count = (size_t)num_states * drift_width;
  const size_t shared = (size_t)ctx->n_patterns * drift_width *
                        (size_t)(nn + 2 * ctx->max_drift + 1);
  DT_ASSERT(ctx->steps - base <= rl);

  double *beta_next = ctx->beta_a, *beta_cur = ctx->beta_b;
  for (size_t node = 0; node < count; ++node) {
    beta_next[node] = tr->metric[node] == INFINITY ? INFINITY : 0.0;
  }

  for (long long t = ctx->steps - 1; t >= base; --t) {
    const int in_emit = t < base + n;
    const int need_beta = t > base; /* beta[base] is never consumed */
    const double *alpha_t = tr->alpha_ring + (size_t)(t % rl) * count;
    const double *bslot = ctx->branch_ring + (size_t)(t % rl) * shared;
    const int sigma = (t + 1 == ctx->steps) ? 0 : ctx->shift[(t + 1) % rl];
    if (need_beta) {
      for (size_t node = 0; node < count; ++node) beta_cur[node] = INFINITY;
    }
    double m0 = INFINITY, m1 = INFINITY;
    for (int state = 0; state < num_states; ++state) {
      for (int di = 0; di < drift_width; ++di) {
        const double a = alpha_t[(size_t)state * drift_width + di];
        if (a == INFINITY) {
          continue;
        }
        int first = di - nn;
        if (first < 0) first = 0;
        double beta_src = INFINITY;
        for (int bit = 0; bit <= 1; ++bit) {
          const int edge = state * 2 + bit;
          const size_t drow = (size_t)code->next_state[edge] * drift_width;
          double best_edge = INFINITY;
          for (int nd = first; nd < drift_width; ++nd) {
            const double bc = branch_cost(ctx, tr, bslot, edge, di, nd);
            const int dest = nd - sigma;
            if (bc == INFINITY || dest < 0 || dest >= drift_width) {
              continue;
            }
            const double cand = bc + beta_next[drow + dest];
            if (cand < best_edge) best_edge = cand;
          }
          if (best_edge < beta_src) beta_src = best_edge;
          if (in_emit && best_edge != INFINITY) {
            const double total = a + best_edge;
            if (bit) {
              if (total < m1) m1 = total;
            } else {
              if (total < m0) m0 = total;
            }
          }
        }
        if (need_beta) beta_cur[(size_t)state * drift_width + di] = beta_src;
      }
    }
    if (in_emit) {
      const int k = (int)(t - base);
      /* Lock at this target's own decision time (frontier t+decision_depth),
       * clamped to the current frontier for the reduced-depth flush tail. */
      long long s = t + dd;
      if (s > ctx->steps) s = ctx->steps;
      const uint8_t bit = finalize_soft(
          ctx, m0, m1, tr->smoothed_ring[s % rl],
          details_out ? &details_out[(size_t)k * detail_stride] : NULL);
      if (bits_out) bits_out[k] = bit;
    }
    if (need_beta) {
      double *tmp = beta_next;
      beta_next = beta_cur;
      beta_cur = tmp;
    }
  }
}

/* Process steps for one trellis, emitting decided bits until input/output limits
 * hit. Bits are emitted in batches: step forward until ring_len bits past the
 * last decision are buffered (or the input/output limit stops us), then one
 * backward sweep (trellis_soft_batch) serves the whole batch - amortising the
 * BCJR backward pass to ~2 steps per bit instead of a full window per bit. Each
 * emitted bit's value goes to out[] and detail to details[] (either may be NULL;
 * with both NULL the bits are still consumed). `draining` relaxes the look-ahead
 * and emits the reduced-depth tail at end-of-stream. */
static int run(dt_decode_ctx *ctx, dt_trellis *tr, uint8_t *out,
               dt_decode_details *details, int max_out, int draining) {
  const int dd = ctx->decision_depth;
  const long long cap_window = 2 * (long long)dd; /* max buffered ahead of decided */
  int output_count = 0;
  for (;;) {
    /* Accumulate forward steps until a batch is buffered or we cannot step. */
    while (ctx->steps - ctx->decided < cap_window) {
      if (!draining) {
        /* +1 of slack covers a re-anchor stepping the cursor forward. */
        if (ctx->received_length <
            ctx->read_base + ctx->n + ctx->max_drift + 1) {
          break;
        }
      } else if (ctx->received_length - ctx->read_base < ctx->n) {
        break;
      }
      dt_decode_step(ctx, tr, 1);
    }

    /* Decidable targets: up to steps - decision_depth normally (each keeps its
     * full look-ahead), or all buffered steps when draining the tail. */
    const long long horizon = draining ? ctx->steps : (ctx->steps - dd);
    long long avail = horizon - ctx->decided;
    if (avail > max_out - output_count) {
      avail = max_out - output_count;
    }
    if (avail <= 0) {
      break;
    }
    const int n_emit = (int)avail;
    if (out || details) {
      dt_trellis_soft_batch(ctx, tr, ctx->decided, n_emit,
                            out ? out + output_count : NULL,
                            details ? details + output_count : NULL, 1);
    }
    output_count += n_emit;
    ctx->decided += n_emit;
    if (output_count >= max_out) {
      break;
    }
  }
  return output_count;
}

/* Initialise a trellis's metric for blind acquisition: every encoder state is
 * equally likely (all at zero drift), so the decoder locks on whether it starts
 * at the stream's beginning or is tapped partway through. */
static void init_metric(const dt_decode_ctx *ctx, dt_trellis *tr) {
  const size_t count = (size_t)ctx->num_states * ctx->drift_width;
  for (size_t i = 0; i < count; ++i) {
    tr->metric[i] = INFINITY;
  }
  for (int state = 0; state < ctx->num_states; ++state) {
    tr->metric[node_at(state, ctx->max_drift, ctx->drift_width)] =
        0.0; /* zero drift, cost 0 */
  }
}

/* -- internal context / trellis lifecycle ---------------------------------- */

int dt_decode_ctx_init(dt_decode_ctx *ctx, const dt_stream_params *params,
                       const dt_code *code) {
  if (!ctx || !params || !code) {
    return DT_ERR_ARG;
  }
  const int decision_depth = params->decision_depth;
  const int max_drift = params->max_drift;
  const double p_flip = params->p_flip;
  const double p_ins_true = params->p_ins_true;
  const double p_ins_false = params->p_ins_false;
  const double p_ins_erase = params->p_ins_erase;
  const double p_del = params->p_del;
  const double p_ovr_true = params->p_ovr_true;
  const double p_ovr_false = params->p_ovr_false;
  const double p_ovr_erase = params->p_ovr_erase;
  const double p_ins = p_ins_true + p_ins_false + p_ins_erase;
  const double p_ovr = p_ovr_true + p_ovr_false + p_ovr_erase;

  if (decision_depth < 1 || max_drift < 0) {
    return DT_ERR_ARG;
  }
  if (!(p_flip > 0.0 && p_flip < 1.0) || p_ins_true < 0.0 ||
      p_ins_false < 0.0 || p_ins_erase < 0.0 || p_del < 0.0 ||
      p_ovr_true < 0.0 || p_ovr_false < 0.0 || p_ovr_erase < 0.0 ||
      !(p_ovr < 1.0) || !(p_ins + p_del < 1.0)) {
    return DT_ERR_ARG;
  }
  /* Insertion/deletion probabilities are only consulted when tracking drift;
   * with max_drift == 0 they may be left 0 (correct flips only). */
  if (max_drift > 0 && (p_ins <= 0.0 || p_del <= 0.0)) {
    return DT_ERR_ARG;
  }

  ctx->n = code->n;
  ctx->max_drift = max_drift;
  ctx->num_states = code->n_states;
  ctx->drift_width = 2 * max_drift + 1;
  ctx->decision_depth = decision_depth;
  /* Batched soft output buffers up to one batch (= decision_depth) past the
   * full-look-ahead horizon, so the snapshot rings span 2*decision_depth steps
   * (+ slack so the oldest needed slot is never aliased by the newest). */
  ctx->ring_len = 2 * decision_depth + 2;

  /* Channel model: a coded bit is overwritten - with a fixed DT_TRUE, DT_FALSE
   * or DT_ERASURE (probs p_ovr_true / p_ovr_false / p_ovr_erase, regardless of
   * what was sent) - else transmitted (prob pn = 1 - p_ovr) and flipped with
   * prob p_flip. So receiving value r against an expected/sent bit b mixes the
   * overwrite floor with the normal channel; cost_bit[b][r] is its -log. The
   * overwrite-to-erasure rate p_ovr_erase doubles as the plain erasure rate. */
  const double pn = 1.0 - p_ovr;
  ctx->cost_bit[0][0] = -dt_log(p_ovr_false + pn * (1.0 - p_flip));
  ctx->cost_bit[0][1] = -dt_log(p_ovr_true + pn * p_flip);
  ctx->cost_bit[1][0] = -dt_log(p_ovr_false + pn * p_flip);
  ctx->cost_bit[1][1] = -dt_log(p_ovr_true + pn * (1.0 - p_flip));
  ctx->cost_erase = -dt_log(p_ovr_erase); /* +inf when 0 (never read) */
  ctx->cost_keep = -dt_log(1.0 - p_ins - p_del);
  /* Cost to consume a received bit as an insertion. The total insertion rate
   * (p_ins_true + p_ins_false + p_ins_erase) sets how readily the decoder
   * realigns; the per-value rates only bias which value it expects an inserted
   * bit to carry. A data insertion's value is scored against a uniform 0/1 prior
   * (the 2x), so an evenly-split rate gives -log(p_ins) - the same eagerness as a
   * single combined rate, only tilting when the true/false rates differ. An
   * erasure carries no value to score, so it is taken at its own rate. */
  ctx->cost_ins_t = -dt_log(2.0 * p_ins_true);  /* consume a received 1 */
  ctx->cost_ins_f = -dt_log(2.0 * p_ins_false); /* consume a received 0 */
  ctx->cost_ins_e = -dt_log(p_ins_erase);       /* consume an erasure   */
  ctx->cost_del = -dt_log(p_del);

  /* Lock anchors (per step = n coded bits). A "kept" coded bit costs cost_keep
   * plus a match/miss term; with asymmetric overwrites we average the per-bit
   * match and miss costs over the two sent values. The expected misfit fraction
   * when locked is the model's miss rate among non-erased bits (= p_flip with no
   * overwrites), and we call it "unlocked" once misfit reaches the midpoint
   * between that and 0.5 (random). Erased bits contribute cost_erase to both. */
  const double avg_match =
      0.5 * (ctx->cost_bit[0][0] + ctx->cost_bit[1][1]);
  const double avg_miss = 0.5 * (ctx->cost_bit[0][1] + ctx->cost_bit[1][0]);
  const double misfit_lock =
      (pn * p_flip + 0.5 * (p_ovr_true + p_ovr_false)) / (1.0 - p_ovr_erase);
  const double misfit_unlock = 0.5 * (misfit_lock + 0.5);
  const double erase_term =
      p_ovr_erase > 0.0 ? p_ovr_erase * ctx->cost_erase : 0.0;
  const double kept = 1.0 - p_ovr_erase;
  ctx->expected_lock =
      ctx->n * (ctx->cost_keep + erase_term +
                kept * ((1.0 - misfit_lock) * avg_match +
                        misfit_lock * avg_miss));
  ctx->expected_unlock =
      ctx->n * (ctx->cost_keep + erase_term +
                kept * ((1.0 - misfit_unlock) * avg_match +
                        misfit_unlock * avg_miss));

  ctx->steps = 0;
  ctx->decided = 0;
  ctx->read_base = 0;
  ctx->received_length = 0;
  ctx->received_capacity =
      (ctx->ring_len + 2) * ctx->n + 8 * ctx->max_drift + 64;

  /* Shared per-step alignment scratch. align_shared is sized later, once every
   * fused trellis has registered its output patterns (dt_decode_ctx_finalize),
   * so the union of patterns is known; the pattern list starts empty and grows
   * as trellises register (one code's edge count is a fine initial capacity). */
  const int max_consume = ctx->n + 2 * ctx->max_drift;
  const int window = ctx->n + 4 * ctx->max_drift;
  ctx->n_patterns = 0;
  ctx->pattern_cap = ctx->num_states * 2;
  ctx->align_shared = NULL;

  const size_t node_count = (size_t)ctx->num_states * ctx->drift_width;
  ctx->shift = dt_malloc((size_t)ctx->ring_len * sizeof(int));
  ctx->received = dt_malloc((size_t)ctx->received_capacity);
  ctx->pattern_bits = dt_malloc((size_t)ctx->pattern_cap * ctx->n);
  ctx->alignment =
      dt_malloc((size_t)(ctx->n + 1) * (max_consume + 1) * sizeof(double));
  ctx->match_cost0 = dt_malloc((size_t)window * sizeof(double));
  ctx->match_cost1 = dt_malloc((size_t)window * sizeof(double));
  ctx->ins_cost = dt_malloc((size_t)window * sizeof(double));
  ctx->in_range = dt_malloc((size_t)window * sizeof(signed char));
  ctx->beta_a = dt_malloc(node_count * sizeof(double));
  ctx->beta_b = dt_malloc(node_count * sizeof(double));
  if (!ctx->shift || !ctx->received || !ctx->pattern_bits || !ctx->alignment ||
      !ctx->match_cost0 || !ctx->match_cost1 || !ctx->ins_cost ||
      !ctx->in_range || !ctx->beta_a || !ctx->beta_b) {
    dt_decode_ctx_free(ctx);
    return DT_ERR_ALLOC;
  }
  return DT_OK;
}

/* Allocate the shared alignment table now that the union of every fused trellis's
 * output patterns is known. Call once after the last dt_trellis_init. */
int dt_decode_ctx_finalize(dt_decode_ctx *ctx) {
  if (!ctx) {
    return DT_ERR_ARG;
  }
  const int stride = ctx->n + 2 * ctx->max_drift + 1;
  const int patterns = ctx->n_patterns > 0 ? ctx->n_patterns : 1;
  const size_t shared = (size_t)patterns * ctx->drift_width * (size_t)stride;
  dt_free(ctx->align_shared);
  dt_free(ctx->branch_ring);
  ctx->align_shared = dt_malloc(shared * sizeof(double));
  /* One align_shared-sized branch snapshot per retained step (BCJR backward). */
  ctx->branch_ring =
      dt_malloc((size_t)ctx->ring_len * shared * sizeof(double));
  return ctx->align_shared && ctx->branch_ring ? DT_OK : DT_ERR_ALLOC;
}

void dt_decode_ctx_free(dt_decode_ctx *ctx) {
  if (!ctx) {
    return;
  }
  dt_free(ctx->shift);
  dt_free(ctx->received);
  dt_free(ctx->pattern_bits);
  dt_free(ctx->alignment);
  dt_free(ctx->align_shared);
  dt_free(ctx->match_cost0);
  dt_free(ctx->match_cost1);
  dt_free(ctx->ins_cost);
  dt_free(ctx->in_range);
  dt_free(ctx->branch_ring);
  dt_free(ctx->beta_a);
  dt_free(ctx->beta_b);
  ctx->shift = NULL;
  ctx->received = NULL;
  ctx->pattern_bits = NULL;
  ctx->alignment = NULL;
  ctx->align_shared = NULL;
  ctx->match_cost0 = NULL;
  ctx->match_cost1 = NULL;
  ctx->ins_cost = NULL;
  ctx->in_range = NULL;
  ctx->branch_ring = NULL;
  ctx->beta_a = NULL;
  ctx->beta_b = NULL;
}

/* Register a code's output patterns into the shared ctx (deduping against those
 * already there) and fill group_of[edge] with each edge's pattern index. The
 * fused decoder thus builds one union pattern list across all its codes, so the
 * per-step alignment is computed once per distinct pattern, not once per code. */
static int register_patterns(dt_decode_ctx *ctx, const dt_code *code,
                             int *group_of) {
  const int n = ctx->n, edges = ctx->num_states * 2;
  for (int edge = 0; edge < edges; ++edge) {
    const uint8_t *row = &code->output[(size_t)edge * n];
    int idx = -1;
    for (int p = 0; p < ctx->n_patterns; ++p) {
      int same = 1;
      for (int j = 0; j < n; ++j) {
        if (ctx->pattern_bits[(size_t)p * n + j] != row[j]) {
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
      if (ctx->n_patterns == ctx->pattern_cap) {
        const int newcap = ctx->pattern_cap * 2;
        uint8_t *grown = dt_realloc(ctx->pattern_bits, (size_t)newcap * n);
        if (!grown) {
          return DT_ERR_ALLOC;
        }
        ctx->pattern_bits = grown;
        ctx->pattern_cap = newcap;
      }
      idx = ctx->n_patterns++;
      for (int j = 0; j < n; ++j) {
        ctx->pattern_bits[(size_t)idx * n + j] = row[j];
      }
    }
    group_of[edge] = idx;
  }
  return DT_OK;
}

int dt_trellis_init(dt_trellis *tr, dt_decode_ctx *ctx, const dt_code *code) {
  if (!tr || !ctx || !code) {
    return DT_ERR_ARG;
  }
  /* The packed backpointer (decode_internal.h) carries prev_state in 16 bits and
   * prev_drift_index in 15. dt_code_create caps K (num_states <= 256) and real
   * max_drift is tiny, but guard the packing invariant rather than silently
   * truncating. */
  if (ctx->num_states - 1 > DT_BP_MAX_STATE ||
      ctx->drift_width - 1 > DT_BP_MAX_DRIFT) {
    return DT_ERR_ARG;
  }
  tr->code = code;
  tr->smoothed_cost =
      ctx->expected_unlock; /* assume unlocked until the stream proves it */

  const size_t count = (size_t)ctx->num_states * ctx->drift_width;
  const int edges = ctx->num_states * 2;
  tr->metric = dt_malloc(count * sizeof(double));
  tr->next_metric = dt_malloc(count * sizeof(double));
  tr->backpointers =
      dt_malloc((size_t)ctx->ring_len * count * sizeof(dt_backpointer));
  tr->group_of = dt_malloc((size_t)edges * sizeof(int));
  tr->alpha_ring = dt_malloc((size_t)ctx->ring_len * count * sizeof(double));
  tr->smoothed_ring = dt_malloc((size_t)ctx->ring_len * sizeof(double));
  tr->frontier_ring = dt_malloc((size_t)ctx->ring_len * sizeof(int));
  if (!tr->metric || !tr->next_metric || !tr->backpointers || !tr->group_of ||
      !tr->alpha_ring || !tr->smoothed_ring || !tr->frontier_ring) {
    dt_trellis_free(tr);
    return DT_ERR_ALLOC;
  }
  if (register_patterns(ctx, code, tr->group_of) < 0) {
    dt_trellis_free(tr);
    return DT_ERR_ALLOC;
  }

  init_metric(ctx, tr);
  return DT_OK;
}

void dt_trellis_free(dt_trellis *tr) {
  if (!tr) {
    return;
  }
  dt_free(tr->metric);
  dt_free(tr->next_metric);
  dt_free(tr->backpointers);
  dt_free(tr->group_of);
  dt_free(tr->alpha_ring);
  dt_free(tr->smoothed_ring);
  dt_free(tr->frontier_ring);
  tr->metric = NULL;
  tr->next_metric = NULL;
  tr->backpointers = NULL;
  tr->group_of = NULL;
  tr->alpha_ring = NULL;
  tr->smoothed_ring = NULL;
  tr->frontier_ring = NULL;
}

int dt_decode_feed(dt_decode_ctx *ctx, const uint8_t *in, int n_in) {
  int status = reserve_received(ctx, n_in);
  if (status < 0) {
    return status;
  }
  dt_memcpy(ctx->received + ctx->received_length, in, (size_t)n_in);
  ctx->received_length += n_in;
  return DT_OK;
}

/* -- public single-stream decoder ------------------------------------------ */

/* The public decoder is one shared context plus one trellis: the N == 1 case of
 * the fused machinery above. */
struct dt_stream_decoder {
  dt_decode_ctx ctx;
  dt_trellis trellis;
};

dt_stream_decoder *dt_stream_decoder_create(const dt_code *code,
                                            const dt_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  struct dt_stream_decoder *sd = dt_calloc(1, sizeof(*sd));
  if (!sd) {
    return NULL;
  }
  if (dt_decode_ctx_init(&sd->ctx, params, code) < 0) {
    dt_free(sd);
    return NULL;
  }
  if (dt_trellis_init(&sd->trellis, &sd->ctx, code) < 0) {
    dt_decode_ctx_free(&sd->ctx);
    dt_free(sd);
    return NULL;
  }
  if (dt_decode_ctx_finalize(&sd->ctx) < 0) {
    dt_trellis_free(&sd->trellis);
    dt_decode_ctx_free(&sd->ctx);
    dt_free(sd);
    return NULL;
  }
  return sd;
}

void dt_stream_decoder_destroy(dt_stream_decoder *sd) {
  if (!sd) {
    return;
  }
  dt_trellis_free(&sd->trellis);
  dt_decode_ctx_free(&sd->ctx);
  dt_free(sd);
}

int dt_stream_decode(dt_stream_decoder *sd, const uint8_t *in, int n_in,
                     uint8_t *out, dt_decode_details *details, int max_out) {
  if (!sd || (n_in > 0 && !in) || n_in < 0 || max_out < 0) {
    return DT_ERR_ARG;
  }
  int status = dt_decode_feed(&sd->ctx, in, n_in);
  if (status < 0) {
    return status;
  }
  return run(&sd->ctx, &sd->trellis, out, details, max_out, /*draining=*/0);
}

int dt_stream_decode_flush(dt_stream_decoder *sd, uint8_t *out,
                           dt_decode_details *details, int max_out) {
  if (!sd || max_out < 0) {
    return DT_ERR_ARG;
  }
  /* draining=1 emits the whole tail (horizon = steps), at reduced look-ahead for
   * the last <= decision_depth bits; nothing is left buffered after. */
  return run(&sd->ctx, &sd->trellis, out, details, max_out, /*draining=*/1);
}
