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
 * Tests for the multi-encoder: holding a same-rate, same-K family of codes over
 * one shared encoder state and emitting whichever code is selected by index.
 * Covers equivalence with the single encoder, that the shared state carries
 * across chunked calls and across a mid-stream code switch, a round-trip through
 * the multi-decoder, and argument/edge handling. Encoding is deterministic, so
 * these run serially (no Monte-Carlo).
 */

#include "dt_test_util.h"

/* Three mutually distinguishable rate-1/3, K=7 codes: same n and constraint
 * length, different generators - the family the multi-encoder expects. */
static const dt_standard_code FAMILY[] = {
    DT_CODE_K7_RATE_1_3,
    DT_CODE_K7_RATE_1_3_ALT1,
    DT_CODE_K7_RATE_1_3_ALT2,
};
#define N_FAM ((int)(sizeof(FAMILY) / sizeof(FAMILY[0])))
#define N_INFO 256
#define DEPTH 48

/* Build a multi-encoder over the whole family, handing back the codes (which
 * must outlive the encoder) via `codes`. Fresh codes => the shared state is 0. */
static dt_multi_encoder *build_multi_encoder(dt_ccode *codes[N_FAM]) {
  const dt_ccode *cp[N_FAM];
  for (int j = 0; j < N_FAM; ++j) {
    codes[j] = dt_ccode_create_standard(FAMILY[j]);
    assert(codes[j]);
    cp[j] = codes[j];
  }
  dt_multi_encode_params params = {.codes = cp, .codes_len = N_FAM};
  return dt_multi_encode_create(&params);
}

static void destroy_codes(dt_ccode *codes[N_FAM]) {
  for (int j = 0; j < N_FAM; ++j) {
    dt_ccode_destroy(codes[j]);
  }
}

/* Each code, encoded through the multi-encoder, is byte-identical to the single
 * encoder over the same code from state 0. */
static void test_single_index_equivalence(uint64_t seed) {
  printf("test_single_index_equivalence\n");
  uint64_t rng = seed;
  uint8_t msg[N_INFO];
  rand_bits(msg, N_INFO, &rng);

  for (int j = 0; j < N_FAM; ++j) {
    dt_ccode *codes[N_FAM];
    dt_multi_encoder *e = build_multi_encoder(codes);
    REQUIRE("equiv: encoder created", e != NULL);

    uint8_t got[MAX_CODED(N_INFO)];
    int w = dt_multi_encode(e, j, msg, N_INFO, got, (int)sizeof got);
    int wf = dt_multi_encode_flush(e, j, got + w, (int)sizeof got - w);

    uint8_t ref[MAX_CODED(N_INFO)];
    int n, k;
    size_t rl = encode(FAMILY[j], msg, N_INFO, ref, &n, &k);

    check("equiv: length matches single encoder", (size_t)(w + wf) == rl);
    check("equiv: bytes match single encoder",
          w + wf > 0 && memcmp(got, ref, (size_t)(w + wf)) == 0);

    dt_multi_encode_destroy(e);
    destroy_codes(codes);
  }
}

/* The shared state carries across calls: encoding a message in chunks gives the
 * same stream as encoding it in one shot. */
static void test_chunked_matches_oneshot(uint64_t seed) {
  printf("test_chunked_matches_oneshot\n");
  uint64_t rng = seed;
  uint8_t msg[N_INFO];
  rand_bits(msg, N_INFO, &rng);
  const int idx = 1;

  dt_ccode *codes_a[N_FAM];
  dt_multi_encoder *one = build_multi_encoder(codes_a);
  REQUIRE("chunk: one-shot encoder", one != NULL);
  uint8_t a[MAX_CODED(N_INFO)];
  int la = dt_multi_encode(one, idx, msg, N_INFO, a, (int)sizeof a);
  la += dt_multi_encode_flush(one, idx, a + la, (int)sizeof a - la);

  dt_ccode *codes_b[N_FAM];
  dt_multi_encoder *many = build_multi_encoder(codes_b);
  REQUIRE("chunk: chunked encoder", many != NULL);
  uint8_t b[MAX_CODED(N_INFO)];
  int lb = 0;
  for (int pos = 0; pos < N_INFO;) {
    int chunk = (N_INFO - pos < 37) ? (N_INFO - pos) : 37;
    lb += dt_multi_encode(many, idx, msg + pos, chunk, b + lb, (int)sizeof b - lb);
    pos += chunk;
  }
  lb += dt_multi_encode_flush(many, idx, b + lb, (int)sizeof b - lb);

  check("chunk: same length", la == lb);
  check("chunk: same bytes", la > 0 && memcmp(a, b, (size_t)la) == 0);

  dt_multi_encode_destroy(one);
  dt_multi_encode_destroy(many);
  destroy_codes(codes_a);
  destroy_codes(codes_b);
}

