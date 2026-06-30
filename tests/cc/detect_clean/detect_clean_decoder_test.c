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
 * Tests for the detect_clean codec - a blind detector of convolutional-code structure.
 * Per position it emits TWO INDEPENDENT goodness-of-fit reads (not a probability
 * split, so they need not sum to 1):
 *   c_erasure = consistency with "a convolutional code IS present"
 *   c_absent  = consistency with "no code / the stream is random"
 * Each answers "does the data fail to contradict this hypothesis?", so the
 * no-discriminating-evidence state is (1, 1) - an all-non-bit run, or the warm-up /
 * flush tail - not (0, 0). The tests confirm: a coded stream is consistent with a
 * code and contradicts random; a random stream the reverse (under a clean model);
 * the channel model lifts the CODE-PRESENT axis (not the no-code one) when flips are
 * expected; an all-erasure run reads (1, 1); un-encodable DT_INVALID placement damps
 * the CODE-PRESENT axis only (an encodable invalid run does not); indels are
 * tolerated; output count tracks input; only the two fields are populated; and the
 * lifecycle is sound.
 */

#include <drifty/cc/ccode.h>
#include <drifty/cc/detect_clean.h>
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

/* A clean-channel model (p_flip 0): an unstructured window is trusted to rule a
 * code out, so c_erasure on random collapses to ~0. */
static dt_cc_detect_clean_stream_params clean_params(void) {
  dt_cc_detect_clean_stream_params p = {0};
  p.decision_depth = 40;
  return p;
}

/* Run detect (with channel model `p`) over rx[] and collect every per-position
 * record. Returns the count. */
static int detect_all(const dt_cc_detect_clean_stream_params *p, const dt_bit *rx,
                      int rl, dt_soft_bit *out, int cap) {
  dt_stream_soft_decoder *sd = dt_cc_detect_clean_soft_decoder_create(p);
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
  dt_cc_detect_clean_soft_decoder_destroy(sd);
  return got;
}

/* Mean code-present (c_erasure) and no-code (c_absent) consistency over a run. */
static void means(const dt_soft_bit *out, int got, double *present, double *absent) {
  double e = 0, a = 0;
  for (int i = 0; i < got; ++i) {
    e += out[i].c_erasure;
    a += out[i].c_absent;
  }
  *present = got ? e / got : 0;
  *absent = got ? a / got : 0;
}

/* Argument validation at the factory, and destroy(NULL) safety. */
static void test_create(void) {
  printf("detect create:\n");
  dt_cc_detect_clean_stream_params p = clean_params();
  dt_stream_soft_decoder *sd = dt_cc_detect_clean_soft_decoder_create(&p);
  check("create succeeds with valid params", sd != NULL);
  dt_cc_detect_clean_soft_decoder_destroy(sd);

  check("rejects NULL params", dt_cc_detect_clean_soft_decoder_create(NULL) == NULL);
  dt_cc_detect_clean_stream_params bad = clean_params();
  bad.decision_depth = 0;
  check("rejects decision_depth < 1",
        dt_cc_detect_clean_soft_decoder_create(&bad) == NULL);
  bad = clean_params();
  bad.p_flip = 1.0f;
  check("rejects p_flip >= 1", dt_cc_detect_clean_soft_decoder_create(&bad) == NULL);
  bad = clean_params();
  bad.p_ovr_true = 0.6f;
  bad.p_ovr_false = 0.6f; /* overwrite family sums to >= 1 */
  check("rejects overwrite sum >= 1",
        dt_cc_detect_clean_soft_decoder_create(&bad) == NULL);

  dt_cc_detect_clean_soft_decoder_destroy(NULL);
  check("destroy(NULL) is safe", 1);
}

/* A coded stream is consistent with a code and contradicts random: mean c_erasure
 * high, mean c_absent low. Output count tracks input; only the two fields populate. */
