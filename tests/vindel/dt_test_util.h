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
 * Shared test utilities for the vindel suite: a small soft-assertion framework
 * plus the PRNG, encode, channel, and decoder helpers the test files have in
 * common. The decoder helpers drive the ported drift_viterbi engine through its
 * private API (vindel/decode.h). Header-only; every helper is `static inline`
 * so a test file that does not use one draws no -Wunused warning.
 */

#ifndef DT_VINDEL_TEST_UTIL_H
#define DT_VINDEL_TEST_UTIL_H

#include <vindel/decode.h>
#include <vindel/encode.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- check framework ------------------------------------------------------- */

/* Failures counted per test executable (each test file is its own program).
 *
 * The check helpers below run inside OpenMP-parallelised test loops, so every
 * one updates `g_failures` and writes its PASS/FAIL line under the same named
 * critical section: the counter stays race-free and lines never tear. Without
 * OpenMP the `#pragma omp critical` is an ignored unknown pragma, so the same
 * source compiles and behaves identically single-threaded. */
static int g_failures = 0;

/* Record a boolean outcome; print PASS/FAIL and return the condition so callers
 * can branch (see REQUIRE). */
static inline int check(const char *name, int cond) {
#pragma omp critical(dt_check)
  {
    printf("  [%s] %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
      ++g_failures;
    }
  }
  return cond;
}

static inline void check_gt(const char *name, double got, double want) {
  int ok = got > want;
#pragma omp critical(dt_check)
  {
    printf("  [%s] %.4f (want > %.4f) %s\n", name, got, want,
           ok ? "PASS" : "FAIL");
    if (!ok) {
      ++g_failures;
    }
  }
}

static inline void check_lt(const char *name, double got, double want) {
  int ok = got < want;
#pragma omp critical(dt_check)
  {
    printf("  [%s] %.4f (want < %.4f) %s\n", name, got, want,
           ok ? "PASS" : "FAIL");
    if (!ok) {
      ++g_failures;
    }
  }
}

/* Fatal precondition: on failure record it and bail out of the current test
 * (which must return void) rather than press on and dereference a NULL. */
#define REQUIRE(name, cond)       \
  do {                            \
    if (!check((name), (cond))) { \
      return;                     \
    }                             \
  } while (0)

/* Print the suite summary and yield a process exit code for main to return. */
static inline int test_summary(const char *suite) {
  if (g_failures) {
    printf("\n%s: %d check(s) FAILED\n", suite, g_failures);
    return 1;
  }
  printf("\nall %s checks passed\n", suite);
  return 0;
}

/* -- deterministic PRNG (splitmix64) --------------------------------------- */

static inline uint64_t rng_next(uint64_t *state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* Map a raw 0/1 (only the low bit is read) to its DT_TRUE/DT_FALSE symbol. */
static inline uint8_t bit_sym(unsigned int v) {
  return (v & 1u) ? DT_TRUE : DT_FALSE;
}

static inline void rand_bits(uint8_t *bits, int n, uint64_t *rng) {
  for (int i = 0; i < n; ++i) {
    bits[i] = bit_sym((unsigned int)rng_next(rng));
  }
}

/* -- encode ---------------------------------------------------------------- */

/* Encode `info_bits` message bits (each DT_FALSE/DT_TRUE) with `code` into
 * out[] (which must hold info_bits * dt_ccode_n(code) + flush slack), including
 * the end-of-stream flush. Returns the coded length. */
static inline int vindel_encode_all(const dt_ccode *code, const uint8_t *msg,
                                    int info_bits, uint8_t *out) {
  int state = 0;
  int len = dt_vindel_encode(code, msg, info_bits, &state, out);
  len += dt_vindel_encode_flush(code, &state, out + len);
  return len;
}

/* -- decoder helpers ------------------------------------------------------- */

/* Build a decoder from positional settings (keeps tests concise). The argument
 * order mirrors the drift_viterbi channel model the engine implements. */
static inline dt_vindel_stream_decoder *make_decoder(const dt_ccode *code,
                                                     int depth, int drift,
                                                     double p_sub, double p_ins,
                                                     double p_del,
                                                     double p_erase) {
  dt_vindel_stream_params params = {
      .decision_depth = depth,
      .max_drift = drift,
      .p_sub = p_sub,
      .p_ins = p_ins,
      .p_del = p_del,
      .p_erase = p_erase,
  };
  return dt_vindel_stream_decoder_create(code, &params);
}

/* Push a whole received buffer through the streaming decoder in small chunks,
 * then drain. Returns the number of decoded bits collected. */
static inline int stream_decode_all(dt_vindel_stream_decoder *sd,
                                    const uint8_t *rx, int rl, uint8_t *out,
                                    int cap) {
  int got = 0;
  for (int pos = 0; pos < rl;) {
    int chunk = (rl - pos < 41) ? (rl - pos) : 41;
    int w = dt_vindel_stream_decode(sd, rx + pos, chunk, out + got, NULL,
                                    cap - got);
    assert(w >= 0);
    got += w;
    pos += chunk;
  }
  for (;;) {
    int w = dt_vindel_stream_decode_flush(sd, out + got, cap - got);
    assert(w >= 0);
    if (w == 0) break;
    got += w;
  }
  return got;
}

/* Encode `msg` (info_bits bits) with `enc`, decode it with a fresh decoder for
 * `dec` at the given decision depth, and return the mean lock probability over
 * the settled second half. When enc != dec this measures whether one code's
 * stream is mistaken for the other's. */
static inline double decoder_lock_mean(const dt_ccode *enc, const dt_ccode *dec,
                                       const uint8_t *msg, int info_bits,
                                       int depth) {
  int clen = info_bits * dt_ccode_n(enc);
  uint8_t *coded = malloc((size_t)clen);
  int st = 0;
  dt_vindel_encode(enc, msg, info_bits, &st, coded);

  dt_vindel_stream_decoder *sd =
      make_decoder(dec, depth, 4, 0.01, 0.01, 0.01, 0.0);
  assert(sd != NULL);
  int cap = info_bits + 64;
  uint8_t *out = malloc((size_t)cap);
  float *lock = malloc((size_t)cap * sizeof(float));
  int got = dt_vindel_stream_decode(sd, coded, clen, out, lock, cap);
  assert(got > 0);

  double sum = 0.0;
  int count = 0;
  for (int i = got / 2; i < got; ++i) {
    sum += lock[i];
    ++count;
  }

  dt_vindel_stream_decoder_destroy(sd);
  free(coded);
  free(out);
  free(lock);
  return count ? sum / count : 0.0;
}

#endif /* DT_VINDEL_TEST_UTIL_H */
