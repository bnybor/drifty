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
 * Tests for the bcjr codec. The encoder is fully implemented, so it is tested
 * for real (length, chunked == one-shot, flush, error paths, all presets, and
 * the public encoder vtable). The hard and soft decoders are stubs, so they are
 * smoke-tested only: the handle and channel-model validation are exercised, and
 * decode/flush are checked to be well-behaved (no error, no output yet). Update
 * these once the forward-backward algorithm lands.
 */

#include "dt_test_util.h"

#include <drifty/bcjr.h>

/* The four default standard codes, spanning n = 2, 3, 5 and K = 3, 5, 7. */
static const dt_standard_code PRESETS[] = {
    DT_CODE_K3_RATE_1_2, DT_CODE_K7_RATE_1_2, DT_CODE_K7_RATE_1_3,
    DT_CODE_K5_RATE_1_5};
static const char *PRESET_NAMES[] = {"K3_R1_2", "K7_R1_2", "K7_R1_3",
                                     "K5_R1_5"};
#define NUM_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

/* A valid channel model for the decoder stubs. */
static dt_bcjr_stream_params good_params(void) {
  return (dt_bcjr_stream_params){.decision_depth = 40, .p_flip = 0.01f};
}

/* -- encoder (implemented) ------------------------------------------------- */

/* Encoding in two chunks must match a single-shot encode, the coded length must
 * be info_bits*n + (K-1)*n, and flush must drain the register back to state 0. */
static void test_encode_length_and_chunked(void) {
  uint64_t rng = 0xB0BACAFEu;
  const int info_bits = 200;

  for (int p = 0; p < NUM_PRESETS; ++p) {
    dt_ccode *code = dt_ccode_create_standard(PRESETS[p]);
    REQUIRE("code created", code != NULL);
    const int n = dt_ccode_n(code), K = dt_ccode_k(code);

    uint8_t *msg = malloc((size_t)info_bits);
    uint8_t *one = malloc((size_t)(info_bits + K) * (size_t)n);
    uint8_t *two = malloc((size_t)(info_bits + K) * (size_t)n);
    rand_bits(msg, info_bits, &rng);

    int len_one = bcjr_encode_all(code, msg, info_bits, one);

    /* Same message, encoded as 80 + 120 through one running state, then flush. */
    int state = 0, len_two = 0;
    len_two += dt_bcjr_encode(code, msg, 80, &state, two);
    len_two += dt_bcjr_encode(code, msg + 80, 120, &state, two + len_two);
    int flushed = dt_bcjr_encode_flush(code, &state, two + len_two);
    len_two += flushed;

    check(PRESET_NAMES[p], 1);
    check("coded length is info*n + (K-1)*n",
          len_one == info_bits * n + (K - 1) * n);
    check("chunked length matches one-shot", len_two == len_one);
    check("chunked bytes match one-shot",
          memcmp(one, two, (size_t)len_one) == 0);
    check("flush writes (K-1)*n bits", flushed == (K - 1) * n);
    check("flush returns the register to state 0", state == 0);

    free(msg);
    free(one);
    free(two);
    dt_ccode_destroy(code);
  }
}

/* Bad arguments are rejected rather than crashing. */
static void test_encode_error_paths(void) {
  dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  uint8_t msg[4] = {DT_TRUE, DT_FALSE, DT_TRUE, DT_TRUE};
  uint8_t out[64];
  int state = 0;

  check("encode rejects NULL code",
        dt_bcjr_encode(NULL, msg, 4, &state, out) == DT_ERR_ARG);
  check("encode rejects NULL state",
        dt_bcjr_encode(code, msg, 4, NULL, out) == DT_ERR_ARG);
  check("encode rejects NULL out",
        dt_bcjr_encode(code, msg, 4, &state, NULL) == DT_ERR_ARG);
  check("encode rejects negative n_bits",
        dt_bcjr_encode(code, msg, -1, &state, out) == DT_ERR_ARG);
  int bad_state = 999999;
  check("encode rejects out-of-range state",
        dt_bcjr_encode(code, msg, 4, &bad_state, out) == DT_ERR_ARG);
  check("flush rejects NULL code",
        dt_bcjr_encode_flush(NULL, &state, out) == DT_ERR_ARG);

  dt_ccode_destroy(code);
}

/* The public encoder vtable drives begin/encode/finalize and produces the same
 * total length as the engine helper. */
static void test_encoder_vtable(void) {
  check("encoder_create rejects NULL code",
        dt_bcjr_encoder_create(NULL) == NULL);

  dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_ccode_n(code), K = dt_ccode_k(code);
  const int info_bits = 64;

  dt_encoder *enc = dt_bcjr_encoder_create(code);
  REQUIRE("encoder created", enc != NULL);

  uint64_t rng = 0x5151u;
  uint8_t *msg = malloc((size_t)info_bits);
  uint8_t *out = malloc((size_t)(info_bits + K) * (size_t)n);
  rand_bits(msg, info_bits, &rng);

  int len = enc->begin(enc, out, (info_bits + K) * n);
  len += enc->encode(enc, out + len, (size_t)(info_bits + K) * n - len, msg,
                     info_bits);
  len += enc->finalize(enc, out + len, (size_t)(info_bits + K) * n - len);
  check("vtable encode length is info*n + (K-1)*n",
        len == info_bits * n + (K - 1) * n);

  /* A too-small destination is reported, not overrun. */
  dt_encoder *enc2 = dt_bcjr_encoder_create(code);
  REQUIRE("encoder created", enc2 != NULL);
  enc2->begin(enc2, out, 1);
  check("vtable encode rejects too-small dst",
        enc2->encode(enc2, out, 1, msg, info_bits) == DT_ERR_ARG);

  dt_bcjr_encoder_destroy(enc);
  dt_bcjr_encoder_destroy(enc2);
  free(msg);
  free(out);
  dt_ccode_destroy(code);
}

