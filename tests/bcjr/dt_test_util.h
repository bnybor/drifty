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
 * Shared test utilities for the bcjr suite: a small soft-assertion framework
 * plus the PRNG and encode helpers the test files have in common. The encoder is
 * fully implemented; the decoders are stubs, so the helpers here cover the
 * encoder and leave decode exercising to the test bodies. Header-only; every
 * helper is `static inline` so a test file that does not use one draws no
 * -Wunused warning.
 */

#ifndef DT_BCJR_TEST_UTIL_H
#define DT_BCJR_TEST_UTIL_H

#include <bcjr/decode.h>
#include <bcjr/encode.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- check framework ------------------------------------------------------- */

/* Failures counted per test executable (each test file is its own program). */
static int g_failures = 0;

static inline int check(const char *name, int cond) {
  printf("  [%s] %s\n", name, cond ? "PASS" : "FAIL");
  if (!cond) {
    ++g_failures;
  }
  return cond;
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

/* Encode `info_bits` message bits (each DT_FALSE/DT_TRUE) with `code` into out[]
 * (which must hold info_bits * dt_ccode_n(code) + flush slack), including the
 * end-of-stream flush. Returns the coded length. */
static inline int bcjr_encode_all(const dt_ccode *code, const uint8_t *msg,
                                  int info_bits, uint8_t *out) {
  int state = 0;
  int len = dt_bcjr_encode(code, msg, info_bits, &state, out);
  len += dt_bcjr_encode_flush(code, &state, out + len);
  return len;
}

#endif /* DT_BCJR_TEST_UTIL_H */
