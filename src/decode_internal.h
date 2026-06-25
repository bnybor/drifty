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

#ifndef DRIFTY_DECODE_INTERNAL_H
#define DRIFTY_DECODE_INTERNAL_H

/*
 * Private decoder internals shared by the single-stream decoder (decode.c) and
 * the multi-decoder (multi.c). Not installed.
 *
 * The sliding-window Viterbi state splits in two: a `dt_decode_ctx` holds
 * everything identical across codes decoded over the same stream - the received
 * buffer, the cadence (read_base/steps/decided and the shared re-anchor history),
 * the channel-derived cost constants, and the trellis dimensions - while a
 * `dt_trellis` holds one code's per-step working state. The single decoder is one
 * ctx + one trellis; the multi-decoder is one ctx + an array of trellises stepped
 * in lockstep, so a single received buffer and a single re-anchor decision serve
 * them all.
 */

#include <drifty/decode.h> /* dt_stream_params, dt_code (opaque), uint8_t */

#include <stddef.h>
#include <stdint.h>

/*
 * Opt-in internal assertions. The core is built -ffreestanding -fno-builtin
 * -nostdlib, so it cannot use <assert.h> (that would pull in __assert_fail /
 * abort from libc). __builtin_trap is a compiler intrinsic - not a libc symbol
 * and not suppressed by -fno-builtin - so it traps without breaking the
 * freestanding link. Off by default (zero cost); define DT_DEBUG_ASSERT to arm
 * the load-bearing invariants in the decoder (see dt_trellis_trace). */
#if defined(DT_DEBUG_ASSERT)
#define DT_ASSERT(cond) \
  do {                  \
    if (!(cond)) {      \
      __builtin_trap(); \
    }                   \
  } while (0)
#else
#define DT_ASSERT(cond) ((void)0)
#endif

/* Backpointer for one node: where it came from (prev_state, prev_drift_index)
 * and the input bit that got there, packed into one 32-bit word to shrink the
 * backpointer ring (~3x vs a struct) and make each forward-pass write a single
 * store. Layout: bit:1 | prev_drift_index:15 | prev_state:16. The field widths
 * dwarf the validated ranges (dt_code_create caps K <= 9, so prev_state < 256;
 * dt_trellis_init guards prev_drift_index/prev_state against overflow). */
typedef uint32_t dt_backpointer;

static inline dt_backpointer dt_bp_pack(int prev_state, int prev_drift_index,
                                        unsigned int bit) {
  return (bit & 1u) | ((unsigned int)prev_drift_index << 1) |
         ((unsigned int)prev_state << 16);
}
static inline unsigned int dt_bp_bit(dt_backpointer b) { return b & 1u; }
static inline int dt_bp_drift(dt_backpointer b) {
  return (int)((b >> 1) & 0x7FFFu);
}
static inline int dt_bp_state(dt_backpointer b) { return (int)(b >> 16); }

/* Field capacities for the packing above (used by dt_trellis_init's guard). */
#define DT_BP_MAX_STATE 0xFFFF
#define DT_BP_MAX_DRIFT 0x7FFF

/* Shared decode context: received buffer, cadence, channel constants, and
 * trellis dimensions - all identical across codes decoded together. */
