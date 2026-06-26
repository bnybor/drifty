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
 * Hybrid codec: realizes the abstract dt_encoder / dt_decoder interfaces over
 * the convolutional encode / drift-tolerant stream-decode engine. The code
 * handle is dt_ccode throughout.
 */

#include <drifty/hybrid.h>

#include <drifty/hybrid/decode.h> /* dt_stream_decoder + dt_stream_decode* */
#include <drifty/stdlib.h>

/* dt_t is uint8_t (bit.h), the same element type the engine's encode/decode
 * buffers use, so the dt_t* <-> uint8_t* hand-offs below need no conversion. */

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  const dt_ccode *code; /* the convolutional code this encoder emits */
  int state;            /* running shift-register state across encode calls */
} hybrid_encoder;

static int hybrid_encoder_begin(dt_encoder *enc, dt_t *dst, size_t dst_len) {
  hybrid_encoder *st = enc->data;
  st->state = 0; /* fresh stream; the convolutional encoder needs no preamble */
  (void)dst;
  (void)dst_len;
  return 0;
}

static int hybrid_encoder_encode(dt_encoder *enc, dt_t *dst, size_t dst_len,
                                 const dt_t *src, size_t src_len) {
  hybrid_encoder *st = enc->data;
  /* The engine writes src_len * n coded bits and does not bound-check, so gate
   * it on the caller's capacity here. */
  if ((size_t)dt_ccode_n(st->code) * src_len > dst_len) {
    return DT_ERR_ARG;
  }
  return dt_ccode_encode(st->code, src, (int)src_len, &st->state, dst);
}

static int hybrid_encoder_finalize(dt_encoder *enc, dt_t *dst, size_t dst_len) {
  hybrid_encoder *st = enc->data;
  /* Flush writes (K-1) * n trailing bits to drain the register back to state 0. */
  if ((size_t)(dt_ccode_k(st->code) - 1) * (size_t)dt_ccode_n(st->code) >
      dst_len) {
    return DT_ERR_ARG;
  }
  return dt_ccode_encode_flush(st->code, &st->state, dst);
}

dt_encoder *dt_hybrid_encoder_create(const dt_ccode *code) {
  if (!code) {
    return NULL;
  }
  dt_encoder *enc = dt_malloc(sizeof(*enc));
  hybrid_encoder *st = dt_malloc(sizeof(*st));
  if (!enc || !st) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  st->code = code;
  st->state = 0;
  enc->begin = hybrid_encoder_begin;
  enc->encode = hybrid_encoder_encode;
  enc->finalize = hybrid_encoder_finalize;
  enc->data = st;
  return enc;
}

void dt_hybrid_encoder_destroy(dt_encoder *enc) {
  if (!enc) {
    return;
  }
  dt_free(enc->data);
  dt_free(enc);
}

/* -- decoder --------------------------------------------------------------- */

static int hybrid_decoder_begin(dt_decoder *dec, dt_t *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* the stream decoder self-acquires; no preamble to emit */
}

static int hybrid_decoder_decode(dt_decoder *dec, dt_t *dst, size_t dst_len,
                                 const dt_t *src, size_t src_len) {
  dt_stream_decoder *sd = dec->data;
  return dt_stream_decode(sd, src, (int)src_len, dst, NULL, (int)dst_len);
}

static int hybrid_decoder_finalize(dt_decoder *dec, dt_t *dst, size_t dst_len) {
  dt_stream_decoder *sd = dec->data;
  return dt_stream_decode_flush(sd, dst, NULL, (int)dst_len);
}

dt_decoder *dt_hybrid_decoder_create(const dt_ccode *code,
                                     const dt_hybrid_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  dt_stream_decoder *sd = dt_stream_decoder_create(code, params);
  if (!sd) {
    return NULL;
  }
  dt_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = hybrid_decoder_begin;
  dec->decode = hybrid_decoder_decode;
  dec->finalize = hybrid_decoder_finalize;
  dec->data = sd;
  return dec;
}

void dt_hybrid_decoder_destroy(dt_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_stream_decoder_destroy(dec->data);
  dt_free(dec);
}
