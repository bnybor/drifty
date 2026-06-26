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
 * BCJR codec: realizes the abstract dt_encoder / dt_decoder / dt_soft_decoder
 * interfaces over the convolutional encode and BCJR decode engine. The code
 * handle is dt_ccode throughout.
 *
 * The maxir codec (src/cc/maxir.c) forked from this one on 2026-06-26 and is now
 * independent - do not propagate bcjr changes to it.
 *
 * The encoder is complete; the decode engine is a stub (see bcjr/decode.c) - the
 * vtable plumbing here is wired but produces no output until the forward-backward
 * algorithm lands.
 */

#include <drifty/cc/bcjr.h>

#include "bcjr/decode.h" /* dt_bcjr_stream_decoder + dt_bcjr_stream_decode* */
#include <drifty/stdlib.h>

/* dt_t is uint8_t (bit.h), the same element type the engine's encode/decode
 * buffers use, so the dt_t* <-> uint8_t* hand-offs below need no conversion. */

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  const dt_ccode *code;  /* the convolutional code this encoder emits */
  int state;             /* running shift-register state across encode calls */
  unsigned int unknown;  /* in-flight poison register (non-boolean inputs) */
} bcjr_encoder;

static int bcjr_encoder_begin(dt_encoder *enc, dt_t *dst, size_t dst_len) {
  bcjr_encoder *st = enc->data;
  st->state = 0; /* fresh stream; the convolutional encoder needs no preamble */
  st->unknown = 0;
  (void)dst;
  (void)dst_len;
  return 0;
}

static int bcjr_encoder_encode(dt_encoder *enc, dt_t *dst, size_t dst_len,
                               const dt_t *src, size_t src_len) {
  bcjr_encoder *st = enc->data;
  /* The engine writes src_len * n coded bits and does not bound-check, so gate
   * it on the caller's capacity here. */
  if ((size_t)dt_ccode_n(st->code) * src_len > dst_len) {
    return DT_ERR_ARG;
  }
  return dt_bcjr_encode(st->code, src, (int)src_len, &st->state, &st->unknown,
                        dst);
}

static int bcjr_encoder_finalize(dt_encoder *enc, dt_t *dst, size_t dst_len) {
  bcjr_encoder *st = enc->data;
  /* Flush writes (K-1) * n trailing bits to drain the register back to state 0. */
  if ((size_t)(dt_ccode_k(st->code) - 1) * (size_t)dt_ccode_n(st->code) >
      dst_len) {
    return DT_ERR_ARG;
  }
  return dt_bcjr_encode_flush(st->code, &st->state, &st->unknown, dst);
}

dt_encoder *dt_bcjr_encoder_create(const dt_ccode *code) {
  if (!code) {
    return NULL;
  }
  dt_encoder *enc = dt_malloc(sizeof(*enc));
  bcjr_encoder *st = dt_malloc(sizeof(*st));
  if (!enc || !st) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  st->code = code;
  st->state = 0;
  st->unknown = 0;
  enc->begin = bcjr_encoder_begin;
  enc->encode = bcjr_encoder_encode;
  enc->finalize = bcjr_encoder_finalize;
  enc->data = st;
  return enc;
}

void dt_bcjr_encoder_destroy(dt_encoder *enc) {
  if (!enc) {
    return;
  }
  dt_free(enc->data);
  dt_free(enc);
}

/* -- decoder --------------------------------------------------------------- */

static int bcjr_decoder_begin(dt_decoder *dec, dt_t *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* no preamble to emit */
}

static int bcjr_decoder_decode(dt_decoder *dec, dt_t *dst, size_t dst_len,
                               const dt_t *src, size_t src_len) {
  dt_bcjr_stream_decoder *sd = dec->data;
  /* The hard decoder ignores the per-bit soft output (pass NULL details). */
  return dt_bcjr_stream_decode(sd, src, (int)src_len, dst, NULL, (int)dst_len);
}

