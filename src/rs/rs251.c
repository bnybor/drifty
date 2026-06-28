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
 * rs251 block codec: adapts the internal Reed-Solomon library (contrib/rs251,
 * <rs251/rs251.h>) to the abstract dt_block_encoder / dt_block_decoder
 * interfaces. The factories below are the public surface; the rs251_* engine
 * stays internal.
 *
 * STUB: the codec is constructed and the rs251 code is initialised, but the
 * vtable callbacks are not implemented yet. Filling them in needs the dt_bit
 * <-> GF(251) symbol (gf251_t) mapping to be fixed - rs251 provides
 * rs251_bytes_to_message() / rs251_message_to_bytes() and rs251_message_bytes()
 * for the byte<->symbol packing that the bit buffers will build on.
 */

#include <drifty/rs/rs251.h>

#include <drifty/stdlib.h> /* dt_malloc / dt_free */
#include <rs251/rs251.h>   /* internal RS engine - not re-exported */

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  rs251_codec codec; /* the underlying RS(n, k) code */
  /* TODO: owned dt_bit buffers for the decoded (message) and encoded (codeword)
   * blocks plus their bit lengths, once the dt_bit <-> gf251_t mapping is set. */
} rs251_block_encoder;

static size_t rs251_encoder_decoded_len(dt_block_encoder *enc) {
  (void)enc;
  return 0; /* TODO: k symbols as a bit count */
}

static dt_bit *rs251_encoder_decoded_buf(dt_block_encoder *enc) {
  (void)enc;
  return NULL; /* TODO: the owned decoded (message) buffer */
}

static size_t rs251_encoder_encoded_len(dt_block_encoder *enc) {
  (void)enc;
  return 0; /* TODO: n symbols as a bit count */
}

static dt_bit *rs251_encoder_encoded_buf(dt_block_encoder *enc) {
  (void)enc;
  return NULL; /* TODO: the owned encoded (codeword) buffer */
}

static dt_result rs251_encoder_encode(dt_block_encoder *enc) {
  (void)enc;
  return DT_ERR_ARG; /* TODO: pack the decoded buffer and call rs251_encode() */
}

static dt_result rs251_encoder_reset(dt_block_encoder *enc) {
  (void)enc;
  return DT_OK; /* TODO: clear any in-progress encode state */
}

dt_block_encoder *dt_rs_rs251_block_encoder_create(uint16_t n, uint16_t k) {
  dt_block_encoder *enc = dt_malloc(sizeof(*enc));
  rs251_block_encoder *st = dt_malloc(sizeof(*st));
  if (!enc || !st) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  if (rs251_codec_init(&st->codec, n, k) != RS251_OK) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  enc->decoded_len = rs251_encoder_decoded_len;
  enc->decoded_buf = rs251_encoder_decoded_buf;
  enc->encoded_len = rs251_encoder_encoded_len;
  enc->encoded_buf = rs251_encoder_encoded_buf;
  enc->encode = rs251_encoder_encode;
  enc->reset = rs251_encoder_reset;
  enc->data = st;
  return enc;
}

void dt_rs_rs251_block_encoder_destroy(dt_block_encoder *enc) {
  if (!enc) {
    return;
  }
  dt_free(enc->data);
  dt_free(enc);
}

/* -- decoder --------------------------------------------------------------- */

typedef struct {
  rs251_codec codec; /* the underlying RS(n, k) code */
  /* TODO: owned dt_bit buffers for the encoded (received) and decoded
   * (recovered) blocks plus their bit lengths. */
} rs251_block_decoder;

static size_t rs251_decoder_decoded_len(dt_block_decoder *dec) {
  (void)dec;
  return 0; /* TODO: k symbols as a bit count */
}

static dt_bit *rs251_decoder_decoded_buf(dt_block_decoder *dec) {
  (void)dec;
  return NULL; /* TODO: the owned decoded (recovered) buffer */
}

static size_t rs251_decoder_encoded_len(dt_block_decoder *dec) {
  (void)dec;
  return 0; /* TODO: n symbols as a bit count */
}

static dt_bit *rs251_decoder_encoded_buf(dt_block_decoder *dec) {
  (void)dec;
  return NULL; /* TODO: the owned encoded (received) buffer */
}

static dt_result rs251_decoder_decode(dt_block_decoder *dec) {
  (void)dec;
  return DT_ERR_ARG; /* TODO: pack the received buffer and call rs251_decode() */
}

static dt_result rs251_decoder_reset(dt_block_decoder *dec) {
  (void)dec;
  return DT_OK; /* TODO: clear any in-progress decode state */
}

dt_block_decoder *dt_rs_rs251_block_decoder_create(uint16_t n, uint16_t k) {
  dt_block_decoder *dec = dt_malloc(sizeof(*dec));
  rs251_block_decoder *st = dt_malloc(sizeof(*st));
  if (!dec || !st) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  if (rs251_codec_init(&st->codec, n, k) != RS251_OK) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  dec->decoded_len = rs251_decoder_decoded_len;
  dec->decoded_buf = rs251_decoder_decoded_buf;
  dec->encoded_len = rs251_decoder_encoded_len;
  dec->encoded_buf = rs251_decoder_encoded_buf;
  dec->decode = rs251_decoder_decode;
  dec->reset = rs251_decoder_reset;
  dec->data = st;
  return dec;
}

void dt_rs_rs251_block_decoder_destroy(dt_block_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_free(dec->data);
  dt_free(dec);
}
