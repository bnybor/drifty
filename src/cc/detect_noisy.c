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
 * detect_noisy codec: realizes the abstract dt_stream_soft_decoder interface over the
 * detect_noisy decode engine (see detect_noisy/decode.c for the blind code-presence
 * detector). detect is soft-only and standalone (no hard decoder, no encoder, and no
 * dt_cc_code), though it does take the rich dt_cc_detect_noisy_stream_params channel
 * model; this file is the vtable plumbing that adapts the engine to the abstract
 * soft interface and maps its per-position verdict onto a dt_soft_bit
 * (details_to_soft below).
 */

#include <drifty/cc/detect_noisy.h>

#include "detect_noisy/decode.h" /* dt_cc_detect_noisy_stream_decoder + dt_cc_detect_noisy_stream_decode* */
#include <drifty/stdlib.h>

/* -- soft decoder ---------------------------------------------------------- */

/* Map detect's per-position verdict onto a dt_soft_bit. detect populates only two
 * consistency reads; they ride in the soft fields by the engine-c_lost -> soft-c_erasure
 * convention:
 *   c_erasure = engine c_lost  = consistency with "a convolutional code IS present"
 *   c_absent  = engine c_absent = consistency with "no code / the stream is random"
 * All other soft fields are 0 (detect does not recover bit values - it only
 * detects code presence). The two need not sum to 1. */
static void details_to_soft(const dt_cc_detect_noisy_decode_details *d,
                            dt_soft_bit *o) {
  o->c_false = 0.0f;
  o->c_true = 0.0f;
  o->c_erasure = d->c_lost;
  o->c_invalid = 0.0f;
  o->c_absent = d->c_absent;
  o->c_locked = 0.0f;
}

/* How many soft outputs to pull from the engine per call - a fixed stack scratch
 * so decode/finalize need no allocation. */
#define DETECT_SOFT_CHUNK 64

static int detect_soft_begin(dt_stream_soft_decoder *dec, const dt_bit *src,
                             size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0; /* no preamble to consume */
}

static int detect_soft_decode(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                              size_t dst_len, const dt_bit *src, size_t src_len) {
  dt_cc_detect_noisy_stream_decoder *sd = dec->data;
  dt_cc_detect_noisy_decode_details chunk[DETECT_SOFT_CHUNK];
  size_t written = 0;
  int fed = 0; /* feed src on the first collect only; drain on later ones */
  /* do/while, not while: src must be handed to the engine even when dst_len is
   * 0 (a feed-only "pump" call), so the input is buffered regardless of how much
   * output is collected. */
  do {
    const size_t remain = dst_len - written;
    const int want =
        remain > DETECT_SOFT_CHUNK ? DETECT_SOFT_CHUNK : (int)remain;
    const int got = dt_cc_detect_noisy_stream_decode(sd, fed ? NULL : src,
                                           fed ? 0 : (int)src_len, chunk, want);
    fed = 1;
    if (got < 0) {
      return got;
    }
    for (int i = 0; i < got; ++i) {
      details_to_soft(&chunk[i], &dst[written + (size_t)i]);
    }
    written += (size_t)got;
    if (got < want) {
      break; /* nothing more decodable from what is buffered */
    }
  } while (written < dst_len);
  return (int)written;
}

static int detect_soft_finalize(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                                size_t dst_len) {
  dt_cc_detect_noisy_stream_decoder *sd = dec->data;
  dt_cc_detect_noisy_decode_details chunk[DETECT_SOFT_CHUNK];
  size_t written = 0;
  while (written < dst_len) {
    const size_t remain = dst_len - written;
    const int want =
        remain > DETECT_SOFT_CHUNK ? DETECT_SOFT_CHUNK : (int)remain;
    const int got = dt_cc_detect_noisy_stream_decode_flush(sd, chunk, want);
    if (got < 0) {
      return got;
    }
    for (int i = 0; i < got; ++i) {
      details_to_soft(&chunk[i], &dst[written + (size_t)i]);
    }
    written += (size_t)got;
    if (got == 0) {
      break; /* fully drained */
    }
  }
  return (int)written;
}

dt_stream_soft_decoder *dt_cc_detect_noisy_soft_decoder_create(
    const dt_cc_detect_noisy_stream_params *params) {
  if (!params) {
    return NULL;
  }
  dt_cc_detect_noisy_stream_decoder *sd = dt_cc_detect_noisy_stream_decoder_create(params);
  if (!sd) {
    return NULL;
  }
  dt_stream_soft_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_cc_detect_noisy_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = detect_soft_begin;
  dec->decode = detect_soft_decode;
  dec->finalize = detect_soft_finalize;
  dec->data = sd;
  return dec;
}

void dt_cc_detect_noisy_soft_decoder_destroy(dt_stream_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_cc_detect_noisy_stream_decoder_destroy(dec->data);
  dt_free(dec);
}
