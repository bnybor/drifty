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
 * once and, per output bit, keep the bit from whichever code is most confidently
 * locked. Rather than wrap N independent dv_stream_decoders, it drives the
 * decoder internals directly (decode_internal.h): one shared dv_decode_ctx (the
 * single received buffer + cadence) and one dv_trellis per code, all advanced in
 * lockstep by dv_decode_step with one shared re-anchor. Because the cadence is
 * shared, every trellis decides the same step at the same time, so the per-bit
 * merge is a direct lock comparison - no output realignment needed.
 */

#include <drift_viterbi/multi.h>

#include <drift_viterbi/encode.h>
#include <drift_viterbi/stdlib.h>

#include "decode_internal.h"

/* Defaults for the selection thresholds when params (or a field) is left 0: a
 * code wins a bit only when its lock probability clears this floor and beats the
 * next-best code's by this margin; otherwise the bit is erased. */
static const double DV_MULTI_LOCK_FLOOR_DEFAULT = 0.6;
static const double DV_MULTI_LOCK_MARGIN_DEFAULT = 0.2;

struct dv_multi_decoder {
  dv_decode_ctx ctx;     /* shared received buffer + cadence            */
  dv_trellis *trellises; /* [n] one per code                            */
  size_t n;
  double lock_floor;  /* min lock probability to commit a bit       */
  double lock_margin; /* min lead over the next-best code           */
  /* Index of the most recently locked code, carried across calls so the
   * end-of-stream flush can decode its tail; -1 until one locks. */
  int locked;
};

dv_multi_decoder *dv_multi_create(const dv_multi_params *params) {
  if (!params || (params->codes_len > 0 && !params->codes)) {
    return NULL;
  }
  dv_multi_decoder *m = dv_calloc(1, sizeof(*m));
  if (!m) {
    return NULL;
  }
  m->n = params->codes_len;
  m->lock_floor = params->lock_floor > 0.0 ? params->lock_floor
                                           : DV_MULTI_LOCK_FLOOR_DEFAULT;
  m->lock_margin = params->lock_margin > 0.0 ? params->lock_margin
                                             : DV_MULTI_LOCK_MARGIN_DEFAULT;
  m->locked = -1;

  if (params->codes_len > 0) {
    /* All codes share the one context's dimensions and channel model; take them
     * from the first (they must share a rate - see multi.h). */
    if (dv_decode_ctx_init(&m->ctx, &params->stream, params->codes[0]) < 0) {
      dv_multi_destroy(m);
      return NULL;
    }
    /* calloc so a build failure partway leaves the rest zeroed, and
     * dv_multi_destroy can free what was built (dv_trellis_free is NULL-safe). */
    m->trellises = dv_calloc(params->codes_len, sizeof(dv_trellis));
    if (!m->trellises) {
      dv_multi_destroy(m);
      return NULL;
    }
    for (size_t j = 0; j < params->codes_len; ++j) {
      if (dv_trellis_init(&m->trellises[j], &m->ctx, params->codes[j]) < 0) {
        dv_multi_destroy(m);
        return NULL;
      }
    }
  }
  return m;
}

void dv_multi_destroy(dv_multi_decoder *m) {
  if (!m) {
    return;
  }
  if (m->trellises) {
    for (size_t j = 0; j < m->n; ++j) {
      dv_trellis_free(&m->trellises[j]);
    }
    dv_free(m->trellises);
  }
  dv_decode_ctx_free(&m->ctx);
  dv_free(m);
}

/* Common argument check for the two public entry points; mirrors
 * dv_stream_decode's contract. */
static int multi_args_ok(const dv_multi_decoder *d, const uint8_t *in, int n_in,
                         const uint8_t *out, int max_out) {
  return d && !(n_in > 0 && !in) && n_in >= 0 && !(max_out > 0 && !out) &&
         max_out >= 0 && !(d->n > 0 && !d->trellises);
}

/* Step the fused trellises over the buffered input, emitting one merged bit per
 * decided step: the bit of the most confidently locked code, or DV_ERASURE when
 * none clears the floor / beats the next best by the margin. `draining` relaxes
 * the look-ahead for end-of-stream. Shared by decode and flush. */
static int multi_run(dv_multi_decoder *d, uint8_t *out, int *locked_decoder,
                     int max_out, int draining) {
  dv_decode_ctx *ctx = &d->ctx;
  int output_count = 0;
  int last = d->locked;
  for (;;) {
    if (!draining) {
      if (ctx->received_length < ctx->read_base + ctx->n + ctx->max_drift + 1) {
        break;
      }
    } else {
      if (ctx->received_length - ctx->read_base < ctx->n) {
        break;
      }
    }

    if (ctx->steps >= ctx->decision_depth) {
      if (output_count >= max_out) {
        break;
      }
      double best = -1.0, second = -1.0;
      int best_idx = 0;
      for (size_t j = 0; j < d->n; ++j) {
        double lk = dv_trellis_lock(ctx, &d->trellises[j]);
        if (lk > best) {
          second = best;
          best = lk;
          best_idx = (int)j;
        } else if (lk > second) {
          second = lk;
        }
      }
      if (best >= d->lock_floor && best - second >= d->lock_margin) {
        int frontier = dv_trellis_frontier(ctx, &d->trellises[best_idx]);
        out[output_count] =
            dv_trellis_trace(ctx, &d->trellises[best_idx], frontier, ctx->decided);
        last = best_idx;
        if (locked_decoder) {
          locked_decoder[output_count] = best_idx;
        }
      } else {
        out[output_count] = DV_ERASURE;
        if (locked_decoder) {
          locked_decoder[output_count] = -1;
        }
      }
      output_count++;
      ctx->decided++;
    }
    dv_decode_step(ctx, d->trellises, d->n);
  }
  d->locked = last; /* carry the latest winner into the end-of-stream flush */
  return output_count;
}

int dv_multi_decode(dv_multi_decoder *d, const uint8_t *in, int n_in,
                    uint8_t *out, int *locked_decoder, int max_out) {
  if (!multi_args_ok(d, in, n_in, out, max_out)) {
    return DV_ERR_ARG;
  }
  if (d->n == 0) {
    return 0;
  }
  int status = dv_decode_feed(&d->ctx, in, n_in);
  if (status < 0) {
    return status;
  }
  return multi_run(d, out, locked_decoder, max_out, /*draining=*/0);
}

int dv_multi_decode_flush(dv_multi_decoder *d, uint8_t *out, int max_out) {
  if (!multi_args_ok(d, NULL, 0, out, max_out)) {
    return DV_ERR_ARG;
  }
  if (d->n == 0 || max_out == 0) {
    return 0;
  }
  dv_decode_ctx *ctx = &d->ctx;
  int output_count = multi_run(d, out, /*locked_decoder=*/NULL, max_out,
                               /*draining=*/1);

  /* Drain the pipeline. The last decision_depth bits carry no lock probability,
   * so decode them with the most recently locked code (its trellis holds the
   * true tail); with nothing ever locked, erase them. */
  if (ctx->decided < ctx->steps && output_count < max_out) {
    int win = d->locked;
    if (win < 0) {
      while (ctx->decided < ctx->steps && output_count < max_out) {
        out[output_count++] = DV_ERASURE;
        ctx->decided++;
      }
    } else {
      int frontier = dv_trellis_frontier(ctx, &d->trellises[win]);
      while (ctx->decided < ctx->steps && output_count < max_out) {
        out[output_count++] =
            dv_trellis_trace(ctx, &d->trellises[win], frontier, ctx->decided);
        ctx->decided++;
      }
    }
  }
  return output_count;
}