/* Switching the selected code mid-stream is coherent because the state is shared
 * and code-independent for a same-K family: the prefix equals code A's encoding
 * of the first half, and the suffix equals code B continued from the very same
 * state (reached by feeding code B the same first-half bits). */
static void test_shared_state_code_switch(uint64_t seed) {
  printf("test_shared_state_code_switch\n");
  uint64_t rng = seed;
  uint8_t msg[N_INFO];
  rand_bits(msg, N_INFO, &rng);
  const int a_idx = 0, b_idx = 1, half = N_INFO / 2;

  dt_ccode *codes[N_FAM];
  dt_multi_encoder *e = build_multi_encoder(codes);
  REQUIRE("switch: encoder created", e != NULL);
  const int n = dt_ccode_n(codes[0]);

  uint8_t got[MAX_CODED(N_INFO)];
  int w = dt_multi_encode(e, a_idx, msg, half, got, (int)sizeof got);
  int w2 =
      dt_multi_encode(e, b_idx, msg + half, N_INFO - half, got + w,
                      (int)sizeof got - w);
  int wf = dt_multi_encode_flush(e, b_idx, got + w + w2, (int)sizeof got - w - w2);

  /* Prefix: code A's encoding of the first half from state 0. */
  uint8_t ref_a[MAX_CODED(N_INFO)];
  int sa = 0;
  int la = dt_ccode_encode(codes[a_idx], msg, half, &sa, ref_a);
  check("switch: prefix is code A", w == la &&
                                        memcmp(got, ref_a, (size_t)la) == 0);

  /* Suffix: advance code B's state over the same first-half bits (output
   * discarded), then its encoding of the second half plus flush. */
  uint8_t scratch[MAX_CODED(N_INFO)];
  uint8_t ref_b[MAX_CODED(N_INFO)];
  int sb = 0;
  dt_ccode_encode(codes[b_idx], msg, half, &sb, scratch);
  int lb = dt_ccode_encode(codes[b_idx], msg + half, N_INFO - half, &sb, ref_b);
  lb += dt_ccode_encode_flush(codes[b_idx], &sb, ref_b + lb);
  check("switch: suffix is code B from shared state",
        w2 + wf == lb && memcmp(got + w, ref_b, (size_t)lb) == 0);
  check("switch: prefix length is half*n", w == half * n);

  dt_multi_encode_destroy(e);
  destroy_codes(codes);
}

/* Round-trip: a stream produced with code `true_idx` decodes cleanly through a
 * multi-decoder over the same family, attributed to that code. */
static void test_round_trip(uint64_t seed, int true_idx) {
  printf("test_round_trip (idx %d)\n", true_idx);
  uint64_t rng = seed;
  uint8_t msg[N_INFO];
  rand_bits(msg, N_INFO, &rng);

  dt_ccode *codes[N_FAM];
  dt_multi_encoder *e = build_multi_encoder(codes);
  REQUIRE("rt: encoder created", e != NULL);

  uint8_t coded[MAX_CODED(N_INFO)];
  int w = dt_multi_encode(e, true_idx, msg, N_INFO, coded, (int)sizeof coded);
  w += dt_multi_encode_flush(e, true_idx, coded + w, (int)sizeof coded - w);
  REQUIRE("rt: encoded some bits", w > 0);

  const dt_ccode *cp[N_FAM];
  for (int j = 0; j < N_FAM; ++j) {
    cp[j] = codes[j];
  }
  dt_multi_params dp = {.codes = cp,
                        .codes_len = N_FAM,
                        .stream = {.decision_depth = DEPTH, .p_flip = 0.02}};
  dt_multi_decoder *md = dt_multi_create(&dp);
  REQUIRE("rt: decoder created", md != NULL);

  const int cap = N_INFO + 64;
  uint8_t out[N_INFO + 64];
  int locked[N_INFO + 64];
  dt_decode_details *details =
      malloc((size_t)cap * N_FAM * sizeof(dt_decode_details));
  for (int i = 0; i < cap; ++i) {
    locked[i] = -1;
  }
  int got = 0;
  for (int pos = 0; pos < w;) {
    int chunk = (w - pos < 41) ? (w - pos) : 41;
    int x = dt_multi_decode(md, coded + pos, chunk, out + got,
                            details + (size_t)got * N_FAM, cap - got);
    assert(x >= 0);
    got += x;
    pos += chunk;
  }
  int n_stream = got; /* the flush tail (below) has no details */
  for (;;) {
    int x = dt_multi_decode_flush(md, out + got, NULL, cap - got);
    assert(x >= 0);
    if (x == 0) break;
    got += x;
  }
  /* The winning code at a committed position is the one with the highest lock
   * consistency; the flush tail carries no details, so it reports no winner. */
  for (int i = 0; i < n_stream; ++i) {
    if (out[i] == DT_ERASURE) continue;
    int best = 0;
    for (int j = 1; j < N_FAM; ++j) {
      if (details[(size_t)i * N_FAM + j].c_lock >
          details[(size_t)i * N_FAM + best].c_lock) {
        best = j;
      }
    }
    locked[i] = best;
  }

  int errors = 0, wrong = 0, tail_set = 0;
  for (int i = 0; i < got && i < N_INFO; ++i) {
    if (out[i] == DT_ERASURE) continue;
    if (out[i] != msg[i]) ++errors;
    if (locked[i] >= 0 && locked[i] != true_idx) ++wrong;
  }
  /* The flush appended K-1 zero input bits, so the decoder yields N_INFO + (K-1)
   * bits; the trailing ones decode back to zero. */
  for (int i = N_INFO; i < got; ++i) {
    if (out[i] != DT_ERASURE && out[i] != DT_FALSE) ++tail_set;
  }
  check("rt: message bits plus flush tail decoded",
        got == N_INFO + (dt_ccode_k(codes[0]) - 1));
  check("rt: exact recovery", errors == 0);
  check("rt: flush tail is zero", tail_set == 0);
  check("rt: attributed to true code", wrong == 0);

  free(details);
  dt_multi_destroy(md);
  dt_multi_encode_destroy(e);
  destroy_codes(codes);
}

