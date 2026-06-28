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
 * Round-trip and transparency tests for the marker frame codec. The codec
 * delimits variable-length frames with 18-bit escape sequences and escapes any
 * payload run that would form one, so the tests check (a) the payload survives
 * intact, including adversarial runs and embedded escape sequences, and (b) the
 * decoder reports the frame boundaries through get_state().
 */

#include <drifty/bit.h>
#include <drifty/fc/marker.h>
#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/soft_bit.h>

#include <stdint.h>
#include <stdio.h>

#define CAP 16384

static int g_failures = 0;

static int check(const char *name, int cond) {
  printf("  [%s] %s\n", name, cond ? "PASS" : "FAIL");
  if (!cond) {
    ++g_failures;
  }
  return cond;
}

static uint64_t g_rng = 0x123456789abcdefULL;
static int rand_bit(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (int)((g_rng >> 11) & 1u);
}

/* -- encode/decode drivers ------------------------------------------------- */

/* Drive the encoder: each frame is opened from outside and closed. If
 * back_to_back, frames are chained (begin_frame while inside) with a single
 * end_frame at the end. Returns coded length. */
static int enc_drive(const dt_bit *const *payloads, const int *lens, int nframes,
                     int back_to_back, dt_bit *coded) {
  dt_frame_encoder *enc = dt_fc_marker_frame_encoder_create();
  int total = 0;
  total += enc->begin(enc, coded, CAP);
  for (int f = 0; f < nframes; ++f) {
    total += enc->begin_frame(enc, coded + total, CAP - total);
    total += enc->encode(enc, coded + total, CAP - total, payloads[f], lens[f]);
    if (!back_to_back) {
      total += enc->end_frame(enc, coded + total, CAP - total);
    }
  }
  if (back_to_back) {
    total += enc->end_frame(enc, coded + total, CAP - total);
  }
  total += enc->finalize(enc, coded + total, CAP - total);
  dt_fc_marker_frame_encoder_destroy(enc);
  return total;
}

/* Drive the hard decoder. Fills out[] (recovered bits) and st_of[] (the frame
 * state each bit was emitted under); counts BEGIN/END transitions into cnt[2].
 * Returns recovered length. */
static int dec_drive(const dt_bit *coded, int clen, dt_bit *out,
                     dt_frame_decoder_state *st_of, int *cnt) {
  dt_frame_decoder *dec = dt_fc_marker_frame_decoder_create();
  dec->begin(dec, NULL, 0);
  cnt[0] = cnt[1] = 0;
  int dpos = 0, fed = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int before = dpos;
    int n = dec->decode(dec, out + dpos, CAP - dpos, fed ? NULL : coded,
                        fed ? 0u : (size_t)clen);
    fed = 1;
    dt_frame_decoder_state state = dec->get_state(dec);
    for (int i = before; i < before + n; ++i) {
      st_of[i] = prev; /* data drained this call was emitted under prev state */
    }
    dpos += n;
    if (state != prev) {
      if (state == DT_FRAME_DECODER_BEGIN) {
        cnt[0]++;
      } else if (state == DT_FRAME_DECODER_END) {
        cnt[1]++;
      }
    }
    if (n == 0 && state == prev) {
      break;
    }
    prev = state;
  }
  int n = dec->finalize(dec, out + dpos, CAP - dpos);
  for (int i = dpos; i < dpos + n; ++i) {
    st_of[i] = prev;
  }
  dpos += n;
  dt_fc_marker_frame_decoder_destroy(dec);
  return dpos;
}

/* Collect just the bits emitted while INSIDE a frame. Returns their count. */
static int inside_bits(const dt_bit *out, const dt_frame_decoder_state *st_of,
                       int n, dt_bit *inside) {
  int k = 0;
  for (int i = 0; i < n; ++i) {
    if (st_of[i] == DT_FRAME_DECODER_INSIDE) {
      inside[k++] = out[i];
    }
  }
  return k;
}

/* Encode a single frame holding `payload`, decode, and verify the recovered
 * inside-frame bits equal the payload exactly. */
