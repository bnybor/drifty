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
 * Tests for the public hybrid soft decoder (dt_hybrid_soft_decoder_create and
 * the dt_soft_decoder vtable): the per-bit consistency output, that its hard
 * decision agrees with the hard decoder, and - the regression - that a feed-only
 * `decode` call (dst_len == 0) still hands its input to the engine instead of
 * dropping it.
 */

#include "dt_test_util.h"

#include <drifty/hybrid.h>

#define CODE DT_CODE_K7_RATE_1_2
#define WARMUP 40

/* A clean-channel decoder model (the streams below carry light or no noise). */
static dt_hybrid_stream_params clean_params(void) {
  dt_hybrid_stream_params p = {0};
  p.decision_depth = 40;
  p.max_drift = 4;
  p.p_flip = 0.01;
  p.p_ins_true = 0.005;
  p.p_ins_false = 0.005;
  p.p_del = 0.01;
  return p;
}

/* The hard decision implied by a soft record - mirrors the engine's own rule
 * (erasure wins on a tie, else the more-consistent value). */
static uint8_t soft_hard(const dt_soft_decoder_out *o) {
  if (o->c_erasure >= o->c_true && o->c_erasure >= o->c_false) {
    return DT_ERASURE;
  }
  return o->c_true >= o->c_false ? DT_TRUE : DT_FALSE;
}

/* Soft-decode a whole received buffer in small chunks, then drain. Returns the
 * number of soft records collected. */
static int soft_decode_all(dt_soft_decoder *sd, const uint8_t *rx, int rl,
                           dt_soft_decoder_out *out, int cap) {
  int got = sd->begin(sd, NULL, 0);
  for (int pos = 0; pos < rl;) {
    int chunk = (rl - pos < 41) ? (rl - pos) : 41;
    int w = sd->decode(sd, out + got, cap - got, rx + pos, chunk);
    assert(w >= 0);
    got += w;
    pos += chunk;
  }
  int w = sd->finalize(sd, out + got, cap - got);
  assert(w >= 0);
  return got + w;
}

/* Hard-decode the same way (same feed chunking), for the soft/hard cross-check. */
static int hard_decode_all(dt_decoder *dec, const uint8_t *rx, int rl,
                           uint8_t *out, int cap) {
  int got = dec->begin(dec, out, cap);
  for (int pos = 0; pos < rl;) {
    int chunk = (rl - pos < 41) ? (rl - pos) : 41;
    int w = dec->decode(dec, out + got, cap - got, rx + pos, chunk);
    assert(w >= 0);
    got += w;
    pos += chunk;
  }
  int w = dec->finalize(dec, out + got, cap - got);
  assert(w >= 0);
  return got + w;
}

/* Argument handling: NULL code/params rejected, destroy(NULL) safe. */
static void test_soft_args(void) {
  printf("test_soft_args\n");
  dt_ccode *code = dt_ccode_create_standard(CODE);
  REQUIRE("code created", code != NULL);
  dt_hybrid_stream_params p = clean_params();

  check("create rejects NULL code",
        dt_hybrid_soft_decoder_create(NULL, &p) == NULL);
  check("create rejects NULL params",
        dt_hybrid_soft_decoder_create(code, NULL) == NULL);
  dt_hybrid_soft_decoder_destroy(NULL); /* must not crash */
  check("destroy(NULL) is safe", 1);

  dt_soft_decoder *sd = dt_hybrid_soft_decoder_create(code, &p);
  check("create succeeds with valid args", sd != NULL);
  dt_hybrid_soft_decoder_destroy(sd);
  dt_ccode_destroy(code);
}

/* A clean stream: the winning consistency matches the message on the settled
 * bits, lock stays high, the consistencies stay in [0, 1], and c_invalid /
 * c_absent (which the hybrid codec does not model) are always 0. */
static void test_soft_clean(void) {
  printf("test_soft_clean\n");
  const int N = 300;
  uint8_t *msg = malloc((size_t)N);
  uint64_t rng = 0x5EED01u;
  rand_bits(msg, N, &rng);

  int n, k;
  uint8_t *coded = malloc(MAX_CODED(N));
  int clen = (int)encode(CODE, msg, N, coded, &n, &k);

  dt_ccode *code = dt_ccode_create_standard(CODE);
  dt_hybrid_stream_params p = clean_params();
  dt_soft_decoder *sd = dt_hybrid_soft_decoder_create(code, &p);
  REQUIRE("soft decoder created", sd != NULL);

  const int cap = N + 64;
  dt_soft_decoder_out *out = malloc((size_t)cap * sizeof(*out));
  int got = soft_decode_all(sd, coded, clen, out, cap);

  int oob = 0, nonzero_ia = 0, errors = 0;
  double min_lock = 1.0;
  const int hi = (got < N ? got : N) - WARMUP;
  for (int i = 0; i < got; ++i) {
    const dt_soft_decoder_out *o = &out[i];
    if (o->c_true < 0.0 || o->c_true > 1.0 || o->c_false < 0.0 ||
        o->c_false > 1.0 || o->c_erasure < 0.0 || o->c_erasure > 1.0 ||
        o->c_locked < 0.0 || o->c_locked > 1.0) {
      ++oob;
    }
    if (o->c_invalid != 0.0 || o->c_absent != 0.0) {
      ++nonzero_ia;
    }
    if (i >= WARMUP && i < hi) {
      if (soft_hard(o) != msg[i]) ++errors;
      if (o->c_locked < min_lock) min_lock = o->c_locked;
    }
  }
  printf("  decoded %d (msg %d), settled errors=%d, min lock=%.3f\n", got, N,
         errors, min_lock);
  check("clean: all consistencies in [0,1]", oob == 0);
  check("clean: c_invalid/c_absent always 0", nonzero_ia == 0);
  check("clean: settled bits decode correctly", errors == 0);
  check_gt("clean: settled lock high", min_lock, 0.8);
  check("clean: output length tracks message", got >= N && got <= N + 16);

  dt_hybrid_soft_decoder_destroy(sd);
  free(out);
  free(coded);
  free(msg);
  dt_ccode_destroy(code);
}