static void test_detects_code(void) {
  printf("detect on a coded stream (consistent with a code, contradicts random):\n");
  enum { NINFO = 2000, CAP = NINFO * 5 + 256 };
  uint64_t rng = 0xC0DE11u;
  dt_bit *coded = malloc(CAP);
  int clen = encode(DT_CC_CODE_K7_RATE_1_2, NINFO, coded, CAP, &rng);

  dt_cc_detect_clean_stream_params p = clean_params();
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  int got = detect_all(&p, coded, clen, out, CAP);

  int other_nonzero = 0, range_ok = 1;
  for (int i = 0; i < got; ++i) {
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
  double present, absent;
  means(out, got, &present, &absent);
  check("output count tracks input", got == clen);
  check_gt("coded: consistency with a code high", present, 0.9);
  check_lt("coded: consistency with random low", absent, 0.05);
  check("coded: consistencies in [0,1]", range_ok);
  check("coded: only c_erasure/c_absent populated", other_nonzero == 0);

  free(coded);
  free(out);
}

/* A random stream fits the random model and, under a clean channel model,
 * contradicts a code: mean c_absent high, mean c_erasure low. */
static void test_rejects_random(void) {
  printf("detect on a random stream (fits random, contradicts a code):\n");
  enum { RL = 4000, CAP = RL + 256 };
  uint64_t rng = 0x9A9A9Au;
  dt_bit *rx = malloc(CAP);
  for (int i = 0; i < RL; ++i) {
    rx[i] = (rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
  }
  dt_cc_detect_clean_stream_params p = clean_params();
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  int got = detect_all(&p, rx, RL, out, CAP);

  double present, absent;
  means(out, got, &present, &absent);
  check("output count tracks input", got == RL);
  check_gt("random: consistency with random high", absent, 0.8);
  check_lt("random: consistency with a code low", present, 0.1);

  free(rx);
  free(out);
}

/* The channel model calibrates the CODE-PRESENT axis, not the no-code one. On a
 * random stream, telling detect to expect flips RAISES c_erasure - a clean-looking
 * full-rank window can no longer rule a code out, since the expected flips could
 * have broken a real code's parity - while c_absent (the positive fit to the random
 * model) is left untouched, because an unstructured window is consistent with random
 * whatever the channel. (The old model damped c_absent; that coupled the axes and
 * is exactly what changed.) */
static void test_noise_calibration(void) {
  printf("detect channel-model calibration of the code-present axis:\n");
  enum { RL = 4000, CAP = RL + 256 };
  uint64_t rng = 0x5151FFu;
  dt_bit *rx = malloc(CAP);
  for (int i = 0; i < RL; ++i) {
    rx[i] = (rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
  }
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));

  dt_cc_detect_clean_stream_params clean = clean_params();
  dt_cc_detect_clean_stream_params noisy = clean_params();
  noisy.p_flip = 0.05f; /* expect 5% flips: a code could hide under that */

  double present_clean, absent_clean, present_noisy, absent_noisy;
  int gc = detect_all(&clean, rx, RL, out, CAP);
  means(out, gc, &present_clean, &absent_clean);
  int gn = detect_all(&noisy, rx, RL, out, CAP);
  means(out, gn, &present_noisy, &absent_noisy);

  printf("  random stream: c_erasure clean=%.3f vs noisy-model=%.3f"
         "  |  c_absent clean=%.3f vs noisy-model=%.3f\n",
         present_clean, present_noisy, absent_clean, absent_noisy);
  check_lt("clean model: code-present consistency low", present_clean, 0.1);
  check_gt("noisy model: code-present consistency raised", present_noisy, 0.5);
  check("noisy model lifts the code-present axis", present_noisy > present_clean);
  check_gt("no-code consistency stays high (clean model)", absent_clean, 0.8);
  check_gt("no-code consistency stays high (noisy model)", absent_noisy, 0.8);
  check("channel model leaves the no-code axis unchanged",
        absent_noisy - absent_clean < 0.01 && absent_clean - absent_noisy < 0.01);

  free(rx);
  free(out);
}

/* No discriminating evidence reads (1, 1): with no usable bits to judge (a run of
 * all DT_ERASURE), nothing contradicts either hypothesis, so both consistencies stay
 * near 1 rather than collapsing to 0. */
static void test_no_evidence(void) {
  printf("detect on an all-erasure run (no discriminating evidence -> (1,1)):\n");
  enum { RL = 4000, CAP = RL + 256 };
  dt_bit *rx = malloc(CAP);
  for (int i = 0; i < RL; ++i) {
    rx[i] = DT_ERASURE;
  }
  dt_cc_detect_clean_stream_params p = clean_params();
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  int got = detect_all(&p, rx, RL, out, CAP);

  double present, absent;
  means(out, got, &present, &absent);
  check("output count tracks input", got == RL);
  check_gt("all-erasure: code-present consistency near 1", present, 0.9);
  check_gt("all-erasure: no-code consistency near 1", absent, 0.9);

  free(rx);
  free(out);
}

/* DT_INVALID is present-axis evidence. A placement no single convolutional encoder
 * could emit - a lone invalid (a length-1 run), or runs of differing length - damps
 * the CODE-PRESENT read (c_erasure) toward 0, while leaving c_absent (the fit to
 * random) untouched: an invalid is off the random-boolean manifold too, so it never
 * argues FOR random. An ENCODABLE invalid shape (a single contiguous run, one trellis
 * offset) carries no penalty. The damping applies even with no other usable bits (an
 * all-erasure base), and on a coded stream it drives c_erasure down off its ceiling -
 * code-like structure that nonetheless carries un-encodable symbols. */
static void test_invalid_evidence(void) {
  printf("detect DT_INVALID present-axis evidence (damps code-present, not no-code):\n");
  enum { RL = 4000, CAP = RL + 256 };
  dt_cc_detect_clean_stream_params p = clean_params();
  dt_bit *rx = malloc(CAP);
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  double present, absent;

  /* Lone invalids (singletons) over an all-erasure base: un-encodable, so c_erasure
   * is crushed while c_absent stays at the no-evidence ceiling (~1). Contrast
   * test_no_evidence, where the same base with NO invalids reads (1, 1). */
  for (int i = 0; i < RL; ++i) rx[i] = DT_ERASURE;
  for (int i = 20; i < RL; i += 20) rx[i] = DT_INVALID;
  int got = detect_all(&p, rx, RL, out, CAP);
  means(out, got, &present, &absent);
  check_lt("invalid singletons: code-present consistency crushed", present, 0.1);
  check_gt("invalid singletons: no-code consistency untouched", absent, 0.9);

  /* A single contiguous invalid RUN is an encodable shape (one offset) and adds no
   * penalty: it reads like an all-non-bit gap, (1, 1). */
  for (int i = 0; i < RL; ++i) rx[i] = DT_ERASURE;
  for (int i = 1000; i < 1100; ++i) rx[i] = DT_INVALID;
  got = detect_all(&p, rx, RL, out, CAP);
  means(out, got, &present, &absent);
  check_gt("single invalid run: encodable shape, no damping", present, 0.9);

  /* On a CODED stream the same singletons contradict a single code: c_erasure drops
   * off its ~1 ceiling even though the underlying structure is still code-like. */
  uint64_t rng = 0xC0DE99u;
  int clen = encode(DT_CC_CODE_K7_RATE_1_2, 1500, rx, CAP, &rng);
  for (int i = 50; i < clen; i += 50) rx[i] = DT_INVALID;
  got = detect_all(&p, rx, clen, out, CAP);
  means(out, got, &present, &absent);
  check_lt("coded + invalid singletons: code-present contradicted", present, 0.1);

  free(rx);
  free(out);
}

/* Drop each bit with probability p (a deletion channel); returns the new length.
 * Deletions drift the stream's phase - the case the sliding-window method handles. */
static int delete_channel(const dt_bit *in, int len, double p, uint64_t *rng,
                          dt_bit *out) {
  int o = 0;
  for (int i = 0; i < len; ++i) {
    if ((double)(rng_next(rng) >> 11) * (1.0 / 9007199254740992.0) < p) {
      continue;
    }
    out[o++] = in[i];
  }
  return o;
}

/* detect tolerates sparse indels: a coded stream through a ~1% deletion channel
 * stays consistent with a code (the runs between deletions are clean and aligned),
 * while a random stream through the same channel stays a confident no-code. */
static void test_indel_tolerance(void) {
  printf("detect indel tolerance (coded stream through a ~1%% deletion channel):\n");
  enum { NINFO = 2500, CAP = NINFO * 5 + 256 };
  uint64_t rng = 0x1DEC0DEu;
  dt_cc_detect_clean_stream_params p = clean_params();

  dt_bit *coded = malloc(CAP);
  int clen = encode(DT_CC_CODE_K7_RATE_1_2, NINFO, coded, CAP, &rng);
  dt_bit *rx = malloc(CAP);
  int rl = delete_channel(coded, clen, 0.01, &rng, rx);
  dt_soft_bit *out = malloc((size_t)CAP * sizeof(*out));
  int got = detect_all(&p, rx, rl, out, CAP);
  double present, absent;
  means(out, got, &present, &absent);
  printf("  coded + 1%% deletions: %d bits -> consistency with a code %.3f\n", rl,
         present);
  check("output count tracks input", got == rl);
  check_gt("coded+indels stays consistent with a code", present, 0.5);

  /* random through the same channel must NOT become a false positive */
  for (int i = 0; i < clen; ++i) {
    coded[i] = (rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
  }
  rl = delete_channel(coded, clen, 0.01, &rng, rx);
  got = detect_all(&p, rx, rl, out, CAP);
  means(out, got, &present, &absent);
  check_gt("random+indels stays a confident no-code", absent, 0.8);
  check_lt("random+indels: no false code-present", present, 0.1);

  free(coded);
  free(rx);
  free(out);
}

int main(void) {
  test_create();
  test_detects_code();
  test_rejects_random();
  test_noise_calibration();
  test_no_evidence();
  test_invalid_evidence();
  test_indel_tolerance();
  printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