static int single_frame_ok(const dt_bit *payload, int len) {
  static dt_bit coded[CAP], out[CAP], inside[CAP];
  static dt_frame_decoder_state st_of[CAP];
  const dt_bit *ps[1] = {payload};
  int lens[1] = {len};
  int cnt[2];
  int clen = enc_drive(ps, lens, 1, 0, coded);
  int n = dec_drive(coded, clen, out, st_of, cnt);
  int ni = inside_bits(out, st_of, n, inside);
  if (ni != len || cnt[0] != 1 || cnt[1] != 1) {
    return 0;
  }
  for (int i = 0; i < len; ++i) {
    if (inside[i] != payload[i]) {
      return 0;
    }
  }
  return 1;
}

static void seq_bits(dt_bit *buf, int code) { /* 18-bit sequence 1^15 + code */
  for (int i = 0; i < 15; ++i) {
    buf[i] = DT_TRUE;
  }
  buf[15] = (code & 4) ? DT_TRUE : DT_FALSE;
  buf[16] = (code & 2) ? DT_TRUE : DT_FALSE;
  buf[17] = (code & 1) ? DT_TRUE : DT_FALSE;
}

/* -- tests ----------------------------------------------------------------- */

static void test_basic_round_trip(void) {
  printf("marker basic round trip:\n");
  enum { NF = 4, FLEN = 30 };
  static dt_bit pl[NF][FLEN];
  const dt_bit *ps[NF];
  int lens[NF];
  for (int f = 0; f < NF; ++f) {
    for (int i = 0; i < FLEN; ++i) {
      pl[f][i] = rand_bit() ? DT_TRUE : DT_FALSE;
    }
    ps[f] = pl[f];
    lens[f] = FLEN;
  }
  static dt_bit coded[CAP], out[CAP], inside[CAP];
  static dt_frame_decoder_state st_of[CAP];
  int cnt[2];
  int clen = enc_drive(ps, lens, NF, 0, coded);
  int n = dec_drive(coded, clen, out, st_of, cnt);
  int ni = inside_bits(out, st_of, n, inside);

  check("BEGIN seen once per frame", cnt[0] == NF);
  check("END seen once per frame", cnt[1] == NF);
  check("inside-bit count matches payload", ni == NF * FLEN);
  check("no spurious out-of-frame data", n == ni);
  int ok = (ni == NF * FLEN);
  for (int f = 0; f < NF && ok; ++f) {
    for (int i = 0; i < FLEN && ok; ++i) {
      ok = inside[f * FLEN + i] == pl[f][i];
    }
  }
  check("every frame's payload recovered intact", ok);
}

static void test_transparency_sequences(void) {
  printf("marker transparency (embedded escape sequences):\n");
  const char *names[4] = {"pure 000", "begin-out 001", "end 010",
                          "begin-in 011"};
  int codes[4] = {0, 1, 2, 3};
  for (int c = 0; c < 4; ++c) {
    static dt_bit pl[64];
    int len = 0;
    for (int i = 0; i < 7; ++i) {
      pl[len++] = rand_bit() ? DT_TRUE : DT_FALSE;
    }
    seq_bits(pl + len, codes[c]);
    len += 18;
    for (int i = 0; i < 9; ++i) {
      pl[len++] = rand_bit() ? DT_TRUE : DT_FALSE;
    }
    check(names[c], single_frame_ok(pl, len));
  }
  /* two sequences back to back, then more data */
  static dt_bit pl[64];
  int len = 0;
  seq_bits(pl + len, 2);
  len += 18;
  seq_bits(pl + len, 1);
  len += 18;
  pl[len++] = DT_FALSE;
  pl[len++] = DT_TRUE;
  check("two sequences back to back", single_frame_ok(pl, len));
}

