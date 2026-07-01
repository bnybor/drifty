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
 * Shared scaffolding for the drifty examples. NONE of this is part of drifty -
 * it is just the boring bits every example needs: a deterministic PRNG, channel
 * simulators (flip / erase / insert / delete), symbol-printing helpers, and the
 * stream encode/decode drive loops. Each example #includes this so its body can
 * stay focused on the drifty API it demonstrates.
 *
 * The drive helpers (ex_encode / ex_decode_hard / ex_decode_soft) show the codec
 * lifecycle every streaming codec follows:
 *
 *   encoder: begin -> encode -> finalize
 *   decoder: begin -> decode (feed) -> decode (drain, src_len 0)* -> finalize
 *
 * They are called through the public vtables (dt_stream_encoder / _decoder /
 * _soft_decoder), so they work for ANY cc codec unchanged - only the _create call
 * differs per codec. Output trails input by ~decision_depth bits (the look-ahead
 * delay), so the first ~decision_depth recovered symbols are warm-up: discard them
 * or send a known preamble. These helpers feed the whole stream at once for
 * brevity; feeding it in chunks works identically because the decoder is stateful.
 */

#ifndef DRIFTY_EXAMPLES_UTIL_H
#define DRIFTY_EXAMPLES_UTIL_H

#include <drifty/bit.h>
#include <drifty/cc/ccode.h>
#include <drifty/cc/encoder.h>
#include <drifty/soft_bit.h>
#include <drifty/stream_decoder.h>
#include <drifty/stream_encoder.h>
#include <drifty/stream_soft_decoder.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- deterministic PRNG (splitmix64), so every run is reproducible ---------- */

