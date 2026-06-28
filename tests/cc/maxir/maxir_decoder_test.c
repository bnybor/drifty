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
 * Tests for the maxir codec. The encoder is exercised for real (length, chunked
 * == one-shot, flush, error paths, all presets, and the public encoder vtable).
 * The decoder is the drift-tolerant max-log-MAP core, tested end to end:
 * channel-model validation, clean round trip and flip correction across the
 * presets, the soft-output invariants, the DT_INVALID poison contract and
 * erasure handling, blind acquisition, the hard and soft decoder vtables, and -
 * with max_drift > 0 - re-anchoring through cumulative and mid-group indels,
 * blind acquisition under drift, and re-acquisition after a sustained loss.
 */

#include "dt_test_util.h"

#include <drifty/cc/encoder.h>
#include <drifty/cc/maxir.h>

#include <math.h>

/* The four default standard codes, spanning n = 2, 3, 5 and K = 3, 5, 7. */
static const dt_cc_standard_code PRESETS[] = {
    DT_CC_CODE_K3_RATE_1_2, DT_CC_CODE_K7_RATE_1_2, DT_CC_CODE_K7_RATE_1_3,
    DT_CC_CODE_K5_RATE_1_5};
static const char *PRESET_NAMES[] = {"K3_R1_2", "K7_R1_2", "K7_R1_3",
                                     "K5_R1_5"};
#define NUM_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

/* A valid channel model for the decoder (synchronous: no drift). */
static dt_cc_maxir_stream_params good_params(void) {
  return (dt_cc_maxir_stream_params){.decision_depth = 40, .p_flip = 0.01f};
}

/* -- encoder (implemented) ------------------------------------------------- */

/* Encoding in two chunks must match a single-shot encode, the coded length must
 * be info_bits*n + (K-1)*n, and flush must drain the register back to state 0. */
static void test_encode_length_and_chunked(void) {
  uint64_t rng = 0xB0BACAFEu;
  const int info_bits = 200;

  for (int p = 0; p < NUM_PRESETS; ++p) {
    dt_cc_code *code = dt_cc_code_create_standard(PRESETS[p]);
    REQUIRE("code created", code != NULL);
    const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);

    uint8_t *msg = malloc((size_t)info_bits);
    uint8_t *one = malloc((size_t)(info_bits + K) * (size_t)n);
    uint8_t *two = malloc((size_t)(info_bits + K) * (size_t)n);
    rand_bits(msg, info_bits, &rng);

    int len_one = maxir_encode_all(code, msg, info_bits, one);

    /* Same message, encoded as 80 + 120 through one running state, then flush. */
    int state = 0, len_two = 0;
    unsigned int unknown = 0;
    len_two += dt_cc_encoder_encode(code, msg, 80, &state, &unknown, two);
    len_two += dt_cc_encoder_encode(code, msg + 80, 120, &state, &unknown, two + len_two);
    int flushed = dt_cc_encoder_flush(code, &state, &unknown, two + len_two);
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
    dt_cc_code_destroy(code);
  }
}

/* Bad arguments are rejected rather than crashing. */
static void test_encode_error_paths(void) {
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  uint8_t msg[4] = {DT_TRUE, DT_FALSE, DT_TRUE, DT_TRUE};
  uint8_t out[64];
  int state = 0;
  unsigned int unknown = 0;

  check("encode rejects NULL code",
        dt_cc_encoder_encode(NULL, msg, 4, &state, &unknown, out) == DT_ERR_ARG);
  check("encode rejects NULL state",
        dt_cc_encoder_encode(code, msg, 4, NULL, &unknown, out) == DT_ERR_ARG);
  check("encode rejects NULL unknown",
        dt_cc_encoder_encode(code, msg, 4, &state, NULL, out) == DT_ERR_ARG);
  check("encode rejects NULL out",
        dt_cc_encoder_encode(code, msg, 4, &state, &unknown, NULL) == DT_ERR_ARG);
  check("encode rejects negative n_bits",
        dt_cc_encoder_encode(code, msg, -1, &state, &unknown, out) == DT_ERR_ARG);
  int bad_state = 999999;
  check("encode rejects out-of-range state",
        dt_cc_encoder_encode(code, msg, 4, &bad_state, &unknown, out) == DT_ERR_ARG);
  check("flush rejects NULL code",
        dt_cc_encoder_flush(NULL, &state, &unknown, out) == DT_ERR_ARG);

  dt_cc_code_destroy(code);
}