typedef struct {
  int n, max_drift, num_states, drift_width, decision_depth;
  /* Snapshot-ring length for the BCJR soft output. The single-stream decoder
   * emits in batches: it steps up to (decision_depth + batch) ahead of the last
   * decided bit, then one backward sweep serves the whole batch (the warmup
   * overlap is re-swept, so cost is ~2 backward steps per bit instead of one full
   * window per bit). The alpha/branch/shift/smoothed rings must outlast that, so
   * ring_len = 2*decision_depth + slack (the backpointer ring stays
   * decision_depth - only the per-bit multi traceback uses it). */
  int ring_len;
  /* Branch-metric constants, in cost (negative-log-likelihood) units.
   * cost_bit[expected][received] is the per-bit match/mismatch cost for an
   * expected 0/1 against a received 0/1 (asymmetric: overwrites bias the two
   * directions differently). Insertion cost depends on the inserted bit's value:
   * cost_ins_t / cost_ins_f / cost_ins_e for a consumed 1 / 0 / DT_ERASURE. */
  double cost_bit[2][2];
  double cost_erase, cost_keep, cost_del;
  double cost_ins_t, cost_ins_f, cost_ins_e;
  /* Lock detection reference costs (channel-derived, code-independent). */
  double expected_lock, expected_unlock;

  int *shift; /* [decision_depth] shared re-anchor sigma per step */

  long long steps;   /* trellis steps processed                      */
  long long decided; /* decisions emitted (next step index to emit)  */

  /* Received-bit buffer: valid bits live in received[0 .. received_length).
   * read_base is the buffer index of the current step's zero-drift read base. */
  uint8_t *received;
  int received_capacity, received_length, read_base;

  /* Shared per-step scratch for the fused forward pass. An edge's alignment DP
   * depends only on its n-bit output pattern and source drift, not on the code
   * (fused codes share n / K / trellis geometry), so the per-(pattern, drift)
   * final rows are computed once per step here and every trellis's scatter
   * indexes them by pattern. Decoding N codes thus recomputes the alignment once
   * per step, not N times - the multi-decoder's chief redundancy. patterns are
   * the union of the codes' distinct output rows (group_of maps edges to them). */
  uint8_t *pattern_bits;   /* [n_patterns * n] distinct output rows (union)     */
  int n_patterns;          /* distinct output patterns across all fused codes   */
  int pattern_cap;         /* allocated capacity of pattern_bits, in patterns   */
  double *alignment;       /* [(n+1)*(max_consume+1)] alignment DP scratch      */
  double *align_shared;    /* [n_patterns*drift_width*(max_consume+1)] final rows */
  /* Per-step received-window match costs (shared), indexed by position-match_lo:
   * cost of aligning an expected 0/1 bit there, with in_range gating the ends. */
  double *match_cost0;     /* [n+4*max_drift] expected-bit-0 costs              */
  double *match_cost1;     /* [n+4*max_drift] expected-bit-1 costs              */
  double *ins_cost;        /* [n+4*max_drift] cost to consume position as insert */
  signed char *in_range;   /* [n+4*max_drift] 1 if position buffered            */
  int match_lo;            /* absolute received index of match table position 0 */

  /* Per-step branch-cost snapshots for the forward-backward (BCJR) soft output:
   * a ring of the shared per-(pattern, source drift) final rows (== align_shared,
   * the rows forward_pass scatters), indexed by step % decision_depth. The
   * backward beta pass (dt_trellis_details) reads these to recover the cost of
   * the best complete path for each bit value at the target step - which plain
   * traceback cannot, since compare-select discards the losing hypothesis. Sized
   * [decision_depth * n_patterns * drift_width * (n+2*max_drift+1)], allocated in
   * dt_decode_ctx_finalize once n_patterns is known. */
  double *branch_ring;
  /* Two [num_states*drift_width] scratch buffers for the backward beta pass
   * (only beta[t+1] is needed to form beta[t]). */
  double *beta_a, *beta_b;
} dt_decode_ctx;

/* One code's trellis working state. The alignment scratch lives in the shared
 * ctx now (it is computed once per step for all codes); a trellis carries only
 * its own metric/backpointers and the map from each edge to its output pattern's
 * index in ctx->pattern_bits / ctx->align_shared. */