static inline uint64_t ex_rng_next(uint64_t *s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
static inline double ex_rng_unit(uint64_t *s) {
  return (double)(ex_rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* A raw 0/1 as its DT_TRUE / DT_FALSE symbol. */
static inline dt_bit ex_bit_sym(unsigned int v) {
  return (v & 1u) ? DT_TRUE : DT_FALSE;
}
static inline void ex_rand_bits(dt_bit *bits, int n, uint64_t *rng) {
  for (int i = 0; i < n; ++i) {
    bits[i] = ex_bit_sym((unsigned int)ex_rng_next(rng));
  }
}

/* Pack a byte into 8 dt_bit symbols, MSB first - the byte<->symbol boundary the block
 * codes sit on (a GF(251) symbol is one byte). */
static inline void ex_byte_to_bits(unsigned char v, dt_bit *out) {
  for (int i = 0; i < 8; ++i) {
    out[i] = (v & (0x80u >> i)) ? DT_TRUE : DT_FALSE;
  }
}
/* Unpack 8 dt_bit symbols (MSB first) back into a byte. Returns 0 if any symbol is not
 * a clean bit (erasure / absent / invalid), so the byte cannot be recovered. */
static inline int ex_bits_to_byte(const dt_bit *in, unsigned char *out) {
  unsigned v = 0;
  for (int i = 0; i < 8; ++i) {
    if (!DT_IS_BIT(in[i])) {
      return 0;
    }
    v = (v << 1) | DT_BIT(in[i]);
  }
  *out = (unsigned char)v;
  return 1;
}

/* -- one-line symbol name, for readable output ------------------------------ */

static inline const char *ex_sym(dt_bit s) {
  switch (s) {
  case DT_TRUE: return "1";
  case DT_FALSE: return "0";
  case DT_ERASURE: return "E"; /* present, value unknown */
  case DT_INVALID: return "I"; /* deliberate non-value (poison) */
  case DT_ABSENT: return "A";  /* decoder could not place this position */
  default: return "?";
  }
}

/* -- channel simulators ----------------------------------------------------- */
/* These corrupt a coded stream the way a real channel would, so the decoders
 * have something to correct. flip/erase work in place; insert/delete change the
 * length and write into out[] (which must hold up to 2*len for insert). */

/* Flip each bit (DT_TRUE <-> DT_FALSE) with probability p, in place. */
static inline void ex_flip(dt_bit *buf, int len, double p, uint64_t *rng) {
  for (int i = 0; i < len; ++i) {
    if (DT_IS_BIT(buf[i]) && ex_rng_unit(rng) < p) {
      buf[i] ^= DT_VALUE; /* toggle the value bit */
    }
  }
}
/* Mark each bit DT_ERASURE (present, value lost) with probability p, in place. */
static inline void ex_erase(dt_bit *buf, int len, double p, uint64_t *rng) {
  for (int i = 0; i < len; ++i) {
    if (ex_rng_unit(rng) < p) {
      buf[i] = DT_ERASURE;
    }
  }
}
/* Insert a spurious random bit before each bit with probability p. Returns the
 * new length; out[] must hold up to 2*len bits. */
static inline int ex_insert(const dt_bit *in, int len, double p, uint64_t *rng,
                            dt_bit *out) {
  int o = 0;
  for (int i = 0; i < len; ++i) {
    if (ex_rng_unit(rng) < p) {
      out[o++] = ex_bit_sym((unsigned int)ex_rng_next(rng));
    }
    out[o++] = in[i];
  }
  return o;
}
/* Drop each bit with probability p. Returns the new (shorter) length. */
static inline int ex_delete(const dt_bit *in, int len, double p, uint64_t *rng,
                            dt_bit *out) {
  int o = 0;
  for (int i = 0; i < len; ++i) {
    if (ex_rng_unit(rng) < p) {
      continue;
    }
    out[o++] = in[i];
  }
  return o;
}

/* -- stream encode / decode drive loops (public vtables) -------------------- */

/* Encode n_info input bits with `code` through the shared cc encoder. Writes the
 * coded stream to out[] (size it (n_info + K) * n + 64) and returns its length. */
static inline int ex_encode(const dt_cc_code *code, const dt_bit *msg,
                            int n_info, dt_bit *out, int cap) {
  dt_stream_encoder *enc = dt_cc_encoder_create(code);
  if (!enc) {
    return -1;
  }
  int len = enc->begin(enc, out, cap);          /* preamble (none for cc) */
  len += enc->encode(enc, out + len, cap - len, msg, n_info);
  len += enc->finalize(enc, out + len, cap - len); /* flush the shift register */
  dt_cc_encoder_destroy(enc);
  return len;
}

/* Drive an already-created hard decoder over the whole received stream and
 * collect recovered symbols into out[]. Returns the number recovered. */
static inline int ex_decode_hard(dt_stream_decoder *dec, const dt_bit *rx,
                                 int rlen, dt_bit *out, int cap) {
  int got = dec->begin(dec, NULL, 0); /* cc decoders self-acquire: no preamble */
  got += dec->decode(dec, out + got, cap - got, rx, rlen);
  for (;;) { /* drain whatever stayed buffered (no new input) */
    int g = dec->decode(dec, out + got, cap - got, NULL, 0);
    if (g <= 0) {
      break;
    }
    got += g;
  }
  got += dec->finalize(dec, out + got, cap - got); /* flush the in-flight tail */
  return got;
}

/* Same, for a soft decoder: collects dt_soft_bit records. */
static inline int ex_decode_soft(dt_stream_soft_decoder *dec, const dt_bit *rx,
                                 int rlen, dt_soft_bit *out, int cap) {
  int got = dec->begin(dec, NULL, 0);
  got += dec->decode(dec, out + got, cap - got, rx, rlen);
  for (;;) {
    int g = dec->decode(dec, out + got, cap - got, NULL, 0);
    if (g <= 0) {
      break;
    }
    got += g;
  }
  got += dec->finalize(dec, out + got, cap - got);
  return got;
}

/* Hard projection of a soft record, by the decoders' own recoverability-first
 * rule (so it matches what the hard decoder emits, including on ties): not locked
 * -> the position cannot be placed (DT_ABSENT); otherwise a determinable value
 * wins; otherwise an undeterminable tie abstains as DT_INVALID (its coded group
 * was the encoder's poison) or DT_ERASURE. A plain argmax would mis-resolve the
 * ties, where c_false and c_invalid (say) both read 1.0. */
static inline dt_bit ex_hard_of(dt_soft_bit b) {
  if (b.c_locked < 0.5f) {
    return DT_ABSENT;
  }
  if (b.c_erasure >= b.c_true && b.c_erasure >= b.c_false) {
    return b.c_invalid > 0.5f ? DT_INVALID : DT_ERASURE;
  }
  return b.c_true >= b.c_false ? DT_TRUE : DT_FALSE;
}

/* Count positions where recovered[i] != truth[i] over [lo, hi). */
static inline int ex_count_errors(const dt_bit *recovered, const dt_bit *truth,
                                  int lo, int hi) {
  int e = 0;
  for (int i = lo; i < hi; ++i) {
    if (recovered[i] != truth[i]) {
      ++e;
    }
  }
  return e;
}

#endif /* DRIFTY_EXAMPLES_UTIL_H */
