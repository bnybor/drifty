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
 * Round-trip tests for the naive frame codec. The encoder is a pass-through and
 * frames are fixed length (`len` symbols) with no markers, so the encoded stream
 * equals the message bits; the value of the codec is the framing it reconstructs
 * on the decode side, reported through get_state():
 *
 *   OUTSIDE -> (BEGIN -> INSIDE -> END)*
 *
 * These tests check both that the payload survives the round trip and that the
 * observed state sequence is exactly one (BEGIN, INSIDE, END) per frame.
 */

#include <drifty/bit.h>
#include <drifty/fc/naive.h>
#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/soft_bit.h>

#include <stdint.h>
#include <stdio.h>

#define LEN 8     /* symbols per frame */
#define NFRAMES 5 /* frames in the test message */
#define MSGLEN (LEN * NFRAMES)
#define CAP 256 /* generous output buffers */

static int g_failures = 0;

static int check(const char *name, int cond) {
  printf("  [%s] %s\n", name, cond ? "PASS" : "FAIL");
  if (!cond) {
    ++g_failures;
  }
  return cond;
}

/* A small deterministic PRNG so runs are reproducible. */
static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
static int rand_bit(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (int)((g_rng >> 11) & 1u);
}

/* Drive the encoder over a message split into NFRAMES frames of LEN bits each;
 * the coded stream lands in `coded`. Returns the coded length. */
static int encode_message(const dt_bit *msg, dt_bit *coded) {
  dt_frame_encoder *enc = dt_fc_naive_frame_encoder_create(LEN);
  if (!enc) {
    return -1;
  }
  int n = 0;
  n += enc->begin(enc, coded + n, CAP - n);
  for (int f = 0; f < NFRAMES; ++f) {
    n += enc->begin_frame(enc, coded + n, CAP - n);
    n += enc->encode(enc, coded + n, CAP - n, msg + f * LEN, LEN);
    n += enc->end_frame(enc, coded + n, CAP - n);
  }
  n += enc->finalize(enc, coded + n, CAP - n);
  dt_fc_naive_frame_encoder_destroy(enc);
  return n;
}

/* -- hard round trip ------------------------------------------------------- */

static void test_hard_round_trip(void) {
  printf("naive hard round trip:\n");

  dt_bit msg[MSGLEN];
  for (int i = 0; i < MSGLEN; ++i) {
    msg[i] = rand_bit() ? DT_TRUE : DT_FALSE;
  }

  dt_bit coded[CAP];
  int clen = encode_message(msg, coded);
  check("encode is a pass-through (length unchanged)", clen == MSGLEN);
  int coded_ok = 1;
  for (int i = 0; i < MSGLEN; ++i) {
    coded_ok = coded_ok && coded[i] == msg[i];
  }
  check("encode copies bits verbatim", coded_ok);

  dt_frame_decoder *dec = dt_fc_naive_frame_decoder_create(LEN);
  if (!check("decoder create", dec != NULL)) {
    return;
  }
  check("begin consumes no preamble", dec->begin(dec, NULL, 0) == 0);
  check("starts OUTSIDE", dec->get_state(dec) == DT_FRAME_DECODER_OUTSIDE);

  dt_bit out[CAP];
  dt_frame_decoder_state seq[64];
  int nseq = 0, spos = 0, dpos = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int n = dec->decode(dec, out + dpos, CAP - dpos, coded + spos, clen - spos);
    dt_frame_decoder_state state = dec->get_state(dec);
    spos += n; /* pass-through: bits consumed == bits written */
    dpos += n;
    if (n == 0 && state == prev) {
      break; /* no progress: out of input, machine settled */
    }
    if (nseq < (int)(sizeof seq / sizeof *seq)) {
      seq[nseq++] = state;
    }
    prev = state;
  }

  check("recovered length matches", dpos == MSGLEN);
  int payload_ok = 1;
  for (int i = 0; i < MSGLEN; ++i) {
    payload_ok = payload_ok && out[i] == msg[i];
  }
  check("payload recovered intact", payload_ok);

  /* The state walk must be exactly (BEGIN, INSIDE, END) per frame, ending END. */
  int seq_ok = (nseq == 3 * NFRAMES);
  for (int f = 0; f < NFRAMES && seq_ok; ++f) {
    seq_ok = seq_ok && seq[f * 3 + 0] == DT_FRAME_DECODER_BEGIN &&
             seq[f * 3 + 1] == DT_FRAME_DECODER_INSIDE &&
             seq[f * 3 + 2] == DT_FRAME_DECODER_END;
  }
  check("state sequence is (BEGIN INSIDE END) per frame", seq_ok);
  check("settles at END", dec->get_state(dec) == DT_FRAME_DECODER_END);

  dt_fc_naive_frame_decoder_destroy(dec);
}

/* -- partial dst (decode stops mid-frame, stays INSIDE) -------------------- */