/* -- decoder (stub) -------------------------------------------------------- */

/* The engine validates its channel model and rejects bad settings with NULL. */
static void test_decoder_param_validation(void) {
  dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_bcjr_stream_params p = good_params();

  check("create rejects NULL code",
        dt_bcjr_stream_decoder_create(NULL, &p) == NULL);
  check("create rejects NULL params",
        dt_bcjr_stream_decoder_create(code, NULL) == NULL);

  dt_bcjr_stream_params bad = p;
  bad.decision_depth = 0;
  check("create rejects decision_depth < 1",
        dt_bcjr_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.p_flip = 0.0f;
  check("create rejects p_flip == 0",
        dt_bcjr_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.p_flip = 1.0f;
  check("create rejects p_flip == 1",
        dt_bcjr_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.p_erase = 1.0f;
  check("create rejects p_erase == 1",
        dt_bcjr_stream_decoder_create(code, &bad) == NULL);

  dt_ccode_destroy(code);
}

/* The stub engine decodes without error and (for now) emits no bits. */
static void test_decoder_stub_smoke(void) {
  dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_bcjr_stream_params p = good_params();

  dt_bcjr_stream_decoder *d = dt_bcjr_stream_decoder_create(code, &p);
  REQUIRE("decoder created", d != NULL);

  uint8_t in[128];
  for (int i = 0; i < 128; ++i) in[i] = (i & 1) ? DT_TRUE : DT_FALSE;
  uint8_t out[128];
  dt_bcjr_decode_details details[128];

  int got = dt_bcjr_stream_decode(d, in, 128, out, details, 128);
  check("stub decode returns no error", got >= 0);
  check("stub decode emits nothing yet", got == 0);
  int drained = dt_bcjr_stream_decode_flush(d, out, details, 128);
  check("stub flush returns no error", drained >= 0);
  check("stub flush emits nothing yet", drained == 0);

  check("decode rejects NULL decoder",
        dt_bcjr_stream_decode(NULL, in, 128, out, NULL, 128) == DT_ERR_ARG);

  dt_bcjr_stream_decoder_destroy(d);
  dt_bcjr_stream_decoder_destroy(NULL); /* NULL is a no-op */
  dt_ccode_destroy(code);
}

/* The public hard-decision decoder vtable is wired over the stub engine. */
static void test_decoder_vtable(void) {
  dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_bcjr_stream_params p = good_params();

  check("decoder_create rejects NULL code",
        dt_bcjr_decoder_create(NULL, &p) == NULL);
  check("decoder_create rejects NULL params",
        dt_bcjr_decoder_create(code, NULL) == NULL);

  dt_decoder *dec = dt_bcjr_decoder_create(code, &p);
  REQUIRE("decoder created", dec != NULL);

  uint8_t in[64], out[64];
  for (int i = 0; i < 64; ++i) in[i] = DT_FALSE;
  check("vtable begin ok", dec->begin(dec, out, 64) >= 0);
  check("vtable decode returns no error",
        dec->decode(dec, out, 64, in, 64) == 0);
  check("vtable finalize returns no error", dec->finalize(dec, out, 64) == 0);

  dt_bcjr_decoder_destroy(dec);
  dt_bcjr_decoder_destroy(NULL); /* NULL is a no-op */
  dt_ccode_destroy(code);
}

/* The public soft-output decoder vtable is wired over the stub engine. */
static void test_soft_decoder_vtable(void) {
  dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_bcjr_stream_params p = good_params();

  check("soft_decoder_create rejects NULL code",
        dt_bcjr_soft_decoder_create(NULL, &p) == NULL);
  check("soft_decoder_create rejects NULL params",
        dt_bcjr_soft_decoder_create(code, NULL) == NULL);

  dt_soft_decoder *sd = dt_bcjr_soft_decoder_create(code, &p);
  REQUIRE("soft decoder created", sd != NULL);

  uint8_t in[64];
  for (int i = 0; i < 64; ++i) in[i] = DT_TRUE;
  dt_soft_decoder_out soft[64];
  check("soft begin ok", sd->begin(sd, NULL, 0) >= 0);
  check("soft decode returns no error", sd->decode(sd, soft, 64, in, 64) == 0);
  check("soft finalize returns no error", sd->finalize(sd, soft, 64) == 0);

  dt_bcjr_soft_decoder_destroy(sd);
  dt_bcjr_soft_decoder_destroy(NULL); /* NULL is a no-op */
  dt_ccode_destroy(code);
}

int main(void) {
  printf("bcjr encoder:\n");
  test_encode_length_and_chunked();
  test_encode_error_paths();
  test_encoder_vtable();
  printf("bcjr decoder (stub):\n");
  test_decoder_param_validation();
  test_decoder_stub_smoke();
  test_decoder_vtable();
  test_soft_decoder_vtable();
  return test_summary("bcjr");
}
