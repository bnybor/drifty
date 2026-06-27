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
 * Shared test utilities for the maxir suite: a small soft-assertion framework
 * plus the PRNG, encode, channel, and decoder-param helpers the test files have
 * in common. Header-only; every helper is `static inline` so a test file that
 * does not use one draws no -Wunused warning.
 */

#ifndef DT_CC_MAXIR_TEST_UTIL_H
#define DT_CC_MAXIR_TEST_UTIL_H

#include <cc/maxir/decode.h>
#include <cc/full_encoder/encode.h>

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
 * (which must hold info_bits * dt_cc_code_n(code) + flush slack), including the
 * end-of-stream flush. Returns the coded length. */
static inline int maxir_encode_all(const dt_cc_code *code, const uint8_t *msg,
                                  int info_bits, uint8_t *out) {
  int state = 0;
  unsigned int unknown = 0;
  int len = dt_cc_full_encoder_encode(code, msg, info_bits, &state, &unknown, out);
  len += dt_cc_full_encoder_flush(code, &state, &unknown, out + len);
  return len;
}

/* -- channels (model the impairments the decoder corrects) ----------------- */

static inline double rng_unit(uint64_t *state) {
  return (double)(rng_next(state) >> 11) * (1.0 / 9007199254740992.0);
}

/* Drop each coded bit with probability p_del; cumulative drift grows with
 * position. Returns the received length. */
static inline int delete_channel(const uint8_t *in, int len, double p_del,
                                 uint64_t *rng, uint8_t *out) {
  int o = 0;
  for (int i = 0; i < len; ++i) {
    if (rng_unit(rng) < p_del) {
      continue;
    }
    out[o++] = in[i];
  }
  return o;
}

/* Insert a spurious random bit before each coded bit with probability p_ins;
 * cumulative drift grows with position. out[] must hold up to 2*len bits.
 * Returns the received length. */
static inline int insert_channel(const uint8_t *in, int len, double p_ins,
                                 uint64_t *rng, uint8_t *out) {
  int o = 0;
  for (int i = 0; i < len; ++i) {
    if (rng_unit(rng) < p_ins) {
      out[o++] = bit_sym((unsigned int)rng_next(rng));
    }
    out[o++] = in[i];
  }
  return o;
}

/* Flip each bit with probability p_flip, in place (run before erase_channel so a
 * flip never lands on a DT_ERASURE marker). */
static inline void flip_channel(uint8_t *buf, int len, double p_flip,
                                uint64_t *rng) {
  for (int i = 0; i < len; ++i) {
    if (rng_unit(rng) < p_flip) {
      buf[i] ^= DT_VALUE; /* toggle the value bit: DT_TRUE <-> DT_FALSE */
    }
  }
}

/* Mark each bit DT_ERASURE with probability p_erase, in place. */
static inline void erase_channel(uint8_t *buf, int len, double p_erase,
                                 uint64_t *rng) {
  for (int i = 0; i < len; ++i) {
    if (rng_unit(rng) < p_erase) {
      buf[i] = DT_ERASURE;
    }
  }
}

/* -- decoder params -------------------------------------------------------- */

/* Build channel-model params from positional settings (keeps tests concise).
 * insert_channel inserts uniformly-random 0/1 bits, so the total insertion rate
 * is split evenly between the true/false components; p_erase maps onto the
 * overwrite-to-erasure rate. */
static inline dt_cc_maxir_stream_params make_params(int depth, int drift,
                                                 double p_flip, double p_ins,
                                                 double p_del, double p_erase) {
  dt_cc_maxir_stream_params params = {
      .decision_depth = depth,
      .max_drift = drift,
      .p_flip = (float)p_flip,
      .p_ins_true = (float)(p_ins * 0.5),
      .p_ins_false = (float)(p_ins * 0.5),
      .p_del = (float)p_del,
      .p_ovr_erase = (float)p_erase,
  };
  return params;
}

#endif /* DT_CC_MAXIR_TEST_UTIL_H */