/* The encoder vtable drives begin/encode/finalize and produces the same
 * total length as the engine helper. */
static void test_encoder_vtable(void) {
  check("encoder_create rejects NULL code",
        dt_cc_encoder_create(NULL) == NULL);

  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 64;

  dt_stream_encoder *enc = dt_cc_encoder_create(code);
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
  dt_stream_encoder *enc2 = dt_cc_encoder_create(code);
  REQUIRE("encoder created", enc2 != NULL);
  enc2->begin(enc2, out, 1);
  check("vtable encode rejects too-small dst",
        enc2->encode(enc2, out, 1, msg, info_bits) == DT_ERR_ARG);

  dt_cc_encoder_destroy(enc);
  dt_cc_encoder_destroy(enc2);
  free(msg);
  free(out);
  dt_cc_code_destroy(code);
}

/* -- decoder --------------------------------------------------------------- */

/* Decode an entire received buffer, feeding it in two chunks and then flushing,
 * to exercise the streaming pump. Fills out[]/details[] (details may be NULL)
 * and returns the number of decisions produced. */
static int maxir_decode_all(const dt_cc_code *code, const dt_cc_maxir_stream_params *p,
                           const uint8_t *rx, int rx_len, uint8_t *out,
                           dt_cc_maxir_decode_details *details, int out_cap) {
  dt_cc_maxir_stream_decoder *d = dt_cc_maxir_stream_decoder_create(code, p);
  if (!d) {
    return -1;
  }
  int total = 0;
  const int split = rx_len / 2;
  const int chunks[2] = {split, rx_len - split};
  int off = 0;
  for (int c = 0; c < 2 && total < out_cap; ++c) {
    int got = dt_cc_maxir_stream_decode(
        d, rx + off, chunks[c], out + total,
        details ? details + total : NULL, out_cap - total);
    if (got < 0) {
      dt_cc_maxir_stream_decoder_destroy(d);
      return got;
    }
    total += got;
    off += chunks[c];
  }
  for (;;) {
    int got = dt_cc_maxir_stream_decode_flush(
        d, out + total, details ? details + total : NULL, out_cap - total);
    if (got <= 0) {
      break;
    }
    total += got;
    if (total >= out_cap) {
      break;
    }
  }
  dt_cc_maxir_stream_decoder_destroy(d);
  return total;
}

/* The engine validates its channel model and rejects bad settings with NULL. A
 * drift request (max_drift > 0) is accepted with indel rates and rejected
 * without. */