/* Regression: feed the WHOLE coded stream through feed-only `decode` calls
 * (dst_len == 0, collecting nothing), then drain. The fix makes those pump calls
 * still hand their input to the engine; before it, the input was dropped and
 * nothing decoded. */
static void test_soft_feed_only_pump(void) {
  printf("test_soft_feed_only_pump\n");
  const int N = 300;
  uint8_t *msg = malloc((size_t)N);
  uint64_t rng = 0x9A17u;
  rand_bits(msg, N, &rng);

  int n, k;
  uint8_t *coded = malloc(MAX_CODED(N));
  int clen = (int)encode(CODE, msg, N, coded, &n, &k);

  dt_ccode *code = dt_ccode_create_standard(CODE);
  dt_hybrid_stream_params p = clean_params();
  dt_soft_decoder *sd = dt_hybrid_soft_decoder_create(code, &p);
  REQUIRE("soft decoder created", sd != NULL);

  const int cap = N + 64;
  dt_soft_decoder_out *out = malloc((size_t)cap * sizeof(*out));
  sd->begin(sd, NULL, 0);

  /* Pump: feed every coded bit with dst_len == 0 - a feed-only call collects no
   * output but must still buffer the input. */
  int collected_on_pump = 0;
  for (int pos = 0; pos < clen;) {
    int chunk = (clen - pos < 41) ? (clen - pos) : 41;
    int w = sd->decode(sd, out, 0, coded + pos, chunk);
    assert(w >= 0);
    collected_on_pump += w;
    pos += chunk;
  }
  check("pump: feed-only calls collect nothing", collected_on_pump == 0);

  /* Drain (no new input): everything pumped in must now come out. */
  int got = sd->decode(sd, out, cap, NULL, 0);
  got += sd->finalize(sd, out + got, cap - got);

  int errors = 0;
  const int hi = (got < N ? got : N) - WARMUP;
  for (int i = WARMUP; i < hi; ++i) {
    if (soft_hard(&out[i]) != msg[i]) ++errors;
  }
  printf("  pumped %d coded bits -> %d decoded, settled errors=%d\n", clen, got,
         errors);
  check("pump: the pumped input was decoded (output produced)", got >= N);
  check("pump: settled bits decode correctly", errors == 0);

  dt_hybrid_soft_decoder_destroy(sd);
  free(out);
  free(coded);
  free(msg);
  dt_ccode_destroy(code);
}

/* The soft decoder's hard decision must equal the hard decoder's bit, position
 * for position: both come from the same per-bit decision. A flip+erase channel
 * (no drift, so identical feed chunking gives identical decodes) exercises the
 * mismatch and erasure paths. */
static void test_soft_matches_hard(void) {
  printf("test_soft_matches_hard\n");
  const int N = 400;
  uint8_t *msg = malloc((size_t)N);
  uint64_t rng = 0xC0DEu;
  rand_bits(msg, N, &rng);

  int n, k;
  uint8_t *coded = malloc(MAX_CODED(N));
  int clen = (int)encode(CODE, msg, N, coded, &n, &k);
  flip_channel(coded, (size_t)clen, 0.02, &rng);
  erase_channel(coded, (size_t)clen, 0.05, &rng);

  dt_ccode *code = dt_ccode_create_standard(CODE);
  dt_hybrid_stream_params p = clean_params();
  p.p_ovr_erase = 0.05; /* the model expects the erasures we injected */

  dt_decoder *hd = dt_hybrid_decoder_create(code, &p);
  dt_soft_decoder *sd = dt_hybrid_soft_decoder_create(code, &p);
  REQUIRE("decoders created", hd != NULL && sd != NULL);

  const int cap = N + 64;
  uint8_t *hard = malloc((size_t)cap);
  dt_soft_decoder_out *soft = malloc((size_t)cap * sizeof(*soft));
  int gh = hard_decode_all(hd, coded, clen, hard, cap);
  int gs = soft_decode_all(sd, coded, clen, soft, cap);

  int mismatches = 0;
  const int m = gh < gs ? gh : gs;
  for (int i = 0; i < m; ++i) {
    if (hard[i] != soft_hard(&soft[i])) ++mismatches;
  }
  printf("  hard=%d soft=%d records, hard-decision mismatches=%d\n", gh, gs,
         mismatches);
  check("soft/hard decode the same number of bits", gh == gs);
  check("soft hard-decision == hard decoder output", mismatches == 0);

  dt_hybrid_decoder_destroy(hd);
  dt_hybrid_soft_decoder_destroy(sd);
  free(hard);
  free(soft);
  free(coded);
  free(msg);
  dt_ccode_destroy(code);
}

int main(void) {
  test_soft_args();
  test_soft_clean();
  test_soft_feed_only_pump();
  test_soft_matches_hard();
  return test_summary("soft_decoder");
}
