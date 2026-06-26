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
 * Multi-decoder: decode one received stream against several candidate codes at
 * once and, per output bit, soft-combine their decisions weighted by how well
 * each code fits the stream. Rather than wrap N independent dt_stream_decoders,
 * it drives the decoder internals directly (decode_internal.h): one shared
 * dt_decode_ctx (the single received buffer + cadence) and one dt_trellis per
 * code, all advanced in lockstep by dt_decode_step with one shared re-anchor.
 * Because the cadence is shared, every trellis decides the same step at the same
 * time, so the per-bit merge needs no output realignment - the codes' bits for a
 * given step are directly commensurable and are combined by likelihood weight
 * (see multi_run).
 */

#include <drifty/hybrid/multi_decode.h>

#include <drifty/hybrid/encode.h>
#include <drifty/stdlib.h>

#include "decode_internal.h"

/* Defaults for the selection thresholds when params (or a field) is left 0: at
 * least one code's lock probability must clear this floor for any bit to commit,
 * and the winning bit value must lead the other by this share of the total
 * likelihood weight; otherwise the bit is erased. */
static const double DT_MULTI_LOCK_FLOOR_DEFAULT = 0.6;
static const double DT_MULTI_LOCK_MARGIN_DEFAULT = 0.2;

struct dt_multi_decoder {
  dt_decode_ctx ctx;     /* shared received buffer + cadence            */
  dt_trellis *trellises; /* [n] one per code                            */
  size_t n;
  double lock_floor;  /* min lock probability to commit a bit       */
  double lock_margin; /* min lead over the next-best code           */
  /* Index of the most recently locked code, carried across calls so the
   * end-of-stream flush can decode its tail; -1 until one locks. */
  int locked;
};

dt_multi_decoder *dt_multi_create(const dt_multi_params *params) {
  if (!params || (params->codes_len > 0 && !params->codes)) {
    return NULL;
  }
  dt_multi_decoder *m = dt_calloc(1, sizeof(*m));
  if (!m) {
    return NULL;
  }
  m->n = params->codes_len;
  m->lock_floor = params->lock_floor > 0.0 ? params->lock_floor
                                           : DT_MULTI_LOCK_FLOOR_DEFAULT;
  m->lock_margin = params->lock_margin > 0.0 ? params->lock_margin
                                             : DT_MULTI_LOCK_MARGIN_DEFAULT;
  m->locked = -1;

  if (params->codes_len > 0) {
    /* All codes share the one context's dimensions and channel model; take them
     * from the first (they must share a rate - see multi.h). */
    if (dt_decode_ctx_init(&m->ctx, &params->stream, params->codes[0]) < 0) {
      dt_multi_destroy(m);
      return NULL;
    }
    /* calloc so a build failure partway leaves the rest zeroed, and
     * dt_multi_destroy can free what was built (dt_trellis_free is NULL-safe). */
    m->trellises = dt_calloc(params->codes_len, sizeof(dt_trellis));
    if (!m->trellises) {
      dt_multi_destroy(m);
      return NULL;
    }
    for (size_t j = 0; j < params->codes_len; ++j) {
      /* Every trellis is sized from the shared ctx (taken from codes[0]) yet
       * forward_pass walks each code's own next_state/output tables over
       * ctx->num_states. Codes that differ in rate (dt_ccode_n) or constraint
       * length (dt_ccode_k, hence n_states) would index those tables out of
       * bounds, so reject the set rather than corrupt memory. A NULL slot fails
       * here too: dt_ccode_n(NULL) == -1 != codes[0]'s rate. */
      if (dt_ccode_n(params->codes[j]) != dt_ccode_n(params->codes[0]) ||
          dt_ccode_k(params->codes[j]) != dt_ccode_k(params->codes[0])) {
        dt_multi_destroy(m);
        return NULL;
      }
      if (dt_trellis_init(&m->trellises[j], &m->ctx, params->codes[j]) < 0) {
        dt_multi_destroy(m);
        return NULL;
      }
    }
    /* Every trellis has registered its output patterns into the shared ctx, so
     * the union is known - size the shared per-step alignment table. */
    if (dt_decode_ctx_finalize(&m->ctx) < 0) {
      dt_multi_destroy(m);
      return NULL;
    }
  }
  return m;
}

void dt_multi_destroy(dt_multi_decoder *m) {
  if (!m) {
    return;
  }
  if (m->trellises) {
    for (size_t j = 0; j < m->n; ++j) {
      dt_trellis_free(&m->trellises[j]);
    }
    dt_free(m->trellises);
  }
  dt_decode_ctx_free(&m->ctx);
  dt_free(m);
}

/* Common argument check for the two public entry points; mirrors
 * dt_stream_decode's contract (out may be NULL - the caller may want only
 * details, or to drain). */
static int multi_args_ok(const dt_multi_decoder *d, const uint8_t *in, int n_in,
                         int max_out) {
  return d && !(n_in > 0 && !in) && n_in >= 0 && max_out >= 0 &&
         !(d->n > 0 && !d->trellises);
}

