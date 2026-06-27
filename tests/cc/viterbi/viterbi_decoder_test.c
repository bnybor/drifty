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
 * Tests for the viterbi codec - a plain Viterbi hard-decision decoder - through
 * its encoder (dt_cc_encoder_encode) and streaming decoder (dt_cc_viterbi_stream_*):
 * chunked encoding, clean decoding, flip correction, erasure handling, the
 * standard presets, and argument handling. Bits crossing the API are dt_bit
 * symbols (DT_FALSE / DT_TRUE / DT_ERASURE).
 *
 * The decoder starts from the known encoder state 0, so it needs no
 * blind-acquisition warm-up: the message is recovered from bit 0. It does not
 * track inserted or dropped bits (that is what vindel is for), so there are no
 * indel tests here. A clean message of N bits decodes to N + (K-1) bits - the
 * trailing K-1 are the flush tail.
 */

#include "dt_test_util.h"

/* rate-1/5, K=5 code. */
static const unsigned int GENERATORS[] = {037, 033, 025, 027, 035};
#define K 5
#define N_GEN ((int)(sizeof(GENERATORS) / sizeof(GENERATORS[0])))
#define FLUSH (K - 1) /* flush bits the encoder appends / decoder emits */

/* The streaming encoder is stateful: encoding in chunks (plus a final flush)
 * must match encoding the whole message in one call, and end in state 0. */
static void test_encode_stream(void) {
  printf("test_encode_stream\n");
  dt_cc_code *code = dt_cc_code_create(K, GENERATORS, N_GEN);
  REQUIRE("code created", code != NULL);
  check("code n", dt_cc_code_n(code) == N_GEN);
  check("code k", dt_cc_code_k(code) == K);

  const int n_info = 200;
  uint8_t *msg = malloc((size_t)n_info);
  for (int i = 0; i < n_info; ++i) msg[i] = bit_sym(i * 7 + 3);

  int data_len = n_info * N_GEN;
  int total_len = data_len + FLUSH * N_GEN;

  uint8_t *ref = malloc((size_t)total_len);
  int rstate = 0, ri = 0;
  unsigned int unknown = 0;
  ri += dt_cc_encoder_encode(code, msg, n_info, &rstate, &unknown, ref);
  ri += dt_cc_encoder_flush(code, &rstate, &unknown, ref + ri);
  check("one-shot length and end state", ri == total_len && rstate == 0);

  uint8_t *out = malloc((size_t)total_len);
  int state = 0, oi = 0;
  const int n1 = 80, n2 = n_info - n1;
  int w = dt_cc_encoder_encode(code, msg, n1, &state, &unknown, out);
  check("chunk 1 length", w == n1 * N_GEN);
  oi += w;
  w = dt_cc_encoder_encode(code, msg + n1, n2, &state, &unknown, out + oi);
  check("chunk 2 length", w == n2 * N_GEN);
  oi += w;
  w = dt_cc_encoder_flush(code, &state, &unknown, out + oi);
  check("flush length", w == FLUSH * N_GEN);
  oi += w;

  check("chunked total length and end state", oi == total_len && state == 0);
  check("chunked matches one-shot", memcmp(out, ref, (size_t)total_len) == 0);
  printf("  %d bits, chunked matches one-shot, end state=%d\n", oi, state);

  free(out);
  free(ref);
  free(msg);
  dt_cc_code_destroy(code);
}

/* A clean continuously-encoded stream, pushed through the decoder in small
 * chunks, must come back out exactly - from bit 0, no warm-up. */
static void test_stream_clean(void) {
  printf("test_stream_clean\n");
  dt_cc_code *code = dt_cc_code_create(K, GENERATORS, N_GEN);
  REQUIRE("code created", code != NULL);

  const int n_info = 300;
  uint8_t *msg = malloc((size_t)n_info);
  for (int i = 0; i < n_info; ++i) msg[i] = bit_sym(i * 7 + 3);

  uint8_t *coded = malloc((size_t)(n_info + FLUSH) * N_GEN);
  int clen = viterbi_encode_all(code, msg, n_info, coded);

  dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
  REQUIRE("decoder created", sd != NULL);

  uint8_t *outbuf = malloc((size_t)n_info + 16);
  int got = stream_decode_all(sd, coded, clen, outbuf, n_info + 16);

  int errors = 0, cmp = got < n_info ? got : n_info;
  for (int i = 0; i < cmp; ++i) errors += (outbuf[i] != msg[i]);
  printf("  decoded %d bits (msg %d + flush %d), errors=%d\n", got, n_info,
         FLUSH, errors);
  check("clean stream recovered exactly", got == n_info + FLUSH && errors == 0);

  dt_cc_viterbi_stream_decoder_destroy(sd);
  free(outbuf);
  free(coded);
  free(msg);
  dt_cc_code_destroy(code);
}

/* A clean stream with a burst of received bits marked erased: an erasure gives
 * no evidence for any branch (a neutral 0 in the metric), so the decoder still
 * recovers the message. */
