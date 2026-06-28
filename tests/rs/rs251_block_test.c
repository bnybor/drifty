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
 * Round-trip tests for the rs251 block codec. A systematic RS(N, K) over GF(251)
 * corrects 2*errors + erasures <= N - K. Here N=20, K=12, so N-K=8: up to 8
 * erasures, or 4 errors, are recoverable; 9 erasures is not.
 */

#include <drifty/bit.h>
#include <drifty/block_decoder.h>
#include <drifty/block_encoder.h>
#include <drifty/result.h>
#include <drifty/rs/rs251.h>

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

/* A small deterministic PRNG so runs are reproducible. */
static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
static int rand_bit(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (int)((g_rng >> 11) & 1u);
}

/* -- helpers on the encoded buffer (n symbols, 8 bits MSB-first each) ------- */

static int sym_get(const dt_bit *enc, int sym) {
  int v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 1) | (enc[sym * 8 + i] == DT_TRUE ? 1 : 0);
  }
  return v;
}

static void sym_put(dt_bit *enc, int sym, int v) {
  for (int i = 0; i < 8; ++i) {
    enc[sym * 8 + i] = (v & 0x80) ? DT_TRUE : DT_FALSE;
    v <<= 1;
  }
}

static void sym_erase(dt_bit *enc, int sym) {
  for (int i = 0; i < 8; ++i) {
    enc[sym * 8 + i] = DT_ERASURE;
  }
}

/* Build an encoder + decoder for RS(N, K), fill a random message into the
 * encoder's decoded buffer, encode, and copy the codeword into the decoder's
 * encoded buffer. The original message bits are saved to *orig (malloc'd). The
 * encoder is destroyed. Returns 1 iff setup + encode succeeded. */
static int setup(dt_block_decoder **dec_out, dt_bit **orig_out, size_t *dlen_out) {
  dt_block_encoder *enc = dt_rs_rs251_block_encoder_create(N, K);
  dt_block_decoder *dec = dt_rs_rs251_block_decoder_create(N, K);
  if (!enc || !dec) {
    dt_rs_rs251_block_encoder_destroy(enc);
    dt_rs_rs251_block_decoder_destroy(dec);
    return 0;
  }
  const size_t dlen = enc->decoded_len(enc);
  const size_t elen = enc->encoded_len(enc);
  dt_bit *din = enc->decoded_buf(enc);
  dt_bit *orig = malloc(dlen);
  for (size_t i = 0; i < dlen; ++i) {
    din[i] = rand_bit() ? DT_TRUE : DT_FALSE;
    orig[i] = din[i];
  }
  const int ok = (enc->encode(enc) == DT_OK);
  memcpy(dec->encoded_buf(dec), enc->encoded_buf(enc), elen);
  dt_rs_rs251_block_encoder_destroy(enc);
  *dec_out = dec;
  *orig_out = orig;
  *dlen_out = dlen;
  return ok;
}

static int recovered(dt_block_decoder *dec, const dt_bit *orig, size_t dlen) {
  return memcmp(dec->decoded_buf(dec), orig, dlen) == 0;
}

/* -- tests ----------------------------------------------------------------- */

static void test_lengths(void) {
  dt_block_encoder *enc = dt_rs_rs251_block_encoder_create(N, K);
  if (!check("create encoder", enc != NULL)) {
    return;
  }
  /* B = bytes that fit in K symbols = K-1 = 11 -> 88 decoded bits; 8*N encoded. */
  check("decoded_len == 88", enc->decoded_len(enc) == 88u);
  check("encoded_len == 8*N", enc->encoded_len(enc) == (size_t)8 * N);
  check("bad params reject", dt_rs_rs251_block_encoder_create(5, 9) == NULL);
  dt_rs_rs251_block_encoder_destroy(enc);
}

static void test_clean(void) {
  dt_block_decoder *dec;
  dt_bit *orig;
  size_t dlen;
  if (!check("clean: setup+encode", setup(&dec, &orig, &dlen))) {
    return;
  }
  check("clean: decode OK", dec->decode(dec) == DT_OK);
  check("clean: recovered", recovered(dec, orig, dlen));
  free(orig);
  dt_rs_rs251_block_decoder_destroy(dec);
}

static void test_erasures(void) {
  dt_block_decoder *dec;
  dt_bit *orig;
  size_t dlen;
  if (!check("erasure: setup+encode", setup(&dec, &orig, &dlen))) {
    return;
  }
  dt_bit *rx = dec->encoded_buf(dec);
  for (int s = 0; s < N - K; ++s) { /* N-K=8 erasures, the limit */
    sym_erase(rx, s);
  }
  check("erasure: decode OK", dec->decode(dec) == DT_OK);
  check("erasure: recovered", recovered(dec, orig, dlen));
  free(orig);
  dt_rs_rs251_block_decoder_destroy(dec);
}

static void test_invalid_groups(void) {
  dt_block_decoder *dec;
  dt_bit *orig;
  size_t dlen;
  if (!check("invalid: setup+encode", setup(&dec, &orig, &dlen))) {
    return;
  }
  dt_bit *rx = dec->encoded_buf(dec);
  for (int s = 0; s < N - K; ++s) { /* 0xFF > 250 -> not a valid GF(251) symbol */
    sym_put(rx, s, 0xFF);
  }
  check("invalid: decode OK (treated as erasures)", dec->decode(dec) == DT_OK);
  check("invalid: recovered", recovered(dec, orig, dlen));
  free(orig);
  dt_rs_rs251_block_decoder_destroy(dec);
}

static void test_errors(void) {
  dt_block_decoder *dec;
  dt_bit *orig;
  size_t dlen;
  if (!check("error: setup+encode", setup(&dec, &orig, &dlen))) {
    return;
  }
  dt_bit *rx = dec->encoded_buf(dec);
  for (int s = 0; s < (N - K) / 2; ++s) { /* 4 errors: 2*4 = 8 <= N-K */
    sym_put(rx, s, (sym_get(rx, s) + 1) % 251); /* a different valid symbol */
  }
  check("error: decode OK", dec->decode(dec) == DT_OK);
  check("error: recovered", recovered(dec, orig, dlen));
  free(orig);
  dt_rs_rs251_block_decoder_destroy(dec);
}

static void test_uncorrectable(void) {
  dt_block_decoder *dec;
  dt_bit *orig;
  size_t dlen;
  if (!check("uncorrectable: setup+encode", setup(&dec, &orig, &dlen))) {
    return;
  }
  dt_bit *rx = dec->encoded_buf(dec);
  for (int s = 0; s < N - K + 1; ++s) { /* 9 erasures > N-K */
    sym_erase(rx, s);
  }
  check("uncorrectable: returns DT_ERR_DECODE", dec->decode(dec) == DT_ERR_DECODE);
  free(orig);
  dt_rs_rs251_block_decoder_destroy(dec);
}

int main(void) {
  printf("rs251 block codec, RS(%d, %d):\n", N, K);
  test_lengths();
  test_clean();
  test_erasures();
  test_invalid_groups();
  test_errors();
  test_uncorrectable();
  printf("%s\n", g_failures ? "FAILED" : "OK");
  return g_failures ? 1 : 0;
}
