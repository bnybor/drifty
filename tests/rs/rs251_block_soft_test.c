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
 * Tests for the rs251 soft block decoder. The soft decoder reads dt_soft_bit per
 * coded bit and, on a decode failure, iteratively erases the least reliable
 * still-known symbol (ranked by c_invalid + c_absent of its worst bit). With
 * RS(20, 12) the hard decoder corrects 4 symbol errors; the soft decoder corrects
 * up to 8 when the soft bits flag the wrong symbols, by erasing them.
 */

#include <drifty/bit.h>
#include <drifty/block_decoder.h>
#include <drifty/block_encoder.h>
#include <drifty/block_soft_decoder.h>
#include <drifty/result.h>
#include <drifty/rs/rs251.h>
#include <drifty/soft_bit.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 20
#define K 12

static int g_failures = 0;

static int check(const char *name, int cond) {
  printf("  [%s] %s\n", name, cond ? "PASS" : "FAIL");
  if (!cond) {
    ++g_failures;
  }
  return cond;
}

/* Bail out of the current test if a required pointer is NULL. */
#define REQUIRE_NONNULL(p)                       \
  do {                                           \
    if (!check("decoder created", (p) != NULL)) { \
      return;                                     \
    }                                             \
  } while (0)

static uint64_t g_rng = 0x1234567890abcdefULL;
static int rand_bit(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (int)((g_rng >> 11) & 1u);
}

/* The current codeword: symbol values and the original message bits. */
static int g_code[N];
static int g_orig[(K - 1) * 8];

/* Read symbol `sym`'s value from a hard encoded buffer (8 bits MSB-first). */
static int hard_sym(const dt_bit *enc, int sym) {
  int v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 1) | (enc[sym * 8 + i] == DT_TRUE ? 1 : 0);
  }
  return v;
}

/* Encode a fresh random message; record its codeword symbols and message bits. */
static int build_codeword(void) {
  dt_block_encoder *enc = dt_rs_rs251_block_encoder_create(N, K);
  if (!enc) {
    return 0;
  }
  const size_t dlen = enc->decoded_len(enc);
  dt_bit *msg = enc->decoded_buf(enc);
  for (size_t i = 0; i < dlen; ++i) {
    const int bit = rand_bit();
    msg[i] = bit ? DT_TRUE : DT_FALSE;
    g_orig[i] = bit;
  }
  const int ok = (enc->encode(enc) == DT_OK);
  const dt_bit *code = enc->encoded_buf(enc);
  for (int s = 0; s < N; ++s) {
    g_code[s] = hard_sym(code, s);
  }
  dt_rs_rs251_block_encoder_destroy(enc);
  return ok;
}

/* Write symbol `sym` = `value` into a soft buffer with c_invalid mass `inv`. The
 * bit reads as its true value (argmax) while inv < 0.5, so the symbol stays
 * "known" but carries unreliability `inv`. */
static void soft_sym(dt_soft_bit *enc, int sym, int value, float inv) {
  for (int i = 0; i < 8; ++i) {
    const int bit = (value >> (7 - i)) & 1;
    dt_soft_bit *sb = &enc[sym * 8 + i];
    memset(sb, 0, sizeof(*sb));
    if (bit) {
      sb->c_true = 1.0f - inv;
    } else {
      sb->c_false = 1.0f - inv;
    }
    sb->c_invalid = inv;
    sb->c_locked = 1.0f;
  }
}

/* Write symbol `sym` as a genuine erasure (c_erasure dominates) into a soft buffer. */
static void soft_sym_erase(dt_soft_bit *enc, int sym) {
  for (int i = 0; i < 8; ++i) {
    dt_soft_bit *sb = &enc[sym * 8 + i];
    memset(sb, 0, sizeof(*sb));
    sb->c_erasure = 1.0f;
  }
}

/* Mirror onto a hard buffer (the argmax of the soft construction above). */
static void hard_sym_set(dt_bit *enc, int sym, int value) {
  for (int i = 0; i < 8; ++i) {
    enc[sym * 8 + i] = ((value >> (7 - i)) & 1) ? DT_TRUE : DT_FALSE;
  }
}
static void hard_sym_erase(dt_bit *enc, int sym) {
  for (int i = 0; i < 8; ++i) {
    enc[sym * 8 + i] = DT_ERASURE;
  }
}

/* Fill a soft encoded buffer: every symbol correct and reliable, then overlay
 * `nerr` wrong-but-flagged error symbols and `nera` genuine erasures. A flagged
 * error carries a *different* valid GF(251) value with c_invalid = `inv`. */
static void fill_soft(dt_soft_bit *enc, const int *err, int nerr, const int *era,
                      int nera, float inv) {
  for (int s = 0; s < N; ++s) {
    soft_sym(enc, s, g_code[s], 0.0f);
  }
  for (int j = 0; j < nerr; ++j) {
    soft_sym(enc, err[j], (g_code[err[j]] + 1) % 251, inv);
  }
  for (int j = 0; j < nera; ++j) {
    soft_sym_erase(enc, era[j]);
  }
}

