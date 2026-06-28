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

#include <drifty/bit.h>      /* dt_bit, DT_BIT / DT_IS_BIT, DT_TRUE / DT_FALSE */
#include <drifty/soft_bit.h> /* dt_soft_bit */
#include <drifty/stdlib.h>   /* dt_malloc / dt_free */
#include <rs251/rs251.h>     /* internal RS engine - not re-exported */

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

/* Pack 8 received soft bits (MSB first) into a GF(251) symbol, and report the
 * symbol's unreliability in *unrel. Each bit's hard reading is the argmax over the
 * output alphabet, recoverability-first: a clean boolean iff the larger of
 * {c_false, c_true} is at least the larger of {c_erasure, c_invalid, c_absent}.
 * As in pack_symbol(), any non-boolean bit or an assembled value > 250 makes the
 * whole symbol RS251_ERASURE. The unreliability is the symbol's *least reliable*
 * constituent bit - the largest (c_invalid + c_absent) over the 8 bits - so the
 * worst bit gates the symbol. */
static gf251_t soft_pack_symbol(const dt_soft_bit *bits, float *unrel) {
  uint8_t v = 0;
  int erased = 0;
  float worst = 0.0f;
  for (int i = 0; i < 8; ++i) {
    const dt_soft_bit *sb = &bits[i];
    const float bit_unrel = sb->c_invalid + sb->c_absent;
    if (bit_unrel > worst) {
      worst = bit_unrel;
    }
    const float val = sb->c_true >= sb->c_false ? sb->c_true : sb->c_false;
    float non = sb->c_erasure;
    if (sb->c_invalid > non) {
      non = sb->c_invalid;
    }
    if (sb->c_absent > non) {
      non = sb->c_absent;
    }
    if (val >= non) { /* a clean boolean (recoverability-first ties to a value) */
      v = (uint8_t)((v << 1) | (sb->c_true >= sb->c_false ? 1u : 0u));
    } else {
      erased = 1;
    }
  }
  *unrel = worst;
  return (erased || v > 250u) ? RS251_ERASURE : (gf251_t)v;
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
  uint16_t spare;    /* spare (unspent) check symbols required: reject a decode
                      * spending more than (n - k) - spare of the budget */
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

  uint16_t erasures = 0;
  for (uint16_t i = 0; i < n; ++i) {
    recv[i] = pack_symbol(&st->encoded[(size_t)i * 8u]);
    if (recv[i] == RS251_ERASURE) {
      ++erasures;
    }
  }
  uint16_t errors = 0;
  const rs251_status s = rs251_decode(&st->codec, recv, msg, &errors);
  if (s == RS251_ERR_DECODE) {
    return DT_ERR_DECODE;
  }
  if (s != RS251_OK) {
    return DT_ERR_ARG;
  }
  /* The recovered codeword is valid algebra, but if its message value does not
   * fit in b bytes it was not a real encoding of one (a miscorrection or
   * non-codeword input): a decode failure, not a bad argument. */
  if (rs251_message_to_bytes(&st->codec, msg, bytes) != RS251_OK) {
    return DT_ERR_DECODE;
  }
  /* A block spends 2*errors + erasures of its n - k check symbols; require at
   * least `spare` of them to be left unspent, else reject the (algebraically
   * valid) decode to guard against silent miscorrection. */
  const uint16_t budget = (uint16_t)(st->codec.n - st->codec.k);
  const uint32_t spent = (uint32_t)2u * errors + erasures;
  if (spent + st->spare > budget) {
    return DT_ERR_DECODE;
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

dt_block_decoder *dt_rs_rs251_block_decoder_create(uint16_t n, uint16_t k,
                                                   uint16_t s) {
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
  /* `s` spare check symbols must fit within the n - k budget. (codec_init has
   * already validated k <= n, so n - k does not underflow.) */
  if (s > (uint16_t)(st->codec.n - st->codec.k)) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  st->spare = s;
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

/* -- soft decoder ---------------------------------------------------------- */

typedef struct {
  rs251_codec codec;      /* the underlying RS(n, k) code */
  uint16_t spare;         /* spare (unspent) check symbols required */
  dt_bit *decoded;        /* owned output buffer: B bytes as bits (B*8 dt_bit) */
  dt_soft_bit *encoded;   /* owned input buffer:  n symbols as soft bits (n*8) */
} rs251_block_soft_decoder;

static size_t rs251_soft_decoder_decoded_len(dt_block_soft_decoder *dec) {
  rs251_block_soft_decoder *st = dec->data;
  return (size_t)rs251_message_bytes(&st->codec) * 8u;
}

static dt_bit *rs251_soft_decoder_decoded_buf(dt_block_soft_decoder *dec) {
  rs251_block_soft_decoder *st = dec->data;
  return st->decoded;
}

static size_t rs251_soft_decoder_encoded_len(dt_block_soft_decoder *dec) {
  rs251_block_soft_decoder *st = dec->data;
  return (size_t)st->codec.n * 8u;
}

static dt_soft_bit *rs251_soft_decoder_encoded_buf(dt_block_soft_decoder *dec) {
  rs251_block_soft_decoder *st = dec->data;
  return st->encoded;
}

static dt_result rs251_soft_decoder_decode(dt_block_soft_decoder *dec) {
  rs251_block_soft_decoder *st = dec->data;
  const uint16_t n = st->codec.n;
  const uint16_t b = rs251_message_bytes(&st->codec);
  const uint16_t budget = (uint16_t)(st->codec.n - st->codec.k);
  gf251_t recv[RS251_MAX_N];
  gf251_t msg[RS251_MAX_N];
  uint8_t bytes[RS251_MAX_N];
  float score[RS251_MAX_N]; /* per-symbol unreliability (least-reliable bit) */

  uint16_t erasures = 0;
  for (uint16_t i = 0; i < n; ++i) {
    recv[i] = soft_pack_symbol(&st->encoded[(size_t)i * 8u], &score[i]);
    if (recv[i] == RS251_ERASURE) {
      ++erasures;
    }
  }

  /* Decode; while it fails, erase the least reliable still-known symbol (the one
   * whose worst constituent bit reads most as poison/absent) and retry, trading a
   * wrong symbol for an erasure until 2*errors + erasures fits the n - k budget. */
  for (;;) {
    uint16_t errors = 0;
    const rs251_status s = rs251_decode(&st->codec, recv, msg, &errors);
    if (s == RS251_ERR_PARAMS) {
      return DT_ERR_ARG;
    }
    if (s == RS251_OK) {
      const uint32_t spent = (uint32_t)2u * errors + erasures;
      if (spent + st->spare <= budget &&
          rs251_message_to_bytes(&st->codec, msg, bytes) == RS251_OK) {
        for (uint16_t i = 0; i < b; ++i) {
          unpack_byte(bytes[i], &st->decoded[(size_t)i * 8u]);
        }
        return DT_OK;
      }
      /* Algebraically OK but rejected (marginal spare, or a miscorrection that does
       * not map back to a message): keep erasing rather than accept it. */
    }
    /* RS251_ERR_DECODE, or OK-but-rejected: one more erasure can only help if it
     * keeps the count within the n - k budget. */
    if (erasures >= budget) {
      return DT_ERR_DECODE;
    }
    int victim = -1;
    float worst = -1.0f;
    for (uint16_t i = 0; i < n; ++i) {
      if (recv[i] != RS251_ERASURE && score[i] > worst) {
        worst = score[i];
        victim = (int)i;
      }
    }
    if (victim < 0) {
      return DT_ERR_DECODE; /* no known symbol left to erase */
    }
    recv[victim] = RS251_ERASURE;
    ++erasures;
  }
}

static dt_result rs251_soft_decoder_reset(dt_block_soft_decoder *dec) {
  (void)dec; /* decode reads the whole encoded buffer fresh; no state to reset */
  return DT_OK;
}

dt_block_soft_decoder *dt_rs_rs251_block_soft_decoder_create(uint16_t n,
                                                             uint16_t k,
                                                             uint16_t s) {
  dt_block_soft_decoder *dec = dt_malloc(sizeof(*dec));
  rs251_block_soft_decoder *st = dt_malloc(sizeof(*st));
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
  if (s > (uint16_t)(st->codec.n - st->codec.k)) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  st->spare = s;
  const size_t dec_bytes = (size_t)rs251_message_bytes(&st->codec) * 8u;
  const size_t enc_recs = (size_t)st->codec.n * 8u; /* one soft bit per coded bit */
  st->decoded = dec_bytes ? dt_malloc(dec_bytes) : NULL;
  st->encoded = dt_malloc(enc_recs * sizeof(dt_soft_bit)); /* n >= 1, non-zero */
  if (!st->encoded || (dec_bytes && !st->decoded)) {
    dt_free(st->decoded);
    dt_free(st->encoded);
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  dec->decoded_len = rs251_soft_decoder_decoded_len;
  dec->decoded_buf = rs251_soft_decoder_decoded_buf;
  dec->encoded_len = rs251_soft_decoder_encoded_len;
  dec->encoded_buf = rs251_soft_decoder_encoded_buf;
  dec->decode = rs251_soft_decoder_decode;
  dec->reset = rs251_soft_decoder_reset;
  dec->data = st;
  return dec;
}

void dt_rs_rs251_block_soft_decoder_destroy(dt_block_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  rs251_block_soft_decoder *st = dec->data;
  dt_free(st->decoded);
  dt_free(st->encoded);
  dt_free(st);
  dt_free(dec);
}
