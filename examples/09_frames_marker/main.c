/*
 * 09 - Framing (variable length): the marker frame codec, hard and soft.
 *
 * The `marker` codec delimits VARIABLE-length frames with 18-bit escape sequences
 * instead of an agreed length. It keeps the stream transparent: if the payload
 * happens to contain a bit pattern that looks like a marker, the encoder escapes
 * it, so ANY payload survives. Frames can be any size and need no length field.
 *
 * Part A (hard): encode three differently-sized frames - one carrying an
 *   adversarial run that mimics a marker - and recover them via the get_state()
 *   boundary machine, proving transparency.
 * Part B (soft): the same stream as dt_soft_bit, through the soft frame decoder
 *   (the reframing middle stage of a soft pipeline), recovering the soft payload.
 *
 * Demonstrates dt_frame_*soft*_decoder, variable-length frames, and transparency.
 *
 * Run: ./09_frames_marker
 */

#include "util.h"

#include <drifty/fc/marker.h>
#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>

enum { CAP = 8192 };

/* Drive the hard decoder, returning the bits emitted while INSIDE a frame. */
static int inside_hard(const dt_bit *coded, int clen, dt_bit *inside) {
  dt_frame_decoder *dec = dt_fc_marker_frame_decoder_create();
  dec->begin(dec, NULL, 0);
  dt_bit out[CAP];
  dt_frame_decoder_state st_of[CAP];
  int dpos = 0, fed = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int before = dpos;
    int n = dec->decode(dec, out + dpos, CAP - dpos, fed ? NULL : coded,
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
  dpos += dec->finalize(dec, out + dpos, CAP - dpos);
  dt_fc_marker_frame_decoder_destroy(dec);
  int k = 0;
  for (int i = 0; i < dpos; ++i) {
    if (st_of[i] == DT_FRAME_DECODER_INSIDE) {
      inside[k++] = out[i];
    }
  }
  return k;
}

/* Drive the SOFT decoder over a soft copy of the stream; project inside records. */
static int inside_soft(const dt_bit *coded, int clen, dt_bit *inside) {
  dt_soft_bit sin[CAP];
  for (int i = 0; i < clen; ++i) {
    dt_soft_bit s = {0, 0, 0, 0, 0, 1.0f};
    if (coded[i] == DT_TRUE) {
      s.c_true = 1.0f;
    } else {
      s.c_false = 1.0f;
    }
    sin[i] = s;
  }
  dt_frame_soft_decoder *dec = dt_fc_marker_frame_soft_decoder_create();
  dec->begin(dec, NULL, 0);
  dt_soft_bit out[CAP];
  dt_frame_decoder_state st_of[CAP];
  int dpos = 0, fed = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int before = dpos;
    int n = dec->decode(dec, out + dpos, CAP - dpos, fed ? NULL : sin,
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
  dpos += dec->finalize(dec, out + dpos, CAP - dpos);
  dt_fc_marker_frame_soft_decoder_destroy(dec);
  int k = 0;
  for (int i = 0; i < dpos; ++i) {
    if (st_of[i] == DT_FRAME_DECODER_INSIDE) {
      inside[k++] = ex_hard_of(out[i]); /* project soft -> hard for comparison */
    }
  }
  return k;
}

int main(void) {
  /* Three variable-length frames. Frame 1 carries a run of 20 ones - a pattern
   * that looks like the start of a marker - to exercise transparency. */
  uint64_t rng = 0x3A6Eu;
  dt_bit f0[10], f1[40], f2[7];
  int len[3] = {10, 40, 7};
  dt_bit *frame[3] = {f0, f1, f2};
  ex_rand_bits(f0, 10, &rng);
  ex_rand_bits(f2, 7, &rng);
  for (int i = 0; i < 40; ++i) {
    f1[i] = (i >= 10 && i < 30) ? DT_TRUE : (ex_rng_next(&rng) & 1u ? DT_TRUE
                                                                    : DT_FALSE);
  }

  /* Encode the three frames. */
  dt_frame_encoder *enc = dt_fc_marker_frame_encoder_create();
  dt_bit coded[CAP];
  int clen = enc->begin(enc, coded, CAP);
  for (int f = 0; f < 3; ++f) {
    clen += enc->begin_frame(enc, coded + clen, CAP - clen);
    clen += enc->encode(enc, coded + clen, CAP - clen, frame[f], len[f]);
    clen += enc->end_frame(enc, coded + clen, CAP - clen);
  }
  clen += enc->finalize(enc, coded + clen, CAP - clen);
  dt_fc_marker_frame_encoder_destroy(enc);
  printf("encoded frames of %d + %d + %d = %d payload bits -> %d coded bits\n",
         len[0], len[1], len[2], len[0] + len[1] + len[2], clen);
  printf("(the overhead is the begin/end markers plus escaping of the 1-run)\n\n");

  const int total = len[0] + len[1] + len[2];

  dt_bit inside[CAP];
  int kh = inside_hard(coded, clen, inside);
  int okh = (kh == total) && memcmp(inside, f0, 10) == 0 &&
            memcmp(inside + 10, f1, 40) == 0 && memcmp(inside + 50, f2, 7) == 0;
  printf("hard decoder: recovered %d in-frame bits; payloads (incl. 1-run) "
         "transparent: %s\n",
         kh, okh ? "yes" : "no");

  int ks = inside_soft(coded, clen, inside);
  int oks = (ks == total) && memcmp(inside, f0, 10) == 0 &&
            memcmp(inside + 10, f1, 40) == 0 && memcmp(inside + 50, f2, 7) == 0;
  printf("soft decoder: recovered %d in-frame bits; payloads transparent: %s\n",
         ks, oks ? "yes" : "no");
  return 0;
}