/* The same corruption on a hard buffer, for the hard-decoder baseline. */
static void fill_hard(dt_bit *enc, const int *err, int nerr, const int *era,
                      int nera) {
  for (int s = 0; s < N; ++s) {
    hard_sym_set(enc, s, g_code[s]);
  }
  for (int j = 0; j < nerr; ++j) {
    hard_sym_set(enc, err[j], (g_code[err[j]] + 1) % 251);
  }
  for (int j = 0; j < nera; ++j) {
    hard_sym_erase(enc, era[j]);
  }
}

static int recovered(const dt_bit *out) {
  for (int i = 0; i < (K - 1) * 8; ++i) {
    if ((out[i] == DT_TRUE ? 1 : 0) != g_orig[i]) {
      return 0;
    }
  }
  return 1;
}

/* -- tests ----------------------------------------------------------------- */

static void test_lengths(void) {
  dt_block_soft_decoder *d = dt_rs_rs251_block_soft_decoder_create(N, K, 0);
  if (!check("create", d != NULL)) {
    return;
  }
  check("decoded_len == 88", d->decoded_len(d) == 88u);     /* (K-1)*8 */
  check("encoded_len == 160", d->encoded_len(d) == 160u);   /* 8*N */
  dt_rs_rs251_block_soft_decoder_destroy(d);

  check("s>n-k rejected",
        dt_rs_rs251_block_soft_decoder_create(N, K, N - K + 1) == NULL);
  dt_block_soft_decoder *dmax = dt_rs_rs251_block_soft_decoder_create(N, K, N - K);
  check("s=n-k ok", dmax != NULL);
  dt_rs_rs251_block_soft_decoder_destroy(dmax);
}

static void test_clean(void) {
  if (!check("clean: build", build_codeword())) {
    return;
  }
  dt_block_soft_decoder *d = dt_rs_rs251_block_soft_decoder_create(N, K, 0);
  REQUIRE_NONNULL(d);
  fill_soft(d->encoded_buf(d), NULL, 0, NULL, 0, 0.0f);
  check("clean: decode OK", d->decode(d) == DT_OK);
  check("clean: recovered", recovered(d->decoded_buf(d)));
  dt_rs_rs251_block_soft_decoder_destroy(d);
}

/* 6 wrong symbols (> hard's 4) flagged unreliable: the soft decoder erases exactly
 * those and recovers, while the hard decoder on the same bits fails. */
static void test_soft_advantage(void) {
  static const int err[6] = {1, 4, 7, 10, 13, 16};
  if (!check("advantage: build", build_codeword())) {
    return;
  }
  dt_block_soft_decoder *d = dt_rs_rs251_block_soft_decoder_create(N, K, 0);
  REQUIRE_NONNULL(d);
  fill_soft(d->encoded_buf(d), err, 6, NULL, 0, 0.4f);
  check("advantage: soft decode OK", d->decode(d) == DT_OK);
  check("advantage: soft recovered", recovered(d->decoded_buf(d)));
  dt_rs_rs251_block_soft_decoder_destroy(d);

  /* Same corruption, hard decoder: 6 errors > (n-k)/2 = 4 -> uncorrectable. */
  dt_block_decoder *h = dt_rs_rs251_block_decoder_create(N, K, 0);
  REQUIRE_NONNULL(h);
  fill_hard(h->encoded_buf(h), err, 6, NULL, 0);
  check("advantage: hard decode fails (DT_ERR_DECODE)",
        h->decode(h) == DT_ERR_DECODE);
  dt_rs_rs251_block_decoder_destroy(h);
}

/* 2 genuine erasures + 4 flagged errors: 2*4 + 2 = 10 > 8 at first, but erasing
 * the 4 flagged errors leaves 6 erasures (<= 8) and recovers. */
static void test_mixed(void) {
  static const int err[4] = {3, 8, 11, 17};
  static const int era[2] = {6, 14};
  if (!check("mixed: build", build_codeword())) {
    return;
  }
  dt_block_soft_decoder *d = dt_rs_rs251_block_soft_decoder_create(N, K, 0);
  REQUIRE_NONNULL(d);
  fill_soft(d->encoded_buf(d), err, 4, era, 2, 0.4f);
  check("mixed: decode OK", d->decode(d) == DT_OK);
  check("mixed: recovered", recovered(d->decoded_buf(d)));
  dt_rs_rs251_block_soft_decoder_destroy(d);
}

/* 9 flagged errors exceed the code: even erasing all that fit (8) cannot decode. */
static void test_uncorrectable(void) {
  static const int err[9] = {0, 2, 4, 6, 8, 10, 12, 14, 16};
  if (!check("uncorrectable: build", build_codeword())) {
    return;
  }
  dt_block_soft_decoder *d = dt_rs_rs251_block_soft_decoder_create(N, K, 0);
  REQUIRE_NONNULL(d);
  fill_soft(d->encoded_buf(d), err, 9, NULL, 0, 0.4f);
  check("uncorrectable: DT_ERR_DECODE", d->decode(d) == DT_ERR_DECODE);
  dt_rs_rs251_block_soft_decoder_destroy(d);
}

int main(void) {
  printf("rs251 soft block decoder, RS(%d, %d):\n", N, K);
  test_lengths();
  test_clean();
  test_soft_advantage();
  test_mixed();
  test_uncorrectable();
  printf("%s\n", g_failures ? "FAILED" : "OK");
  return g_failures ? 1 : 0;
}
