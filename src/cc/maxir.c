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
 * MAXIR codec: realizes the abstract dt_stream_decoder / dt_stream_soft_decoder interfaces
 * over the MAXIR decode engine. The code handle is dt_cc_code throughout. To
 * encode, use the standalone encoder (src/cc/encoder).
 */

#include <drifty/cc/maxir.h>

#include "maxir/decode.h" /* dt_cc_maxir_stream_decoder + dt_cc_maxir_stream_decode* */
#include <drifty/stdlib.h>

/* dt_bit is uint8_t (bit.h), the same element type the engine's decode buffers
 * use, so the dt_bit* <-> uint8_t* hand-offs below need no conversion. */

/* -- decoder --------------------------------------------------------------- */

static int maxir_decoder_begin(dt_stream_decoder *dec, const dt_bit *src, size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0; /* no preamble to consume */
}

static int maxir_decoder_decode(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len,
                               const dt_bit *src, size_t src_len) {
  dt_cc_maxir_stream_decoder *sd = dec->data;
  /* The hard decoder ignores the per-bit soft output (pass NULL details). */
  return dt_cc_maxir_stream_decode(sd, src, (int)src_len, dst, NULL, (int)dst_len);
}

static int maxir_decoder_finalize(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len) {
  dt_cc_maxir_stream_decoder *sd = dec->data;
  return dt_cc_maxir_stream_decode_flush(sd, dst, NULL, (int)dst_len);
}

dt_stream_decoder *dt_cc_maxir_decoder_create(const dt_cc_code *code,
                                   const dt_cc_maxir_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_cc_maxir_stream_decoder *sd = dt_cc_maxir_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_stream_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_cc_maxir_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = maxir_decoder_begin;
  dec->decode = maxir_decoder_decode;
  dec->finalize = maxir_decoder_finalize;
  dec->data = sd;
  return dec;
}

void dt_cc_maxir_decoder_destroy(dt_stream_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_cc_maxir_stream_decoder_destroy(dec->data);
  dt_free(dec);
}

/* -- soft decoder ---------------------------------------------------------- */

/* Map the engine's per-bit soft output onto a dt_soft_bit. The fields
 * line up one-to-one: c_lost is the "erasure / unknowable value" consistency,
 * and the engine also reports c_invalid (the slot's coded group was the
 * encoder's DT_INVALID poison marker) and c_absent (1 - c_lock; the slot is not
 * backed by a tracked codeword stream), so both pass straight through. */
static void details_to_soft(const dt_cc_maxir_decode_details *d,
                            dt_soft_bit *o) {
  o->c_false = d->c_false;
  o->c_true = d->c_true;
  o->c_erasure = d->c_lost;
  o->c_invalid = d->c_invalid;
  o->c_absent = d->c_absent;
  o->c_locked = d->c_lock;
}

/* How many soft outputs to pull from the engine per call - a fixed stack scratch
 * so decode/finalize need no allocation. */
#define MAXIR_SOFT_CHUNK 64

static int maxir_soft_begin(dt_stream_soft_decoder *dec, const dt_bit *src, size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0; /* no preamble to consume */
}

static int maxir_soft_decode(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                            size_t dst_len, const dt_bit *src, size_t src_len) {
  dt_cc_maxir_stream_decoder *sd = dec->data;
  dt_cc_maxir_decode_details chunk[MAXIR_SOFT_CHUNK];
  size_t written = 0;
  int fed = 0; /* feed src on the first collect only; drain on later ones */
  /* do/while, not while: src must be handed to the engine even when dst_len is
   * 0 (a feed-only "pump" call), matching the hard decoder, which buffers the
   * input regardless of how much output is collected. */
  do {
    const size_t remain = dst_len - written;
    const int want = remain > MAXIR_SOFT_CHUNK ? MAXIR_SOFT_CHUNK : (int)remain;
    const int got = dt_cc_maxir_stream_decode(sd, fed ? NULL : src,
                                          fed ? 0 : (int)src_len, NULL, chunk,
                                          want);
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

static int maxir_soft_finalize(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                              size_t dst_len) {
  dt_cc_maxir_stream_decoder *sd = dec->data;
  dt_cc_maxir_decode_details chunk[MAXIR_SOFT_CHUNK];
  size_t written = 0;
  while (written < dst_len) {
    const size_t remain = dst_len - written;
    const int want = remain > MAXIR_SOFT_CHUNK ? MAXIR_SOFT_CHUNK : (int)remain;
    const int got = dt_cc_maxir_stream_decode_flush(sd, NULL, chunk, want);
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

dt_stream_soft_decoder *dt_cc_maxir_soft_decoder_create(
    const dt_cc_code *code, const dt_cc_maxir_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_cc_maxir_stream_decoder *sd = dt_cc_maxir_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_stream_soft_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_cc_maxir_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = maxir_soft_begin;
  dec->decode = maxir_soft_decode;
  dec->finalize = maxir_soft_finalize;
  dec->data = sd;
  return dec;
}

void dt_cc_maxir_soft_decoder_destroy(dt_stream_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_cc_maxir_stream_decoder_destroy(dec->data);
  dt_free(dec);
}