static void test_partial_dst(void) {
  printf("naive partial-dst decode:\n");

  dt_bit msg[MSGLEN];
  for (int i = 0; i < MSGLEN; ++i) {
    msg[i] = rand_bit() ? DT_TRUE : DT_FALSE;
  }
  dt_bit coded[CAP];
  int clen = encode_message(msg, coded);

  dt_frame_decoder *dec = dt_fc_naive_frame_decoder_create(LEN);
  if (!check("decoder create", dec != NULL)) {
    return;
  }
  dec->begin(dec, NULL, 0);

  /* Offer at most 3 bits of dst per call: a frame's LEN payload must then be
   * delivered over several INSIDE calls, the decoder staying INSIDE between
   * them, before it reaches END. The payload must still come out intact. */
  dt_bit out[CAP];
  int spos = 0, dpos = 0;
  int saw_partial_inside = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    size_t room = CAP - dpos;
    if (room > 3) {
      room = 3;
    }
    int n = dec->decode(dec, out + dpos, room, coded + spos, clen - spos);
    dt_frame_decoder_state state = dec->get_state(dec);
    if (n > 0 && state == DT_FRAME_DECODER_INSIDE) {
      saw_partial_inside = 1; /* payload bits delivered while staying INSIDE */
    }
    spos += n;
    dpos += n;
    if (n == 0 && state == prev) {
      break;
    }
    prev = state;
  }

  check("payload split across INSIDE calls", saw_partial_inside);
  check("recovered length matches", dpos == MSGLEN);
  int payload_ok = 1;
  for (int i = 0; i < MSGLEN; ++i) {
    payload_ok = payload_ok && out[i] == msg[i];
  }
  check("payload recovered intact under small dst", payload_ok);

  dt_fc_naive_frame_decoder_destroy(dec);
}

/* -- soft round trip ------------------------------------------------------- */

static void test_soft_round_trip(void) {
  printf("naive soft round trip:\n");

  /* Build a soft stream: unit mass on each bit's value, distinct per position so
   * a copy is verifiable. */
  dt_soft_bit in[MSGLEN];
  for (int i = 0; i < MSGLEN; ++i) {
    int b = rand_bit();
    dt_soft_bit s = {0};
    s.c_true = b ? 1.0f : 0.0f;
    s.c_false = b ? 0.0f : 1.0f;
    s.c_locked = (float)(i + 1); /* a per-position tag to catch misplacement */
    in[i] = s;
  }

  dt_frame_soft_decoder *dec = dt_fc_naive_frame_soft_decoder_create(LEN);
  if (!check("soft decoder create", dec != NULL)) {
    return;
  }
  check("begin consumes no preamble", dec->begin(dec, NULL, 0) == 0);
  check("starts OUTSIDE", dec->get_state(dec) == DT_FRAME_DECODER_OUTSIDE);

  dt_soft_bit out[CAP];
  dt_frame_decoder_state seq[64];
  int nseq = 0, spos = 0, dpos = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int n = dec->decode(dec, out + dpos, CAP - dpos, in + spos, MSGLEN - spos);
    dt_frame_decoder_state state = dec->get_state(dec);
    spos += n;
    dpos += n;
    if (n == 0 && state == prev) {
      break;
    }
    if (nseq < (int)(sizeof seq / sizeof *seq)) {
      seq[nseq++] = state;
    }
    prev = state;
  }

  check("recovered length matches", dpos == MSGLEN);
  int payload_ok = 1;
  for (int i = 0; i < MSGLEN; ++i) {
    payload_ok = payload_ok && out[i].c_true == in[i].c_true &&
                 out[i].c_false == in[i].c_false &&
                 out[i].c_locked == in[i].c_locked;
  }
  check("soft records pass through unchanged", payload_ok);

  int seq_ok = (nseq == 3 * NFRAMES);
  for (int f = 0; f < NFRAMES && seq_ok; ++f) {
    seq_ok = seq_ok && seq[f * 3 + 0] == DT_FRAME_DECODER_BEGIN &&
             seq[f * 3 + 1] == DT_FRAME_DECODER_INSIDE &&
             seq[f * 3 + 2] == DT_FRAME_DECODER_END;
  }
  check("state sequence is (BEGIN INSIDE END) per frame", seq_ok);

  dt_fc_naive_frame_soft_decoder_destroy(dec);
}

/* -- lifecycle ------------------------------------------------------------- */

static void test_lifecycle(void) {
  printf("naive lifecycle:\n");

  dt_frame_encoder *enc = dt_fc_naive_frame_encoder_create(LEN);
  dt_frame_decoder *dec = dt_fc_naive_frame_decoder_create(LEN);
  dt_frame_soft_decoder *sdec = dt_fc_naive_frame_soft_decoder_create(LEN);
  check("encoder create", enc != NULL);
  check("decoder create", dec != NULL);
  check("soft decoder create", sdec != NULL);
  dt_fc_naive_frame_encoder_destroy(enc);
  dt_fc_naive_frame_decoder_destroy(dec);
  dt_fc_naive_frame_soft_decoder_destroy(sdec);

  /* destroy(NULL) must be a no-op. */
  dt_fc_naive_frame_encoder_destroy(NULL);
  dt_fc_naive_frame_decoder_destroy(NULL);
  dt_fc_naive_frame_soft_decoder_destroy(NULL);
  check("destroy(NULL) is safe", 1);
}

int main(void) {
  test_hard_round_trip();
  test_partial_dst();
  test_soft_round_trip();
  test_lifecycle();
  printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
