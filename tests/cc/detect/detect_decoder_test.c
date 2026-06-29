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
 * Tests for the detect codec - a blind detector of convolutional-code structure.
 * It outputs, per position, c_erasure = confidence a code IS present (engine
 * c_lost) and c_absent = confidence a code is NOT present, all other soft fields
 * 0. The tests confirm: a real coded stream reads as "code present", a random
 * stream reads as "no code", output count tracks input, only the two fields are
 * populated, and the lifecycle is sound.
 */

#include <drifty/cc/ccode.h>
#include <drifty/cc/detect.h>
#include <drifty/cc/encoder.h>
#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <drifty/stream_encoder.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

static int check(const char *name, int cond) {
  printf("  [%s] %s\n", name, cond ? "PASS" : "FAIL");
  if (!cond) {
    ++g_failures;
  }
  return cond;
}
static void check_gt(const char *name, double got, double want) {
  printf("  [%s] %.3f (want > %.3f) %s\n", name, got, want,
         got > want ? "PASS" : "FAIL");
  if (!(got > want)) {
    ++g_failures;
  }
}
static void check_lt(const char *name, double got, double want) {
  printf("  [%s] %.3f (want < %.3f) %s\n", name, got, want,
         got < want ? "PASS" : "FAIL");
  if (!(got < want)) {
    ++g_failures;
  }
}

/* deterministic splitmix64 */
static uint64_t rng_next(uint64_t *s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* Encode `n_info` random bits with `which` through the shared encoder. */
static int encode(dt_cc_standard_code which, int n_info, dt_bit *out, int cap,
                  uint64_t *rng) {
  dt_bit *msg = malloc((size_t)n_info);
  for (int i = 0; i < n_info; ++i) {
    msg[i] = (rng_next(rng) & 1) ? DT_TRUE : DT_FALSE;
  }
  dt_cc_code *code = dt_cc_code_create_standard(which);
  dt_stream_encoder *enc = dt_cc_encoder_create(code);
  int len = enc->begin(enc, out, cap);
  len += enc->encode(enc, out + len, cap - len, msg, n_info);
  len += enc->finalize(enc, out + len, cap - len);
  dt_cc_encoder_destroy(enc);
  dt_cc_code_destroy(code);
  free(msg);
  return len;
}

/* Run detect over rx[] and collect every per-position record. Returns the count. */
static int detect_all(const dt_bit *rx, int rl, dt_soft_bit *out, int cap) {
  dt_stream_soft_decoder *sd = dt_cc_detect_soft_decoder_create();
  int got = sd->begin(sd, NULL, 0);
  got += sd->decode(sd, out + got, cap - got, rx, rl);
  for (;;) {
    int w = sd->decode(sd, out + got, cap - got, NULL, 0);
    if (w <= 0) {
      break;
    }
    got += w;
  }
  got += sd->finalize(sd, out + got, cap - got);
  dt_cc_detect_soft_decoder_destroy(sd);
  return got;
}

/* The factory takes no parameters; destroy(NULL) is safe. */
static void test_create(void) {
  printf("detect create:\n");
  dt_stream_soft_decoder *sd = dt_cc_detect_soft_decoder_create();
  check("create succeeds", sd != NULL);
  dt_cc_detect_soft_decoder_destroy(sd);
  dt_cc_detect_soft_decoder_destroy(NULL);
  check("destroy(NULL) is safe", 1);
}

/* A real coded stream reads as "code present": mean coded-confidence high, mean
 * not-coded low, and the other soft fields stay 0. Output count tracks input. */
static void test_detects_code(void) {
  printf("detect on a coded stream (should read as code present):\n");
  enum { NINFO = 2000, CAP = NINFO * 5 + 256 };
  uint64_t rng = 0xC0DE11u;
  dt_bit *coded = malloc(CAP);
  int clen = encode(DT_CC_CODE_K7_RATE_1_2, NINFO, coded, CAP, &rng);

  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  int got = detect_all(coded, clen, out, CAP);

  double lost = 0, absent = 0;
  int other_nonzero = 0, range_ok = 1;
  for (int i = 0; i < got; ++i) {
    lost += out[i].c_erasure; /* engine c_lost = coded confidence */
    absent += out[i].c_absent;
    if (out[i].c_true != 0.0f || out[i].c_false != 0.0f ||
        out[i].c_invalid != 0.0f || out[i].c_locked != 0.0f) {
      ++other_nonzero;
    }
    const float f[2] = {out[i].c_erasure, out[i].c_absent};
    for (int k = 0; k < 2; ++k) {
      if (f[k] < -1e-6f || f[k] > 1.0f + 1e-6f) {
        range_ok = 0;
      }
    }
  }
  check("output count tracks input", got == clen);
  check_gt("coded: mean code-present confidence high", lost / got, 0.8);
  check_lt("coded: mean no-code confidence low", absent / got, 0.1);
  check("coded: confidences in [0,1]", range_ok);
  check("coded: only c_erasure/c_absent populated", other_nonzero == 0);

  free(coded);
  free(out);
}

/* A random stream reads as "no code": mean not-coded confidence high. */
static void test_rejects_random(void) {
  printf("detect on a random stream (should read as no code):\n");
  enum { RL = 4000, CAP = RL + 256 };
  uint64_t rng = 0x9A9A9Au;
  dt_bit *rx = malloc(CAP);
  for (int i = 0; i < RL; ++i) {
    rx[i] = (rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
  }
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  int got = detect_all(rx, RL, out, CAP);

  double lost = 0, absent = 0;
  for (int i = 0; i < got; ++i) {
    lost += out[i].c_erasure;
    absent += out[i].c_absent;
  }
  check("output count tracks input", got == RL);
  check_gt("random: mean no-code confidence high", absent / got, 0.8);
  check_lt("random: mean code-present confidence low", lost / got, 0.05);

  free(rx);
  free(out);
}

int main(void) {
  test_create();
  test_detects_code();
  test_rejects_random();
  printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