static void test_runs(void) {
  printf("marker long one-runs:\n");
  static dt_bit pl[128];
  for (int len = 14; len <= 64; len += 1) {
    for (int i = 0; i < len; ++i) {
      pl[i] = DT_TRUE; /* all ones */
    }
    if (!single_frame_ok(pl, len)) {
      char buf[32];
      snprintf(buf, sizeof buf, "all-ones len=%d", len);
      check(buf, 0);
      return;
    }
  }
  check("all-ones runs 14..64 round-trip", 1);

  /* run of ones then a zero (a sequence prefix mid-payload) at many lengths */
  for (int ones = 14; ones <= 40; ++ones) {
    int len = 0;
    for (int i = 0; i < ones; ++i) {
      pl[len++] = DT_TRUE;
    }
    pl[len++] = DT_FALSE;
    pl[len++] = DT_TRUE;
    pl[len++] = DT_FALSE;
    if (!single_frame_ok(pl, len)) {
      check("ones-run then zero", 0);
      return;
    }
  }
  check("ones-run then zero round-trip", 1);
}

static void test_frame_boundary_corner(void) {
  printf("marker frame-boundary corner (payload ends on 1^15 0):\n");
  static dt_bit pl[64];
  /* payload ends exactly on a bare sequence prefix: 1^15 0 */
  for (int lead = 0; lead <= 3; ++lead) {
    int len = 0;
    for (int i = 0; i < lead; ++i) {
      pl[len++] = rand_bit() ? DT_TRUE : DT_FALSE;
    }
    for (int i = 0; i < 15; ++i) {
      pl[len++] = DT_TRUE;
    }
    pl[len++] = DT_FALSE; /* the lone data zero, then the frame ends */
    if (!single_frame_ok(pl, len)) {
      check("ends on 1^15 0", 0);
      return;
    }
  }
  check("ends on 1^15 0 (lead 0..3)", 1);

  /* and ending on 1^15 0 b (17-bit prefix) */
  int len = 0;
  for (int i = 0; i < 15; ++i) {
    pl[len++] = DT_TRUE;
  }
  pl[len++] = DT_FALSE;
  pl[len++] = DT_TRUE;
  check("ends on 1^15 01", single_frame_ok(pl, len));
}

static void test_back_to_back(void) {
  printf("marker back-to-back frames (begin-from-inside):\n");
  enum { NF = 3, FLEN = 12 };
  static dt_bit pl[NF][FLEN];
  const dt_bit *ps[NF];
  int lens[NF];
  for (int f = 0; f < NF; ++f) {
    for (int i = 0; i < FLEN; ++i) {
      pl[f][i] = rand_bit() ? DT_TRUE : DT_FALSE;
    }
    ps[f] = pl[f];
    lens[f] = FLEN;
  }
  static dt_bit coded[CAP], out[CAP], inside[CAP];
  static dt_frame_decoder_state st_of[CAP];
  int cnt[2];
  int clen = enc_drive(ps, lens, NF, 1, coded);
  int n = dec_drive(coded, clen, out, st_of, cnt);
  int ni = inside_bits(out, st_of, n, inside);
  check("BEGIN seen once per frame", cnt[0] == NF);
  check("END seen once per frame", cnt[1] == NF);
  int ok = (ni == NF * FLEN);
  for (int i = 0; i < NF * FLEN && ok; ++i) {
    ok = inside[i] == pl[i / FLEN][i % FLEN];
  }
  check("all payloads recovered across chained frames", ok);
}