static void test_decoder_param_validation(void) {
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_cc_maxir_stream_params p = good_params();

  check("create rejects NULL code",
        dt_cc_maxir_stream_decoder_create(NULL, &p) == NULL);
  check("create rejects NULL params",
        dt_cc_maxir_stream_decoder_create(code, NULL) == NULL);

  dt_cc_maxir_stream_params bad = p;
  bad.decision_depth = 0;
  check("create rejects decision_depth < 1",
        dt_cc_maxir_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.p_flip = 0.0f;
  check("create rejects p_flip == 0",
        dt_cc_maxir_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.p_flip = 1.0f;
  check("create rejects p_flip == 1",
        dt_cc_maxir_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.p_ovr_erase = 1.0f;
  check("create rejects p_ovr_erase == 1",
        dt_cc_maxir_stream_decoder_create(code, &bad) == NULL);
  bad = p;
  bad.max_drift = -1;
  check("create rejects max_drift < 0",
        dt_cc_maxir_stream_decoder_create(code, &bad) == NULL);

  /* Drift tracking needs both an insertion mass and a deletion rate. */
  bad = p;
  bad.max_drift = 4;
  check("create rejects max_drift > 0 without indel rates",
        dt_cc_maxir_stream_decoder_create(code, &bad) == NULL);
  dt_cc_maxir_stream_params drift =
      make_params(40, 4, 0.01, 0.01, 0.01, 0.0);
  dt_cc_maxir_stream_decoder *sd = dt_cc_maxir_stream_decoder_create(code, &drift);
  check("create accepts max_drift > 0 with indel rates", sd != NULL);
  dt_cc_maxir_stream_decoder_destroy(sd);
  drift.p_del = 1.0f; /* p_ins + p_del >= 1 */
  check("create rejects p_ins + p_del >= 1",
        dt_cc_maxir_stream_decoder_create(code, &drift) == NULL);

  dt_cc_code_destroy(code);
}

/* Clean channel: one decision per coded group, the warm-up prefix reads as
 * DT_ABSENT, and every committed bit past warm-up matches the message. Then a
 * light flip rate is corrected back to the message. */
static void test_decoder_roundtrip(void) {
  uint64_t rng = 0xD0D0CACAu;
  const int info_bits = 500;

  for (int pi = 0; pi < NUM_PRESETS; ++pi) {
    dt_cc_code *code = dt_cc_code_create_standard(PRESETS[pi]);
    REQUIRE("code created", code != NULL);
    const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
    const int depth = 40;
    const int warmup = depth + 4 * K; /* unreliable left-context region */
    const int cap = (info_bits + K) * n;

    uint8_t *msg = malloc((size_t)info_bits);
    uint8_t *coded = malloc((size_t)cap);
    uint8_t *out = malloc((size_t)(info_bits + K + 8));
    rand_bits(msg, info_bits, &rng);
    int clen = maxir_encode_all(code, msg, info_bits, coded);

    dt_cc_maxir_stream_params p = {.decision_depth = depth, .p_flip = 0.01f};
    int got = maxir_decode_all(code, &p, coded, clen, out, NULL, info_bits + K + 8);

    check(PRESET_NAMES[pi], 1);
    /* one decision per group: info_bits + (K-1) flush groups */
    check("decision count == groups", got == info_bits + (K - 1));
    int absent = 0;
    for (int i = 0; i < warmup; ++i) {
      if (out[i] == DT_ABSENT) ++absent;
    }
    check("warm-up emits DT_ABSENT", absent > 0);
    int mism = 0;
    for (int i = warmup; i < info_bits; ++i) {
      if (out[i] != msg[i]) ++mism;
    }
    check("clean round trip recovers the message", mism == 0);

    /* ~1.5% flips: a good convolutional code corrects them all here. */
    uint8_t *rx = malloc((size_t)clen);
    memcpy(rx, coded, (size_t)clen);
    for (int i = 0; i < clen; ++i) {
      if ((rng_next(&rng) % 1000u) < 15u) {
        rx[i] = (rx[i] == DT_TRUE) ? DT_FALSE : DT_TRUE;
      }
    }
    dt_cc_maxir_stream_params pf = {.decision_depth = depth, .p_flip = 0.02f};
    got = maxir_decode_all(code, &pf, rx, clen, out, NULL, info_bits + K + 8);
    int err = 0;
    for (int i = warmup; i < info_bits; ++i) {
      if (out[i] != msg[i]) ++err;
    }
    check("flip correction recovers the message", err <= 2);

    free(msg);
    free(coded);
    free(out);
    free(rx);
    dt_cc_code_destroy(code);
  }
}

/* The soft output is internally consistent at every step: all six consistencies
 * lie in [0, 1], the winning value reads 1 while the loser reads c_lost (so
 * c_true + c_false == 1 + c_lost and c_lost == min(c_true, c_false)), and a
 * resolved hard symbol agrees with argmax(c_true, c_false). */
static void test_decoder_soft_invariants(void) {
  uint64_t rng = 0x50F750F7u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 300;
  const int cap = (info_bits + K) * n;

  uint8_t *msg = malloc((size_t)info_bits);
  uint8_t *coded = malloc((size_t)cap);
  rand_bits(msg, info_bits, &rng);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  /* a mix of flips and erasures so c_lost actually varies */
  uint8_t *rx = malloc((size_t)clen);
  memcpy(rx, coded, (size_t)clen);
  for (int i = 0; i < clen; ++i) {
    unsigned int u = (unsigned int)(rng_next(&rng) % 1000u);
    if (u < 20u) rx[i] = (rx[i] == DT_TRUE) ? DT_FALSE : DT_TRUE;
    else if (u < 50u) rx[i] = DT_ERASURE;
  }

  const int outc = info_bits + K + 8;
  uint8_t *sym = malloc((size_t)outc);
  dt_cc_maxir_decode_details *det = malloc(sizeof(*det) * (size_t)outc);
  dt_cc_maxir_stream_params p = {.decision_depth = 40, .p_flip = 0.02f,
                             .p_ovr_erase = 0.03f};
  int got = maxir_decode_all(code, &p, rx, clen, sym, det, outc);
  REQUIRE("decode produced output", got > 0);

  int range_ok = 1, sum_ok = 1, lost_ok = 1, argmax_ok = 1;
  for (int i = 0; i < got; ++i) {
    const dt_cc_maxir_decode_details *q = &det[i];
    const float v[6] = {q->c_true,    q->c_false, q->c_lost,
                        q->c_invalid, q->c_lock,  q->c_absent};
    for (int k = 0; k < 6; ++k) {
      if (!(v[k] >= -1e-5f && v[k] <= 1.0f + 1e-5f)) range_ok = 0;
    }
    /* Skip starvation positions (no surviving path -> all value fields 0), where
     * the sum/argmax invariants do not apply. */
    if (q->c_true == 0.0f && q->c_false == 0.0f && q->c_lost == 0.0f) {
      continue;
    }
    if (fabsf((q->c_true + q->c_false) - (1.0f + q->c_lost)) > 1e-3f) sum_ok = 0;
    const float mn = q->c_true < q->c_false ? q->c_true : q->c_false;
    if (fabsf(mn - q->c_lost) > 1e-4f) lost_ok = 0;
    if (sym[i] == DT_TRUE && !(q->c_true >= q->c_false)) argmax_ok = 0;
    if (sym[i] == DT_FALSE && !(q->c_false >= q->c_true)) argmax_ok = 0;
  }
  check("soft fields all in [0, 1]", range_ok);
  check("c_true + c_false == 1 + c_lost", sum_ok);
  check("c_lost == min(c_true, c_false)", lost_ok);
  check("hard T/F matches argmax(c_true, c_false)", argmax_ok);

  free(msg);
  free(coded);
  free(rx);
  free(sym);
  free(det);
  dt_cc_code_destroy(code);
}

/* The encoder marks a non-boolean input on exactly the coded bits that would
 * carry its value, per kind. A DT_INVALID input is structural poison: those bits
 * are emitted DT_INVALID, the originating slot is a true tie and decodes to
 * DT_INVALID (c_invalid == 1), with no erasure channel model needed. A DT_ERASURE
 * input is an unbound value deferred to the channel: those bits are emitted
 * DT_ERASURE, and with an erasure channel model (p_ovr_erase) the slot decodes
 * back to DT_ERASURE, not poison. Clean bits on either side recover in both
 * cases. A long erasure burst destroys the value over its span (DT_ERASURE /
 * DT_ABSENT, never a confident bit), and bits away from it still decode. */
static void test_decoder_invalid_and_erasure(void) {
  uint64_t rng = 0x12345678u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 240;
  const int depth = 40;
  const int warmup = depth + 4 * K;
  const int cap = (info_bits + K) * n;

  uint8_t *msg = malloc((size_t)info_bits);
  rand_bits(msg, info_bits, &rng);
  const int poison_at = 120;

  uint8_t *coded = malloc((size_t)cap);
  const int outc = info_bits + K + 8;
  uint8_t *sym = malloc((size_t)outc);
  dt_cc_maxir_decode_details *det = malloc(sizeof(*det) * (size_t)outc);

  /* (a) DT_INVALID input: structural poison -> a DT_INVALID tie, no erasure
   * channel model needed. */
  msg[poison_at] = DT_INVALID;
  int clen = maxir_encode_all(code, msg, info_bits, coded);
  dt_cc_maxir_stream_params pi = {.decision_depth = depth, .p_flip = 0.01f};
  int got = maxir_decode_all(code, &pi, coded, clen, sym, det, outc);
  REQUIRE("decode produced output", got > info_bits);

  check("invalid input decodes DT_INVALID", sym[poison_at] == DT_INVALID);
  check("invalid slot c_invalid == 1", det[poison_at].c_invalid > 0.99f);
  int around_ok = 1;
  for (int i = warmup; i < poison_at - 1; ++i) {
    if (sym[i] != msg[i]) around_ok = 0;
  }
  for (int i = poison_at + K; i < info_bits; ++i) {
    if (sym[i] != msg[i]) around_ok = 0;
  }
  check("clean bits around the invalid recover", around_ok);

  /* (b) DT_ERASURE input: an unbound value deferred to the channel. The coded
   * bits carrying it are emitted DT_ERASURE; told to expect erased coded bits
   * (p_ovr_erase), the decoder reads the slot back as DT_ERASURE, not poison. */
  msg[poison_at] = DT_ERASURE;
  clen = maxir_encode_all(code, msg, info_bits, coded);
  dt_cc_maxir_stream_params px = {.decision_depth = depth, .p_flip = 0.01f,
                              .p_ovr_erase = 0.05f};
  got = maxir_decode_all(code, &px, coded, clen, sym, det, outc);
  REQUIRE("erasure decode produced output", got > info_bits);

  check("erasure input decodes DT_ERASURE", sym[poison_at] == DT_ERASURE);
  check("erasure slot is not poison", det[poison_at].c_invalid < 0.5f);
  around_ok = 1;
  for (int i = warmup; i < poison_at - 1; ++i) {
    if (sym[i] != msg[i]) around_ok = 0;
  }
  for (int i = poison_at + K; i < info_bits; ++i) {
    if (sym[i] != msg[i]) around_ok = 0;
  }
  check("clean bits around the erasure recover", around_ok);

  /* Now an erasure burst in the coded stream over info slots [150, 168). */
  uint8_t *rx = malloc((size_t)clen);
  memcpy(rx, coded, (size_t)clen);
  for (int i = 150 * n; i < 168 * n && i < clen; ++i) rx[i] = DT_ERASURE;
  dt_cc_maxir_stream_params pe = {.decision_depth = depth, .p_flip = 0.01f,
                              .p_ovr_erase = 0.05f};
  got = maxir_decode_all(code, &pe, rx, clen, sym, det, outc);
  int lost = 0, span = 0;
  for (int i = 155; i < 165; ++i) {
    ++span;
    if (sym[i] == DT_ERASURE || sym[i] == DT_ABSENT) ++lost;
  }
  check("erasure burst reads as lost", lost >= span - 2);
  int far_ok = 1;
  /* a window clear of both the poison span [poison_at, poison_at+K) and the
   * burst [150, 168) */
  for (int i = poison_at + K; i < 145; ++i) {
    if (sym[i] != msg[i]) far_ok = 0;
  }
  check("bits away from the burst still decode", far_ok);

  free(msg);
  free(coded);
  free(rx);
  free(sym);
  free(det);
  dt_cc_code_destroy(code);
}

/* Tapping into the middle of a coded stream (no leading sync) still locks and
 * recovers: decoded position j corresponds to original info bit skip + j. */
static void test_decoder_blind_acquisition(void) {
  uint64_t rng = 0xACAC99u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 500;
  const int depth = 40;
  const int warmup = depth + 4 * K;
  const int cap = (info_bits + K) * n;

  uint8_t *msg = malloc((size_t)info_bits);
  uint8_t *coded = malloc((size_t)cap);
  rand_bits(msg, info_bits, &rng);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  const int skip_groups = 80;
  const int skip = skip_groups * n;
  const int outc = info_bits + K + 8;
  uint8_t *out = malloc((size_t)outc);
  dt_cc_maxir_stream_params p = {.decision_depth = depth, .p_flip = 0.01f};
  int got = maxir_decode_all(code, &p, coded + skip, clen - skip, out, NULL, outc);
  REQUIRE("decode produced output", got > 0);

  int err = 0, checked = 0;
  for (int j = warmup; j + skip_groups < info_bits; ++j) {
    ++checked;
    if (out[j] != msg[skip_groups + j]) ++err;
  }
  check("blind acquisition recovers after lock", checked > 0 && err == 0);

  free(msg);
  free(coded);
  free(out);
  dt_cc_code_destroy(code);
}

/* The public hard-decision decoder vtable decodes end to end. */
static void test_decoder_vtable(void) {
  uint64_t rng = 0x4242u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_cc_maxir_stream_params p = good_params();

  check("decoder_create rejects NULL code",
        dt_cc_maxir_decoder_create(NULL, &p) == NULL);
  check("decoder_create rejects NULL params",
        dt_cc_maxir_decoder_create(code, NULL) == NULL);

  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 200;
  const int warmup = 40 + 4 * K;
  const int cap = (info_bits + K) * n;
  uint8_t *msg = malloc((size_t)info_bits);
  uint8_t *coded = malloc((size_t)cap);
  uint8_t *out = malloc((size_t)(info_bits + K + 8));
  rand_bits(msg, info_bits, &rng);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  dt_stream_decoder *dec = dt_cc_maxir_decoder_create(code, &p);
  REQUIRE("decoder created", dec != NULL);
  REQUIRE("vtable begin ok", dec->begin(dec, out, info_bits + K + 8) >= 0);
  int total = 0;
  int got = dec->decode(dec, out + total, (size_t)(info_bits + K + 8 - total),
                        coded, (size_t)clen);
  REQUIRE("vtable decode ok", got >= 0);
  total += got;
  for (;;) {
    int g = dec->finalize(dec, out + total, (size_t)(info_bits + K + 8 - total));
    if (g <= 0) break;
    total += g;
  }
  int mism = 0;
  for (int i = warmup; i < info_bits; ++i) {
    if (out[i] != msg[i]) ++mism;
  }
  check("vtable hard round trip recovers the message",
        total == info_bits + (K - 1) && mism == 0);

  dt_cc_maxir_decoder_destroy(dec);
  dt_cc_maxir_decoder_destroy(NULL); /* NULL is a no-op */
  free(msg);
  free(coded);
  free(out);
  dt_cc_code_destroy(code);
}

/* The public soft-output decoder vtable decodes end to end and the resolved bit
 * (argmax of the soft pair) recovers the message past warm-up. */
static void test_soft_decoder_vtable(void) {
  uint64_t rng = 0x9001u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  dt_cc_maxir_stream_params p = good_params();

  check("soft_decoder_create rejects NULL code",
        dt_cc_maxir_soft_decoder_create(NULL, &p) == NULL);
  check("soft_decoder_create rejects NULL params",
        dt_cc_maxir_soft_decoder_create(code, NULL) == NULL);

  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 200;
  const int warmup = 40 + 4 * K;
  const int cap = (info_bits + K) * n;
  uint8_t *msg = malloc((size_t)info_bits);
  uint8_t *coded = malloc((size_t)cap);
  dt_soft_bit *soft = malloc(sizeof(*soft) * (size_t)(info_bits + K + 8));
  rand_bits(msg, info_bits, &rng);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  dt_stream_soft_decoder *sd = dt_cc_maxir_soft_decoder_create(code, &p);
  REQUIRE("soft decoder created", sd != NULL);
  REQUIRE("soft begin ok", sd->begin(sd, NULL, 0) >= 0);
  int total = 0;
  int got = sd->decode(sd, soft + total, (size_t)(info_bits + K + 8 - total),
                       coded, (size_t)clen);
  REQUIRE("soft decode ok", got >= 0);
  total += got;
  for (;;) {
    int g = sd->finalize(sd, soft + total, (size_t)(info_bits + K + 8 - total));
    if (g <= 0) break;
    total += g;
  }
  int mism = 0, locked_ok = 1;
  for (int i = warmup; i < info_bits; ++i) {
    uint8_t bit = soft[i].c_true >= soft[i].c_false ? DT_TRUE : DT_FALSE;
    if (bit != msg[i]) ++mism;
    if (soft[i].c_locked < 0.0f || soft[i].c_locked > 1.0001f) locked_ok = 0;
  }
  check("soft vtable round trip recovers the message",
        total == info_bits + (K - 1) && mism == 0);
  check("soft vtable populates c_locked", locked_ok);

  dt_cc_maxir_soft_decoder_destroy(sd);
  dt_cc_maxir_soft_decoder_destroy(NULL); /* NULL is a no-op */
  free(msg);
  free(coded);
  free(soft);
  dt_cc_code_destroy(code);
}

/* -- drift (max_drift > 0) ------------------------------------------------- */

/* Count mismatches of out[lo, hi) against msg[off + (i-lo)], over the available
 * output. Returns the comparison count via *counted. */
static int region_errors(const uint8_t *out, int got, const uint8_t *msg,
                         int lo, int hi, int off, int *counted) {
  int err = 0, n = 0;
  for (int i = lo; i < hi && i < got; ++i) {
    ++n;
    if (out[i] != msg[off + (i - lo)]) {
      ++err;
    }
  }
  *counted = n;
  return err;
}

/* Cumulative deletions (one coded bit every 100) push drift steadily negative;
 * re-anchoring keeps the window centred and recovers the message exactly. */
static void test_decoder_reanchor(void) {
  uint64_t rng = 0x5EED01u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 600, depth = 48, warmup = depth + 6 * K;

  uint8_t *msg = malloc((size_t)info_bits);
  rand_bits(msg, info_bits, &rng);
  uint8_t *coded = malloc((size_t)(info_bits + K) * (size_t)n);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  uint8_t *rx = malloc((size_t)clen);
  int rl = 0, ndel = 0;
  for (int i = 0; i < clen; ++i) {
    if ((i + 1) % 100 == 0) {
      ++ndel;
      continue;
    }
    rx[rl++] = coded[i];
  }

  const int outc = info_bits + K + 8;
  uint8_t *out = malloc((size_t)outc);
  dt_cc_maxir_stream_params p = make_params(depth, 6, 0.01, 0.01, 0.01, 0.0);
  int got = maxir_decode_all(code, &p, rx, rl, out, NULL, outc);

  int counted = 0;
  int err = region_errors(out, got, msg, warmup, info_bits, warmup, &counted);
  check("reanchor: cumulative deletions recovered", counted > 200 && err == 0);

  free(msg);
  free(coded);
  free(rx);
  free(out);
  dt_cc_code_destroy(code);
}

/* An insertion and a deletion off the group boundary (phase 2 and 62 of every
 * 120 coded bits) exercise the bit-level alignment, not just group-aligned
 * indels; the message recovers exactly. */
static void test_decoder_midgroup_indel(void) {
  uint64_t rng = 0x71D6A0u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 600, depth = 48, warmup = depth + 6 * K;

  uint8_t *msg = malloc((size_t)info_bits);
  rand_bits(msg, info_bits, &rng);
  uint8_t *coded = malloc((size_t)(info_bits + K) * (size_t)n);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  uint8_t *rx = malloc((size_t)clen + 64);
  int rl = 0, ndel = 0, nins = 0;
  for (int i = 0; i < clen; ++i) {
    const int phase = i % 120;
    if (phase == 2) {
      ++ndel;
      continue;
    }
    if (phase == 62) {
      rx[rl++] = bit_sym((unsigned int)(i >> 1));
      ++nins;
    }
    rx[rl++] = coded[i];
  }

  const int outc = info_bits + K + 8;
  uint8_t *out = malloc((size_t)outc);
  dt_cc_maxir_stream_params p = make_params(depth, 6, 0.01, 0.01, 0.01, 0.0);
  int got = maxir_decode_all(code, &p, rx, rl, out, NULL, outc);

  int counted = 0;
  int err = region_errors(out, got, msg, warmup, info_bits, warmup, &counted);
  check("mid-group indels recovered", counted > 200 && err == 0);

  free(msg);
  free(coded);
  free(rx);
  free(out);
  dt_cc_code_destroy(code);
}

/* Random insertions and deletions PLUS substitution noise together: the codec
 * tracks the drift and corrects the flips, recovering the message (a handful of
 * residual errors tolerated). */
static void test_decoder_drift_plus_flip(void) {
  uint64_t rng = 0xD81F11Fu;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_3);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 600, depth = 48, warmup = depth + 6 * K;

  uint8_t *msg = malloc((size_t)info_bits);
  rand_bits(msg, info_bits, &rng);
  uint8_t *coded = malloc((size_t)(info_bits + K) * (size_t)n);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  uint8_t *ins = malloc((size_t)(2 * clen + 64));
  int il = insert_channel(coded, clen, 0.01, &rng, ins);
  uint8_t *rx = malloc((size_t)il);
  int rl = delete_channel(ins, il, 0.01, &rng, rx);
  flip_channel(rx, rl, 0.01, &rng);

  const int outc = info_bits + K + 16;
  uint8_t *out = malloc((size_t)outc);
  dt_cc_maxir_stream_params p = make_params(depth, 8, 0.02, 0.02, 0.02, 0.0);
  int got = maxir_decode_all(code, &p, rx, rl, out, NULL, outc);

  int counted = 0;
  int err = region_errors(out, got, msg, warmup, info_bits, warmup, &counted);
  check("drift + flips recovered", counted > 200 && err <= counted / 50);

  free(msg);
  free(coded);
  free(ins);
  free(rx);
  free(out);
  dt_cc_code_destroy(code);
}

/* Blind acquisition under drift: tap the stream mid-flight (no leading sync) on a
 * channel that also drops bits, and recover the message past warm-up. */
static void test_decoder_blind_acquisition_under_drift(void) {
  uint64_t rng = 0xB11D04u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 700, depth = 48, warmup = depth + 6 * K;

  uint8_t *msg = malloc((size_t)info_bits);
  rand_bits(msg, info_bits, &rng);
  uint8_t *coded = malloc((size_t)(info_bits + K) * (size_t)n);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  /* Tap at a clean group boundary, then drop bits over the tail. */
  const int skip_groups = 100, skip = skip_groups * n;
  uint8_t *rx = malloc((size_t)clen);
  int rl = delete_channel(coded + skip, clen - skip, 0.01, &rng, rx);

  const int outc = info_bits + K + 8;
  uint8_t *out = malloc((size_t)outc);
  dt_cc_maxir_stream_params p = make_params(depth, 6, 0.01, 0.01, 0.01, 0.0);
  int got = maxir_decode_all(code, &p, rx, rl, out, NULL, outc);

  int counted = 0;
  int err = region_errors(out, got, msg, warmup, info_bits - skip_groups,
                          skip_groups + warmup, &counted);
  check("blind acquisition under drift recovers", counted > 200 && err == 0);

  free(msg);
  free(coded);
  free(rx);
  free(out);
  dt_cc_code_destroy(code);
}

/* Re-acquisition after a sustained loss: a long mid-stream burst of substitution
 * noise wrecks decoding over its span, but the decoder re-locks - via the drift
 * re-anchoring and trellis forgetting, backed by a trellis re-seed if the lock
 * fully collapses - and recovers the post-burst message downstream. The burst can
 * leave a small residual drift, so the recovered run sits at a shifted output
 * index (found by search). */
static void test_decoder_reacquire(void) {
  uint64_t rng = 0x9EACFEu;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  REQUIRE("code created", code != NULL);
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int info_bits = 1000, depth = 40, warmup = depth + 4 * K;

  uint8_t *msg = malloc((size_t)info_bits);
  rand_bits(msg, info_bits, &rng);
  uint8_t *coded = malloc((size_t)(info_bits + K) * (size_t)n);
  int clen = maxir_encode_all(code, msg, info_bits, coded);

  /* Overwrite a 160-group stretch with random noise (>> 2*decision_depth). */
  const int burst_lo = 400, burst_hi = 560;
  uint8_t *rx = malloc((size_t)clen);
  memcpy(rx, coded, (size_t)clen);
  for (int i = burst_lo * n; i < burst_hi * n; ++i) {
    rx[i] = bit_sym((unsigned int)rng_next(&rng));
  }

  const int outc = info_bits + K + 8;
  uint8_t *out = malloc((size_t)outc);
  dt_cc_maxir_stream_params p = make_params(depth, 4, 0.01, 0.01, 0.01, 0.0);
  int got = maxir_decode_all(code, &p, rx, clen, out, NULL, outc);

  /* Pre-burst recovers exactly (stop a look-ahead short of the burst). */
  int pre_n = 0;
  int pre_err =
      region_errors(out, got, msg, warmup, burst_lo - depth, warmup, &pre_n);
  check("re-acquire: pre-burst region clean", pre_n > 100 && pre_err == 0);

  /* The burst wrecks decoding over its span (straight against the message). */
  int burst_err = 0, burst_n = 0;
  for (int i = burst_lo + 20; i < burst_hi - 20 && i < got; ++i) {
    ++burst_n;
    if (out[i] != msg[i]) ++burst_err;
  }
  check("re-acquire: burst disrupts decoding",
        burst_n > 50 && burst_err > burst_n / 4);

  /* After the burst the decoder re-locks: a 200-bit run of the post-burst
   * message (msg[700, 900)) reappears in the output tail. */
  int reacq_ok = 0;
  for (int o = burst_hi - 10; o + 200 <= got && !reacq_ok; ++o) {
    int run_err = 0;
    for (int k = 0; k < 200; ++k) {
      if (out[o + k] != msg[700 + k]) ++run_err;
    }
    if (run_err <= 4) reacq_ok = 1;
  }
  check("re-acquire: re-locks and recovers after the burst", reacq_ok);

  free(msg);
  free(coded);
  free(rx);
  free(out);
  dt_cc_code_destroy(code);
}

int main(void) {
  printf("maxir encoder:\n");
  test_encode_length_and_chunked();
  test_encode_error_paths();
  test_encoder_vtable();
  printf("maxir decoder:\n");
  test_decoder_param_validation();
  test_decoder_roundtrip();
  test_decoder_soft_invariants();
  test_decoder_invalid_and_erasure();
  test_decoder_blind_acquisition();
  test_decoder_vtable();
  test_soft_decoder_vtable();
  printf("maxir drift:\n");
  test_decoder_reanchor();
  test_decoder_midgroup_indel();
  test_decoder_drift_plus_flip();
  test_decoder_blind_acquisition_under_drift();
  test_decoder_reacquire();
  return test_summary("maxir");
}