/* Step the fused trellises over the buffered input, emitting merged bits in
 * batches (like the single-stream decoder): step forward until ring_len bits
 * past the last decision are buffered, then decide the whole batch at once.
 *
 * Per-decoder soft output (details) comes from one BCJR backward sweep per
 * decoder (dt_trellis_soft_batch), interleaved into the details block with
 * stride d->n. The merged out bit is the likelihood-weighted vote of the
 * decoders' traced bits: each decoder's weight w_j = exp(-(cost_j - min_cost))
 * and its lock come from its smoothed cost at the bit's OWN decision time
 * (smoothed_ring[t+decision_depth]), so the merge matches the per-bit decoder; a
 * code that does not fit gets vanishing weight automatically. The heavier value
 * wins when it leads by >= lock_margin of the total weight, else the bit is
 * ambiguous and erased. One code is still gated absolutely: unless the best
 * decoder clears lock_floor nothing is locked and the bit is erased. `draining`
 * emits the reduced-depth tail. out and details may both be NULL. */
static int multi_run(dt_multi_decoder *d, uint8_t *out,
                     dt_decode_details *details, int max_out, int draining) {
  dt_decode_ctx *ctx = &d->ctx;
  const int dd = ctx->decision_depth, rl = ctx->ring_len;
  const long long cap_window = 2 * (long long)dd;
  const size_t ncodes = d->n;
  int output_count = 0;
  int last = d->locked;
  for (;;) {
    /* Accumulate forward steps until a batch is buffered or we cannot step. */
    while (ctx->steps - ctx->decided < cap_window) {
      if (!draining) {
        if (ctx->received_length <
            ctx->read_base + ctx->n + ctx->max_drift + 1) {
          break;
        }
      } else if (ctx->received_length - ctx->read_base < ctx->n) {
        break;
      }
      dt_decode_step(ctx, d->trellises, ncodes);
    }
    const long long horizon = draining ? ctx->steps : (ctx->steps - dd);
    long long avail = horizon - ctx->decided;
    if (avail > max_out - output_count) {
      avail = max_out - output_count;
    }
    if (avail <= 0) {
      break;
    }
    const int n_emit = (int)avail;

    /* Per-decoder soft output: one backward sweep per decoder, interleaved into
     * the details block (stride ncodes). */
    if (details) {
      for (size_t j = 0; j < ncodes; ++j) {
        dt_trellis_soft_batch(ctx, &d->trellises[j], ctx->decided, n_emit, NULL,
                              details + (size_t)output_count * ncodes + j,
                              (int)ncodes);
      }
    }

    for (int k = 0; k < n_emit; ++k) {
      const long long t = ctx->decided + k;
      const int pos = output_count + k;
      /* This bit's own decision frontier (clamped for the flush tail). */
      long long s = t + dd;
      if (s > ctx->steps) s = ctx->steps;
      const size_t si = (size_t)(s % rl);

      double min_cost = d->trellises[0].smoothed_ring[si];
      int best_idx = 0;
      for (size_t j = 1; j < ncodes; ++j) {
        if (d->trellises[j].smoothed_ring[si] < min_cost) {
          min_cost = d->trellises[j].smoothed_ring[si];
          best_idx = (int)j;
        }
      }
      const double best_lock = dt_lock_from_cost(ctx, min_cost);

      int decided_bit = -1; /* 0/1 once chosen; stays -1 to erase */
      if (best_lock >= d->lock_floor) {
        double weight_total = 0.0, weight_ones = 0.0;
        for (size_t j = 0; j < ncodes; ++j) {
          const double w =
              dt_exp(min_cost - d->trellises[j].smoothed_ring[si]);
          weight_total += w;
          /* Trace this decoder's bit from its frontier at the bit's own decision
           * time (step s = t+decision_depth), exactly as the per-bit decoder. */
          const int fr = d->trellises[j].frontier_ring[si];
          if (dt_trellis_trace(ctx, &d->trellises[j], s, fr, t)) {
            weight_ones += w;
          }
        }
        const double lead = 2.0 * weight_ones / weight_total - 1.0;
        if (lead >= d->lock_margin) {
          decided_bit = 1;
        } else if (-lead >= d->lock_margin) {
          decided_bit = 0;
        }
      }

      if (decided_bit >= 0) {
        last = best_idx;
        if (out) out[pos] = decided_bit ? DT_TRUE : DT_FALSE;
      } else if (out) {
        out[pos] = DT_ERASURE;
      }
    }
    output_count += n_emit;
    ctx->decided += n_emit;
    if (output_count >= max_out) {
      break;
    }
  }
  d->locked = last; /* carry the latest winner into the end-of-stream flush */
  return output_count;
}

int dt_multi_decode(dt_multi_decoder *d, const uint8_t *in, int n_in,
                    uint8_t *out, dt_decode_details *details, int max_out) {
  if (!multi_args_ok(d, in, n_in, max_out)) {
    return DT_ERR_ARG;
  }
  if (d->n == 0) {
    return 0;
  }
  int status = dt_decode_feed(&d->ctx, in, n_in);
  if (status < 0) {
    return status;
  }
  return multi_run(d, out, details, max_out, /*draining=*/0);
}

int dt_multi_decode_flush(dt_multi_decoder *d, uint8_t *out,
                          dt_decode_details *details, int max_out) {
  if (!multi_args_ok(d, NULL, 0, max_out)) {
    return DT_ERR_ARG;
  }
  if (d->n == 0 || max_out == 0) {
    return 0;
  }
  /* draining=1 emits the whole tail (horizon = steps) at reduced look-ahead;
   * nothing is left buffered after. */
  return multi_run(d, out, details, max_out, /*draining=*/1);
}