static void test_stream_erasures(void) {
  printf("test_stream_erasures\n");
  dt_cc_code *code = dt_cc_code_create(K, GENERATORS, N_GEN);
  REQUIRE("code created", code != NULL);

  const int n_info = 300;
  uint8_t *msg = malloc((size_t)n_info);
  for (int i = 0; i < n_info; ++i) msg[i] = bit_sym(i * 7 + 3);

  uint8_t *rx = malloc((size_t)(n_info + FLUSH) * N_GEN);
  int clen = viterbi_encode_all(code, msg, n_info, rx);
  for (int i = 700; i < 720; ++i) rx[i] = DT_ERASURE; /* 20-bit burst */

  dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
  REQUIRE("decoder created", sd != NULL);

  uint8_t *outbuf = malloc((size_t)n_info + 16);
  int got = stream_decode_all(sd, rx, clen, outbuf, n_info + 16);

  int errors = 0, cmp = got < n_info ? got : n_info;
  for (int i = 0; i < cmp; ++i) errors += (outbuf[i] != msg[i]);
  printf("  20-bit erasure burst, decoded %d bits, errors=%d\n", got, errors);
  check("recovered through erasure burst",
        got == n_info + FLUSH && errors == 0);

  dt_cc_viterbi_stream_decoder_destroy(sd);
  free(outbuf);
  free(rx);
  free(msg);
  dt_cc_code_destroy(code);
}

/* Scattered bit flips are corrected: the survivor path stays on the true
 * codeword as long as the errors are within the code's correction power. */
static void test_stream_flips(void) {
  printf("test_stream_flips\n");
  dt_cc_code *code = dt_cc_code_create(K, GENERATORS, N_GEN);
  REQUIRE("code created", code != NULL);

  const int n_info = 300;
  uint8_t *msg = malloc((size_t)n_info);
  for (int i = 0; i < n_info; ++i) msg[i] = bit_sym(i * 7 + 3);

  uint8_t *rx = malloc((size_t)(n_info + FLUSH) * N_GEN);
  int clen = viterbi_encode_all(code, msg, n_info, rx);

  int flips[] = {37, 123, 400, 777, 900, 1100, 1450};
  const int nflip = (int)(sizeof(flips) / sizeof(flips[0]));
  for (int i = 0; i < nflip; ++i)
    rx[flips[i]] ^= DT_VALUE; /* toggle DT_TRUE <-> DT_FALSE */

  dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
  REQUIRE("decoder created", sd != NULL);

  uint8_t *outbuf = malloc((size_t)n_info + 16);
  int got = stream_decode_all(sd, rx, clen, outbuf, n_info + 16);

  int errors = 0, cmp = got < n_info ? got : n_info;
  for (int i = 0; i < cmp; ++i) errors += (outbuf[i] != msg[i]);
  printf("  %d flips, decoded %d bits, errors=%d\n", nflip, got, errors);
  check("flips corrected", got == n_info + FLUSH && errors == 0);

  dt_cc_viterbi_stream_decoder_destroy(sd);
  free(outbuf);
  free(rx);
  free(msg);
  dt_cc_code_destroy(code);
}

/* A built-in standard code (K=7 rate 1/2) encodes and decodes cleanly. */
static void test_standard_code(void) {
  printf("test_standard_code\n");
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  check("standard code n", dt_cc_code_n(code) == 2);
  check("standard code k", dt_cc_code_k(code) == 7);

  const int n_info = 150;
  uint8_t *msg = malloc((size_t)n_info);
  for (int i = 0; i < n_info; ++i) msg[i] = bit_sym(i * 5 + 1);

  uint8_t *coded = malloc((size_t)(n_info + 6) * 2);
  int clen = viterbi_encode_all(code, msg, n_info, coded);

  dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
  REQUIRE("decoder created", sd != NULL);

  uint8_t *outbuf = malloc((size_t)n_info + 16);
  int got = stream_decode_all(sd, coded, clen, outbuf, n_info + 16);

  int errors = 0, cmp = got < n_info ? got : n_info;
  for (int i = 0; i < cmp; ++i) errors += (outbuf[i] != msg[i]);
  printf("  K7R12 decoded %d bits, errors=%d\n", got, errors);
  check("standard code recovered exactly", got == n_info + 6 && errors == 0);

  check("bad preset rejected",
        dt_cc_code_create_standard((dt_cc_standard_code)999) == NULL);

  dt_cc_viterbi_stream_decoder_destroy(sd);
  free(outbuf);
  free(coded);
  free(msg);
  dt_cc_code_destroy(code);
}

/* Every standard preset (the four defaults plus their alternates) must create,
 * report the right rate/K, and round-trip a clean stream exactly. */
