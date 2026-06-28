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
 * Buffer layout (see <drifty/rs/rs251.h>):
 *   - decoded buffer: B = rs251_message_bytes() bytes, each as 8 dt_bit MSB-first.
 *   - encoded buffer: n GF(251) codeword symbols, each as 8 dt_bit MSB-first.
 * Encode packs the decoded bits into B bytes, converts them to k GF(251) symbols
 * (rs251_bytes_to_message), encodes to n symbols (rs251_encode), and writes them
 * out 8 bits per symbol. Decode reverses it; a received symbol whose 8 bits are
 * not all clean booleans, or whose value is not a valid GF(251) symbol (> 250),
 * is fed to the engine as an erasure (RS251_ERASURE).
 */

#include <drifty/rs/rs251.h>

#include <drifty/bit.h>    /* dt_bit, DT_BIT / DT_IS_BIT, DT_TRUE / DT_FALSE */
#include <drifty/stdlib.h> /* dt_malloc / dt_free */
#include <rs251/rs251.h>   /* internal RS engine - not re-exported */

/* -- bit <-> byte / symbol packing ----------------------------------------- */

/* Pack 8 dt_bit (MSB first) into a byte value. A non-boolean bit reads as 0. */
static uint8_t pack_byte(const dt_bit *bits) {
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (uint8_t)((v << 1) | DT_BIT(bits[i]));
  }
  return v;
}

/* Unpack a byte value into 8 dt_bit (MSB first), as DT_TRUE / DT_FALSE. */
static void unpack_byte(uint8_t v, dt_bit *bits) {
  for (int i = 0; i < 8; ++i) {
    bits[i] = (v & 0x80u) ? DT_TRUE : DT_FALSE;
    v = (uint8_t)(v << 1);
  }
}

/* Pack 8 received dt_bit (MSB first) into a GF(251) symbol. Any bit that is not a
 * clean boolean (DT_ERASURE / DT_INVALID / DT_ABSENT), or an assembled value that
 * is not a valid GF(251) symbol (> 250), maps the whole symbol to RS251_ERASURE. */
static gf251_t pack_symbol(const dt_bit *bits) {
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i) {
    if (!DT_IS_BIT(bits[i])) {
      return RS251_ERASURE;
    }
    v = (uint8_t)((v << 1) | DT_BIT(bits[i]));
  }
  return (v > 250u) ? RS251_ERASURE : (gf251_t)v;
}

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  rs251_codec codec; /* the underlying RS(n, k) code */
  dt_bit *decoded;   /* owned input buffer:  B bytes as bits (B*8 dt_bit) */
  dt_bit *encoded;   /* owned output buffer: n symbols as bits (n*8 dt_bit) */
} rs251_block_encoder;

static size_t rs251_encoder_decoded_len(dt_block_encoder *enc) {
  rs251_block_encoder *st = enc->data;
  return (size_t)rs251_message_bytes(&st->codec) * 8u;
}

static dt_bit *rs251_encoder_decoded_buf(dt_block_encoder *enc) {
  rs251_block_encoder *st = enc->data;
  return st->decoded;
}

static size_t rs251_encoder_encoded_len(dt_block_encoder *enc) {
  rs251_block_encoder *st = enc->data;
  return (size_t)st->codec.n * 8u;
}

static dt_bit *rs251_encoder_encoded_buf(dt_block_encoder *enc) {
  rs251_block_encoder *st = enc->data;
  return st->encoded;
}

static dt_result rs251_encoder_encode(dt_block_encoder *enc) {
  rs251_block_encoder *st = enc->data;
  const uint16_t n = st->codec.n;
  const uint16_t b = rs251_message_bytes(&st->codec);
  uint8_t bytes[RS251_MAX_N];
  gf251_t msg[RS251_MAX_N];
  gf251_t code[RS251_MAX_N];

  for (uint16_t i = 0; i < b; ++i) {
    bytes[i] = pack_byte(&st->decoded[(size_t)i * 8u]);
  }
  if (rs251_bytes_to_message(&st->codec, bytes, msg) != RS251_OK ||
      rs251_encode(&st->codec, msg, code) != RS251_OK) {
    return DT_ERR_ARG;
  }
  for (uint16_t i = 0; i < n; ++i) {
    unpack_byte((uint8_t)code[i], &st->encoded[(size_t)i * 8u]);
  }
  return DT_OK;
}