static void test_soft_round_trip(void) {
  printf("marker soft round trip:\n");
  /* random payload that avoids 15-one runs, so output is pure pass-through and
   * graded soft records must survive exactly. */
  enum { LEN = 40 };
  static dt_bit pl[LEN];
  int run = 0;
  for (int i = 0; i < LEN; ++i) {
    int b = rand_bit();
    if (b) {
      if (++run >= 12) {
        b = 0;
        run = 0;
      }
    } else {
      run = 0;
    }
    pl[i] = b ? DT_TRUE : DT_FALSE;
  }
  /* encode hard, map coded bits to graded soft records (distinct per position) */
  dt_frame_encoder *enc = dt_fc_marker_frame_encoder_create();
  static dt_bit coded[CAP];
  int clen = 0;
  clen += enc->begin(enc, coded, CAP);
  clen += enc->begin_frame(enc, coded + clen, CAP - clen);
  clen += enc->encode(enc, coded + clen, CAP - clen, pl, LEN);
  clen += enc->end_frame(enc, coded + clen, CAP - clen);
  clen += enc->finalize(enc, coded + clen, CAP - clen);
  dt_fc_marker_frame_encoder_destroy(enc);

  static dt_soft_bit sin[CAP];
  for (int i = 0; i < clen; ++i) {
    dt_soft_bit s = {0, 0, 0, 0, 0, 0};
    if (coded[i] == DT_TRUE) {
      s.c_true = 1.0f;
    } else {
      s.c_false = 1.0f;
    }
    s.c_locked = (float)(i + 1); /* per-position tag to catch misplacement */
    sin[i] = s;
  }

  dt_frame_soft_decoder *dec = dt_fc_marker_frame_soft_decoder_create();
  dec->begin(dec, NULL, 0);
  static dt_soft_bit sout[CAP];
  static dt_frame_decoder_state st_of[CAP];
  int dpos = 0, fed = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int before = dpos;
    int n = dec->decode(dec, sout + dpos, CAP - dpos, fed ? NULL : sin,
                        fed ? 0u : (size_t)clen);
    fed = 1;
    dt_frame_decoder_state state = dec->get_state(dec);
    for (int i = before; i < before + n; ++i) {
      st_of[i] = prev;
    }
    dpos += n;
    if (n == 0 && state == prev) {
      break;
    }
    prev = state;
  }
  dpos += dec->finalize(dec, sout + dpos, CAP - dpos);
  dt_fc_marker_frame_soft_decoder_destroy(dec);

  /* inside soft records must match the original coded payload soft records */
  int k = 0, ok = 1;
  for (int i = 0; i < dpos && ok; ++i) {
    if (st_of[i] == DT_FRAME_DECODER_INSIDE) {
      ok = sout[i].c_true == (pl[k] == DT_TRUE ? 1.0f : 0.0f) &&
           sout[i].c_false == (pl[k] == DT_TRUE ? 0.0f : 1.0f);
      k++;
    }
  }
  check("inside soft-bit count matches payload", k == LEN);
  check("soft records recovered with correct values", ok && k == LEN);
}

static void test_fuzz(void) {
  printf("marker fuzz (random payloads, biased toward 1-runs):\n");
  static dt_bit pl[512];
  int fails = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    int len = 1 + (int)(g_rng % 200);
    for (int i = 0; i < len; ++i) {
      /* bias toward 1s so 15-runs and embedded sequences arise often */
      pl[i] = ((g_rng = g_rng * 6364136223846793005ULL + 1) >> 33) % 4 == 0
                  ? DT_FALSE
                  : DT_TRUE;
    }
    if (!single_frame_ok(pl, len)) {
      fails++;
    }
  }
  check("2000 random one-biased payloads round-trip", fails == 0);
}

static void test_lifecycle(void) {
  printf("marker lifecycle:\n");
  dt_frame_encoder *enc = dt_fc_marker_frame_encoder_create();
  dt_frame_decoder *dec = dt_fc_marker_frame_decoder_create();
  dt_frame_soft_decoder *sdec = dt_fc_marker_frame_soft_decoder_create();
  check("encoder create", enc != NULL);
  check("decoder create", dec != NULL);
  check("soft decoder create", sdec != NULL);
  dt_fc_marker_frame_encoder_destroy(enc);
  dt_fc_marker_frame_decoder_destroy(dec);
  dt_fc_marker_frame_soft_decoder_destroy(sdec);
  dt_fc_marker_frame_encoder_destroy(NULL);
  dt_fc_marker_frame_decoder_destroy(NULL);
  dt_fc_marker_frame_soft_decoder_destroy(NULL);
  check("destroy(NULL) is safe", 1);
}

int main(void) {
  test_basic_round_trip();
  test_transparency_sequences();
  test_runs();
  test_frame_boundary_corner();
  test_back_to_back();
  test_soft_round_trip();
  test_fuzz();
  test_lifecycle();
  printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