typedef struct {
  const dt_code *code;          /* borrowed                                  */
  double *metric;               /* [num_states*drift_width] node costs       */
  double *next_metric;          /* [num_states*drift_width] scratch          */
  dt_backpointer *backpointers; /* [decision_depth*num_states*drift_width]   */
  double smoothed_cost;         /* EWMA of best-path per-step cost           */
  int *group_of;                /* [2*num_states] (state*2+bit)->pattern idx */
  /* Per-step forward-metric (alpha) snapshots for the BCJR backward pass: the
   * metric this trellis fed into each step's forward_pass (post re-anchor),
   * ringed by step % ring_len. [ring_len*num_states*drift_width]. */
  double *alpha_ring;
  /* Per-frontier smoothed-cost snapshots [ring_len]: smoothed_ring[s % ring_len]
   * is smoothed_cost when the frontier was at step s, so a batched decision for
   * target t reads the lock consistency at t's own decision time (frontier
   * t+decision_depth), not the later batch frontier. */
  double *smoothed_ring;
  /* Per-frontier best-node snapshots [ring_len]: frontier_ring[s % ring_len] is
   * the lowest-cost node of the metric at frontier s, so the batched multi vote
   * can trace each decoder's bit from its own decision-time frontier (exactly as
   * the per-bit decoder would), not from the later batch frontier. */
  int *frontier_ring;
} dt_trellis;

/* Validate `params`, take dimensions from `code`, allocate the shared buffers,
 * and zero the cadence. Returns DT_OK or a negative DT_ERR_*. A ctx that failed
 * (or was zero-initialised) is safe to pass to dt_decode_ctx_free. */
int dt_decode_ctx_init(dt_decode_ctx *ctx, const dt_stream_params *params,
                       const dt_code *code);
void dt_decode_ctx_free(dt_decode_ctx *ctx);

/* Allocate the shared per-step alignment table now that every trellis sharing
 * this ctx has been initialised (so the union of output patterns is known). Call
 * once after the last dt_trellis_init. Returns DT_OK or negative. */
int dt_decode_ctx_finalize(dt_decode_ctx *ctx);

/* Allocate one trellis's per-code arrays (sized from `ctx`), initialise its
 * metric and smoothed cost for blind acquisition, and register its output
 * patterns into the shared `ctx` (so `ctx` is mutated). Returns DT_OK or
 * negative; a failed/zeroed trellis is safe to dt_trellis_free. */
int dt_trellis_init(dt_trellis *tr, dt_decode_ctx *ctx, const dt_code *code);
void dt_trellis_free(dt_trellis *tr);

/* Append `n_in` received bits to the shared buffer. Returns DT_OK or negative. */
int dt_decode_feed(dt_decode_ctx *ctx, const uint8_t *in, int n_in);

/* Advance every trellis one fused step: one re-anchor sigma (picked from the
 * best-fitting trellis) is applied to all, then each runs its forward pass, then
 * the shared cadence advances. With n == 1 this is the plain single-decoder step. */
void dt_decode_step(dt_decode_ctx *ctx, dt_trellis *trs, size_t n);

/* Lowest-cost node at a trellis's current frontier. */
int dt_trellis_frontier(const dt_decode_ctx *ctx, const dt_trellis *tr);

/* Input bit a trellis decided at step `target`, traced back from node `frontier`
 * at step `from_step` (from_step - target <= ring_len). Pass ctx->steps and a
 * current frontier node for an ordinary frontier traceback; the batched multi
 * vote passes a snapshotted earlier frontier (frontier_ring). */
unsigned char dt_trellis_trace(const dt_decode_ctx *ctx, const dt_trellis *tr,
                               long long from_step, int frontier,
                               long long target);

/* Map one per-step path cost onto the lock scale (0..1): expected_unlock -> 0,
 * expected_lock -> 1, clamped. The lock consistency c_lock is this applied to a
 * trellis's smoothed cost (dt_trellis_soft_batch). */
double dt_lock_from_cost(const dt_decode_ctx *ctx, double cost);

/* Forward-backward (BCJR) soft output for the n targets [base, base+n) from one
 * backward sweep over the retained window (frontier at ctx->steps). bits_out[k]
 * (stride 1) and details_out[k*detail_stride] (either may be NULL) receive target
 * base+k. The multi-decoder uses detail_stride = codes_len to interleave each
 * decoder's details; the single-stream decoder uses 1. */
void dt_trellis_soft_batch(const dt_decode_ctx *ctx, const dt_trellis *tr,
                           long long base, int n, uint8_t *bits_out,
                           dt_decode_details *details_out, int detail_stride);

#endif /* DRIFTY_DECODE_INTERNAL_H */