static dt_result rs251_encoder_reset(dt_block_encoder *enc) {
  (void)enc; /* encode reads the whole decoded buffer fresh; no state to reset */
  return DT_OK;
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
  const size_t dec_bytes = (size_t)rs251_message_bytes(&st->codec) * 8u;
  const size_t enc_bytes = (size_t)st->codec.n * 8u;
  st->decoded = dec_bytes ? dt_malloc(dec_bytes) : NULL;
  st->encoded = dt_malloc(enc_bytes); /* n >= 1, so always non-zero */
  if (!st->encoded || (dec_bytes && !st->decoded)) {
    dt_free(st->decoded);
    dt_free(st->encoded);
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
  rs251_block_encoder *st = enc->data;
  dt_free(st->decoded);
  dt_free(st->encoded);
  dt_free(st);
  dt_free(enc);
}

/* -- decoder --------------------------------------------------------------- */

typedef struct {
  rs251_codec codec; /* the underlying RS(n, k) code */
  dt_bit *decoded;   /* owned output buffer: B bytes as bits (B*8 dt_bit) */
  dt_bit *encoded;   /* owned input buffer:  n symbols as bits (n*8 dt_bit) */
} rs251_block_decoder;

static size_t rs251_decoder_decoded_len(dt_block_decoder *dec) {
  rs251_block_decoder *st = dec->data;
  return (size_t)rs251_message_bytes(&st->codec) * 8u;
}

static dt_bit *rs251_decoder_decoded_buf(dt_block_decoder *dec) {
  rs251_block_decoder *st = dec->data;
  return st->decoded;
}

static size_t rs251_decoder_encoded_len(dt_block_decoder *dec) {
  rs251_block_decoder *st = dec->data;
  return (size_t)st->codec.n * 8u;
}

static dt_bit *rs251_decoder_encoded_buf(dt_block_decoder *dec) {
  rs251_block_decoder *st = dec->data;
  return st->encoded;
}

static dt_result rs251_decoder_decode(dt_block_decoder *dec) {
  rs251_block_decoder *st = dec->data;
  const uint16_t n = st->codec.n;
  const uint16_t b = rs251_message_bytes(&st->codec);
  gf251_t recv[RS251_MAX_N];
  gf251_t msg[RS251_MAX_N];
  uint8_t bytes[RS251_MAX_N];

  for (uint16_t i = 0; i < n; ++i) {
    recv[i] = pack_symbol(&st->encoded[(size_t)i * 8u]);
  }
  const rs251_status s = rs251_decode(&st->codec, recv, msg, NULL);
  if (s == RS251_ERR_DECODE) {
    return DT_ERR_DECODE;
  }
  if (s != RS251_OK || rs251_message_to_bytes(&st->codec, msg, bytes) != RS251_OK) {
    return DT_ERR_ARG;
  }
  for (uint16_t i = 0; i < b; ++i) {
    unpack_byte(bytes[i], &st->decoded[(size_t)i * 8u]);
  }
  return DT_OK;
}

static dt_result rs251_decoder_reset(dt_block_decoder *dec) {
  (void)dec; /* decode reads the whole encoded buffer fresh; no state to reset */
  return DT_OK;
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
  const size_t dec_bytes = (size_t)rs251_message_bytes(&st->codec) * 8u;
  const size_t enc_bytes = (size_t)st->codec.n * 8u;
  st->decoded = dec_bytes ? dt_malloc(dec_bytes) : NULL;
  st->encoded = dt_malloc(enc_bytes); /* n >= 1, so always non-zero */
  if (!st->encoded || (dec_bytes && !st->decoded)) {
    dt_free(st->decoded);
    dt_free(st->encoded);
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
  rs251_block_decoder *st = dec->data;
  dt_free(st->decoded);
  dt_free(st->encoded);
  dt_free(st);
  dt_free(dec);
}
