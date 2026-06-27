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
 * Basic encoder: realizes the abstract dt_encoder interface over a standalone
 * plain convolutional encode engine (encode.c in this directory). The code
 * handle is dt_cc_code throughout. Unlike the per-codec encoders this one is
 * self-contained - it shares no engine with viterbi or any other codec.
 *
 * The encode engine is complete; this file is just the vtable plumbing that
 * adapts it to the abstract interface.
 */

#include <drifty/cc/encoders.h>

#include "encode.h" /* dt_cc_basic_encoder_encode + dt_cc_basic_encoder_flush */
#include <drifty/stdlib.h>

/* dt_bit is uint8_t (bit.h), the same element type the engine's encode buffers
 * use, so the dt_bit* <-> uint8_t* hand-offs below need no conversion. */

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  const dt_cc_code *code; /* the convolutional code this encoder emits */
  int state;            /* running shift-register state across encode calls */
} cc_basic_encoder;

static int cc_basic_encoder_begin(dt_encoder *enc, dt_bit *dst, size_t dst_len) {
  cc_basic_encoder *st = enc->data;
  st->state = 0; /* fresh stream; the convolutional encoder needs no preamble */
  (void)dst;
  (void)dst_len;
  return 0;
}

static int cc_basic_encoder_encode(dt_encoder *enc, dt_bit *dst, size_t dst_len,
                                   const dt_bit *src, size_t src_len) {
  cc_basic_encoder *st = enc->data;
  /* The engine writes src_len * n coded bits and does not bound-check, so gate
   * it on the caller's capacity here. */
  if ((size_t)dt_cc_code_n(st->code) * src_len > dst_len) {
    return DT_CC_ERR_ARG;
  }
  return dt_cc_basic_encoder_encode(st->code, src, (int)src_len, &st->state, dst);
}

static int cc_basic_encoder_finalize(dt_encoder *enc, dt_bit *dst, size_t dst_len) {
  cc_basic_encoder *st = enc->data;
  /* Flush writes (K-1) * n trailing bits to drain the register back to state 0. */
  if ((size_t)(dt_cc_code_k(st->code) - 1) * (size_t)dt_cc_code_n(st->code) >
      dst_len) {
    return DT_CC_ERR_ARG;
  }
  return dt_cc_basic_encoder_flush(st->code, &st->state, dst);
}

dt_encoder *dt_cc_basic_encoder_create(const dt_cc_code *code) {
  if (!code) {
    return NULL;
  }
  dt_encoder *enc = dt_malloc(sizeof(*enc));
  cc_basic_encoder *st = dt_malloc(sizeof(*st));
  if (!enc || !st) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  st->code = code;
  st->state = 0;
  enc->begin = cc_basic_encoder_begin;
  enc->encode = cc_basic_encoder_encode;
  enc->finalize = cc_basic_encoder_finalize;
  enc->data = st;
  return enc;
}

void dt_cc_basic_encoder_destroy(dt_encoder *enc) {
  if (!enc) {
    return;
  }
  dt_free(enc->data);
  dt_free(enc);
}