static void test_all_presets(void) {
  printf("test_all_presets\n");
  struct {
    dt_cc_standard_code code;
    int n, k;
  } presets[] = {
      {DT_CC_CODE_K3_RATE_1_2, 2, 3},      {DT_CC_CODE_K3_RATE_1_2_ALT1, 2, 3},
      {DT_CC_CODE_K3_RATE_1_2_ALT2, 2, 3}, {DT_CC_CODE_K7_RATE_1_2, 2, 7},
      {DT_CC_CODE_K7_RATE_1_2_ALT1, 2, 7}, {DT_CC_CODE_K7_RATE_1_2_ALT2, 2, 7},
      {DT_CC_CODE_K7_RATE_1_3, 3, 7},      {DT_CC_CODE_K7_RATE_1_3_ALT1, 3, 7},
      {DT_CC_CODE_K7_RATE_1_3_ALT2, 3, 7}, {DT_CC_CODE_K7_RATE_1_3_ALT3, 3, 7},
      {DT_CC_CODE_K7_RATE_1_3_ALT4, 3, 7}, {DT_CC_CODE_K5_RATE_1_5, 5, 5},
      {DT_CC_CODE_K5_RATE_1_5_ALT1, 5, 5}, {DT_CC_CODE_K5_RATE_1_5_ALT2, 5, 5},
      {DT_CC_CODE_K5_RATE_1_5_ALT3, 5, 5}, {DT_CC_CODE_K5_RATE_1_5_ALT4, 5, 5},
  };
  const int np = (int)(sizeof(presets) / sizeof(presets[0]));

  int all_ok = 1;
  for (int idx = 0; idx < np; ++idx) {
    dt_cc_code *code = dt_cc_code_create_standard(presets[idx].code);
    REQUIRE("preset created", code != NULL);
    if (dt_cc_code_n(code) != presets[idx].n ||
        dt_cc_code_k(code) != presets[idx].k) {
      all_ok = 0;
    }

    const int n_info = 200, kk = presets[idx].k;
    uint8_t msg[200];
    for (int i = 0; i < n_info; ++i) msg[i] = bit_sym(i * 5 + 1);
    uint8_t *coded = malloc((size_t)(n_info + kk) * presets[idx].n);
    int clen = viterbi_encode_all(code, msg, n_info, coded);

    dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
    REQUIRE("preset decoder created", sd != NULL);
    int cap = n_info + 32;
    uint8_t *out = malloc((size_t)cap);
    int got = stream_decode_all(sd, coded, clen, out, cap);
    int cmp = got < n_info ? got : n_info, errors = 0;
    for (int i = 0; i < cmp; ++i) errors += (out[i] != msg[i]);
    if (got != n_info + (kk - 1) || errors != 0) {
      all_ok = 0;
    }

    dt_cc_viterbi_stream_decoder_destroy(sd);
    free(out);
    free(coded);
    dt_cc_code_destroy(code);
  }
  printf("  %d presets create, report rate/K, and round-trip cleanly\n", np);
  check("all presets round-trip cleanly", all_ok);
}

static void test_error_paths(void) {
  printf("test_error_paths\n");
  /* Code creation rejects bad arguments by returning NULL. */
  check("create rejects K<2", dt_cc_code_create(1, GENERATORS, N_GEN) == NULL);
  check("create rejects n<1", dt_cc_code_create(K, GENERATORS, 0) == NULL);
  check("dt_cc_code_n(NULL) is -1", dt_cc_code_n(NULL) == -1);

  dt_cc_code *code = dt_cc_code_create(K, GENERATORS, N_GEN);
  REQUIRE("code created", code != NULL);

  /* Encoder rejects an out-of-range carried-in state. */
  uint8_t bit = DT_TRUE, obuf[N_GEN];
  int badstate = 1 << 20;
  unsigned int unknown = 0;
  check("encode rejects bad state",
        dt_cc_encoder_encode(code, &bit, 1, &badstate, &unknown, obuf) == DT_CC_ERR_ARG);

  /* Decoder creation rejects a null code (it takes no other arguments). */
  check("decoder rejects null code",
        dt_cc_viterbi_stream_decoder_create(NULL) == NULL);
  dt_cc_viterbi_stream_decoder_destroy(NULL); /* must not crash */
  check("destroy(NULL) is safe", 1);

  dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
  REQUIRE("decoder created", sd != NULL);

  /* Streaming decode argument checks. */
  uint8_t out8 = 0;
  check("decode rejects null decoder",
        dt_cc_viterbi_stream_decode(NULL, &bit, 1, &out8, 1) == DT_CC_ERR_ARG);
  check("decode rejects null input",
        dt_cc_viterbi_stream_decode(sd, NULL, 1, &out8, 1) == DT_CC_ERR_ARG);
  check("flush rejects null decoder",
        dt_cc_viterbi_stream_decode_flush(NULL, &out8, 1) == DT_CC_ERR_ARG);

  dt_cc_viterbi_stream_decoder_destroy(sd);
  dt_cc_code_destroy(code);
}

int main(void) {
  test_encode_stream();
  test_stream_clean();
  test_stream_erasures();
  test_stream_flips();
  test_standard_code();
  test_all_presets();
  test_error_paths();
  return test_summary("viterbi_decoder");
}