static void test_args(void) {
  printf("test_args\n");
  uint8_t msg[8] = {0};
  uint8_t buf[MAX_CODED(N_INFO)];

  dt_ccode *codes[N_FAM];
  dt_multi_encoder *e = build_multi_encoder(codes);
  REQUIRE("args: encoder created", e != NULL);

  check("args: NULL handle", dt_multi_encode(NULL, 0, msg, 1, buf,
                                             (int)sizeof buf) == DT_ERR_ARG);
  check("args: idx < 0",
        dt_multi_encode(e, -1, msg, 1, buf, (int)sizeof buf) == DT_ERR_ARG);
  check("args: idx >= n",
        dt_multi_encode(e, N_FAM, msg, 1, buf, (int)sizeof buf) == DT_ERR_ARG);
  check("args: negative n_bits",
        dt_multi_encode(e, 0, msg, -1, buf, (int)sizeof buf) == DT_ERR_ARG);
  /* 4 bits at rate 1/3 need 12 output bits; max_out 1 is too small. */
  check("args: max_out too small (encode)",
        dt_multi_encode(e, 0, msg, 4, buf, 1) == DT_ERR_ARG);
  /* flush writes (7-1)*3 = 18 bits; max_out 1 is too small. */
  check("args: max_out too small (flush)",
        dt_multi_encode_flush(e, 0, buf, 1) == DT_ERR_ARG);
  check("args: NULL handle (flush)",
        dt_multi_encode_flush(NULL, 0, buf, (int)sizeof buf) == DT_ERR_ARG);

  dt_multi_encode_destroy(e);
  destroy_codes(codes);

  /* Empty set creates fine but cannot encode (no valid index). */
  dt_multi_encode_params ep = {.codes = NULL, .codes_len = 0};
  dt_multi_encoder *empty = dt_multi_encode_create(&ep);
  check("args: empty set creates", empty != NULL);
  check("args: empty set encode errors",
        dt_multi_encode(empty, 0, msg, 1, buf, (int)sizeof buf) == DT_ERR_ARG);
  dt_multi_encode_destroy(empty);

  check("args: NULL params", dt_multi_encode_create(NULL) == NULL);

  /* A NULL code entry is rejected at create. */
  dt_ccode *c0 = dt_ccode_create_standard(FAMILY[0]);
  assert(c0);
  const dt_ccode *with_null[2] = {c0, NULL};
  dt_multi_encode_params np = {.codes = with_null, .codes_len = 2};
  check("args: NULL code entry rejected", dt_multi_encode_create(&np) == NULL);

  /* A mismatched rate/constraint length is rejected at create. */
  dt_ccode *k3 = dt_ccode_create_standard(DT_CODE_K3_RATE_1_2);
  assert(k3);
  const dt_ccode *mixed[2] = {c0, k3};
  dt_multi_encode_params mp = {.codes = mixed, .codes_len = 2};
  check("args: mismatched n/k rejected", dt_multi_encode_create(&mp) == NULL);

  dt_ccode_destroy(c0);
  dt_ccode_destroy(k3);
}

int main(void) {
  const uint64_t seed = 0xC0FFEEULL;
  test_single_index_equivalence(seed);
  test_chunked_matches_oneshot(seed + 1);
  test_shared_state_code_switch(seed + 2);
  for (int j = 0; j < N_FAM; ++j) {
    test_round_trip(seed + 10 + (uint64_t)j, j);
  }
  test_args();
  return test_summary("multi_encode");
}