static int bcjr_decoder_finalize(dt_decoder *dec, dt_t *dst, size_t dst_len) {
  dt_bcjr_stream_decoder *sd = dec->data;
  return dt_bcjr_stream_decode_flush(sd, dst, NULL, (int)dst_len);
}

dt_decoder *dt_bcjr_decoder_create(const dt_ccode *code,
                                   const dt_bcjr_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_bcjr_stream_decoder *sd = dt_bcjr_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_bcjr_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = bcjr_decoder_begin;
  dec->decode = bcjr_decoder_decode;
  dec->finalize = bcjr_decoder_finalize;
  dec->data = sd;
  return dec;
}

void dt_bcjr_decoder_destroy(dt_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_bcjr_stream_decoder_destroy(dec->data);
  dt_free(dec);
}

/* -- soft decoder ---------------------------------------------------------- */

/* Map the engine's per-bit soft output onto a dt_soft_decoder_out. The fields
 * line up one-to-one: c_lost is the "erasure / unknowable value" consistency,
 * and the engine also reports c_invalid (the slot's coded group was the
 * encoder's DT_INVALID poison marker) and c_absent (1 - c_lock; the slot is not
 * backed by a tracked codeword stream), so both pass straight through. */
static void details_to_soft(const dt_bcjr_decode_details *d,
                            dt_soft_decoder_out *o) {
  o->c_false = d->c_false;
  o->c_true = d->c_true;
  o->c_erasure = d->c_lost;
  o->c_invalid = d->c_invalid;
  o->c_absent = d->c_absent;
  o->c_locked = d->c_lock;
}

/* How many soft outputs to pull from the engine per call - a fixed stack scratch
 * so decode/finalize need no allocation. */
#define BCJR_SOFT_CHUNK 64

static int bcjr_soft_begin(dt_soft_decoder *dec, dt_t *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* no preamble to emit */
}

static int bcjr_soft_decode(dt_soft_decoder *dec, dt_soft_decoder_out *dst,
                            size_t dst_len, const dt_t *src, size_t src_len) {
  dt_bcjr_stream_decoder *sd = dec->data;
  dt_bcjr_decode_details chunk[BCJR_SOFT_CHUNK];
  size_t written = 0;
  int fed = 0; /* feed src on the first collect only; drain on later ones */
  /* do/while, not while: src must be handed to the engine even when dst_len is
   * 0 (a feed-only "pump" call), matching the hard decoder, which buffers the
   * input regardless of how much output is collected. */
  do {
    const size_t remain = dst_len - written;
    const int want = remain > BCJR_SOFT_CHUNK ? BCJR_SOFT_CHUNK : (int)remain;
    const int got = dt_bcjr_stream_decode(sd, fed ? NULL : src,
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

static int bcjr_soft_finalize(dt_soft_decoder *dec, dt_soft_decoder_out *dst,
                              size_t dst_len) {
  dt_bcjr_stream_decoder *sd = dec->data;
  dt_bcjr_decode_details chunk[BCJR_SOFT_CHUNK];
  size_t written = 0;
  while (written < dst_len) {
    const size_t remain = dst_len - written;
    const int want = remain > BCJR_SOFT_CHUNK ? BCJR_SOFT_CHUNK : (int)remain;
    const int got = dt_bcjr_stream_decode_flush(sd, NULL, chunk, want);
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

dt_soft_decoder *dt_bcjr_soft_decoder_create(
    const dt_ccode *code, const dt_bcjr_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_bcjr_stream_decoder *sd = dt_bcjr_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_soft_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_bcjr_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = bcjr_soft_begin;
  dec->decode = bcjr_soft_decode;
  dec->finalize = bcjr_soft_finalize;
  dec->data = sd;
  return dec;
}

void dt_bcjr_soft_decoder_destroy(dt_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_bcjr_stream_decoder_destroy(dec->data);
  dt_free(dec);
}
