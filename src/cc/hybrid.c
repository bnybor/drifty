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
 * Hybrid codec: realizes the abstract dt_stream_decoder / dt_stream_soft_decoder interfaces
 * over the drift-tolerant stream-decode engine. The code handle is dt_cc_code
 * throughout. To encode, use the standalone encoder (src/cc/encoder).
 */

#include <drifty/cc/hybrid.h>

#include "hybrid/decode.h" /* dt_cc_stream_decoder + dt_cc_stream_decode* */
#include <drifty/stdlib.h>

/* dt_bit is uint8_t (bit.h), the same element type the engine's decode buffers
 * use, so the dt_bit* <-> uint8_t* hand-offs below need no conversion. */

/* -- decoder --------------------------------------------------------------- */

static int hybrid_decoder_begin(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* the stream decoder self-acquires; no preamble to emit */
}

static int hybrid_decoder_decode(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len,
                                 const dt_bit *src, size_t src_len) {
  dt_cc_stream_decoder *sd = dec->data;
  return dt_cc_stream_decode(sd, src, (int)src_len, dst, NULL, (int)dst_len);
}

static int hybrid_decoder_finalize(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len) {
  dt_cc_stream_decoder *sd = dec->data;
  return dt_cc_stream_decode_flush(sd, dst, NULL, (int)dst_len);
}

dt_stream_decoder *dt_cc_hybrid_decoder_create(const dt_cc_code *code,
                                     const dt_cc_hybrid_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_cc_stream_decoder *sd = dt_cc_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_stream_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_cc_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = hybrid_decoder_begin;
  dec->decode = hybrid_decoder_decode;
  dec->finalize = hybrid_decoder_finalize;
  dec->data = sd;
  return dec;
}

void dt_cc_hybrid_decoder_destroy(dt_stream_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_cc_stream_decoder_destroy(dec->data);
  dt_free(dec);
}

/* -- soft decoder ---------------------------------------------------------- */

/* Map the engine's per-bit soft output onto a dt_soft_bit. The engine
 * folds all information loss into c_lost (and decodes the bit as DT_ERASURE when
 * it wins), so c_lost is the "erasure / unknowable value" consistency; it does
 * not separately model a stuck non-truth value or a per-position deletion, so
 * c_invalid and c_absent are left 0. */
static void details_to_soft(const dt_cc_decode_details *d,
                            dt_soft_bit *o) {
  o->c_false = d->c_false;
  o->c_true = d->c_true;
  o->c_erasure = d->c_lost;
  o->c_invalid = 0.0;
  o->c_absent = 0.0;
  o->c_locked = d->c_lock;
}

/* How many soft outputs to pull from the engine per call - a fixed stack scratch
 * so decode/finalize need no allocation. */
#define HYBRID_SOFT_CHUNK 64

static int hybrid_soft_begin(dt_stream_soft_decoder *dec, dt_soft_bit *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* the stream decoder self-acquires; no preamble to emit */
}

static int hybrid_soft_decode(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                              size_t dst_len, const dt_bit *src, size_t src_len) {
  dt_cc_stream_decoder *sd = dec->data;
  dt_cc_decode_details chunk[HYBRID_SOFT_CHUNK];
  size_t written = 0;
  int fed = 0; /* feed src on the first collect only; drain on later ones */
  /* do/while, not while: src must be handed to the engine even when dst_len is
   * 0 (a feed-only "pump" call), matching the hard decoder, which buffers the
   * input regardless of how much output is collected. */
  do {
    const size_t remain = dst_len - written;
    const int want =
        remain > HYBRID_SOFT_CHUNK ? HYBRID_SOFT_CHUNK : (int)remain;
    const int got = dt_cc_stream_decode(sd, fed ? NULL : src,
                                     fed ? 0 : (int)src_len, NULL, chunk, want);
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

static int hybrid_soft_finalize(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                                size_t dst_len) {
  dt_cc_stream_decoder *sd = dec->data;
  dt_cc_decode_details chunk[HYBRID_SOFT_CHUNK];
  size_t written = 0;
  while (written < dst_len) {
    const size_t remain = dst_len - written;
    const int want =
        remain > HYBRID_SOFT_CHUNK ? HYBRID_SOFT_CHUNK : (int)remain;
    const int got = dt_cc_stream_decode_flush(sd, NULL, chunk, want);
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

dt_stream_soft_decoder *dt_cc_hybrid_soft_decoder_create(
    const dt_cc_code *code, const dt_cc_hybrid_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_cc_stream_decoder *sd = dt_cc_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_stream_soft_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_cc_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = hybrid_soft_begin;
  dec->decode = hybrid_soft_decode;
  dec->finalize = hybrid_soft_finalize;
  dec->data = sd;
  return dec;
}

void dt_cc_hybrid_soft_decoder_destroy(dt_stream_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_cc_stream_decoder_destroy(dec->data);
  dt_free(dec);
}
