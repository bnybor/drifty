/*
 * 08 - Framing (fixed length): the naive frame codec.
 *
 * A frame codec splits a continuous stream into delimited FRAMES - protected
 * payload framed, with unprotected bits passing through between frames. It carries
 * no error correction of its own; you wrap it around an inner cc / outer bc codec.
 * The decoder reports frame boundaries through a small state machine you read after
 * every decode():
 *
 *   OUTSIDE -> (BEGIN -> INSIDE -> END)*
 *
 * The `naive` codec uses fixed-length frames of `len` symbols with no markers -
 * both sides simply agree on `len`. This encodes a few frames and walks the
 * decoder's state machine to recover each one. Demonstrates dt_frame_encoder /
 * dt_frame_decoder and the boundary state machine.
 *
 * Run: ./08_frames_naive
 */

#include "util.h"

#include <drifty/fc/naive.h>
#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>

int main(void) {
  enum { LEN = 16, NFRAMES = 3, CAP = 4096 };

  /* Build NFRAMES payloads of LEN bits each. */
  uint64_t rng = 0xF7A3E5u;
  dt_bit payload[NFRAMES][LEN];
  for (int f = 0; f < NFRAMES; ++f) {
    ex_rand_bits(payload[f], LEN, &rng);
  }

  /* ---- Encode: begin -> (begin_frame -> encode -> end_frame)* -> finalize ---- */
  dt_frame_encoder *enc = dt_fc_naive_frame_encoder_create(LEN);
  dt_bit coded[CAP];
  int clen = enc->begin(enc, coded, CAP);
  for (int f = 0; f < NFRAMES; ++f) {
    clen += enc->begin_frame(enc, coded + clen, CAP - clen);
    clen += enc->encode(enc, coded + clen, CAP - clen, payload[f], LEN);
    clen += enc->end_frame(enc, coded + clen, CAP - clen);
  }
  clen += enc->finalize(enc, coded + clen, CAP - clen);
  dt_fc_naive_frame_encoder_destroy(enc);
  printf("encoded %d frames of %d bits -> %d coded bits (naive adds no overhead)\n\n",
         NFRAMES, LEN, clen);

  /* ---- Decode. naive buffers nothing: each decode() consumes exactly the bits
   * it writes, so advance the source pointer by the return value and feed the
   * rest. Check get_state() after every call to read the boundary machine. The
   * transitions (BEGIN/END) return 0 bits; the INSIDE state copies the payload.
   * naive frames run back to back, so the output is exactly the payloads. */
  dt_frame_decoder *dec = dt_fc_naive_frame_decoder_create(LEN);
  dec->begin(dec, NULL, 0);
  dt_bit out[CAP];
  int spos = 0, dpos = 0, begins = 0, ends = 0;
  dt_frame_decoder_state prev = dec->get_state(dec);
  for (;;) {
    int n = dec->decode(dec, out + dpos, CAP - dpos, coded + spos, clen - spos);
    dt_frame_decoder_state state = dec->get_state(dec);
    spos += n;
    dpos += n;
    if (n == 0 && state == prev) {
      break; /* no progress: input exhausted, machine settled */
    }
    if (state != prev) {
      if (state == DT_FRAME_DECODER_BEGIN) {
        ++begins;
      } else if (state == DT_FRAME_DECODER_END) {
        ++ends;
      }
    }
    prev = state;
  }
  dt_fc_naive_frame_decoder_destroy(dec);

  printf("decoder saw %d frame opens (BEGIN) and %d closes (END)\n", begins, ends);

  int ok = (dpos == NFRAMES * LEN);
  for (int f = 0; f < NFRAMES && ok; ++f) {
    for (int i = 0; i < LEN; ++i) {
      ok = ok && out[f * LEN + i] == payload[f][i];
    }
  }
  printf("recovered %d payload bits (expected %d); payloads match: %s\n", dpos,
         NFRAMES * LEN, ok ? "yes" : "no");

  /* The naive codec also has a SOFT frame decoder - the reframing stage of a soft
   * pipeline (stream soft decode -> reframe -> block soft decode). Feed a soft
   * copy of the stream and recover soft records, driven the same way. */
  dt_soft_bit sin[CAP], sout[CAP];
  for (int i = 0; i < clen; ++i) {
    dt_soft_bit s = {0, 0, 0, 0, 0, 1.0f};
    if (coded[i] == DT_TRUE) {
      s.c_true = 1.0f;
    } else {
      s.c_false = 1.0f;
    }
    sin[i] = s;
  }
  dt_frame_soft_decoder *sdec = dt_fc_naive_frame_soft_decoder_create(LEN);
  sdec->begin(sdec, NULL, 0);
  int sspos = 0, sdpos = 0;
  dt_frame_decoder_state sprev = sdec->get_state(sdec);
  for (;;) {
    int m = sdec->decode(sdec, sout + sdpos, CAP - sdpos, sin + sspos,
                         clen - sspos);
    dt_frame_decoder_state sstate = sdec->get_state(sdec);
    sspos += m;
    sdpos += m;
    if (m == 0 && sstate == sprev) {
      break;
    }
    sprev = sstate;
  }
  dt_fc_naive_frame_soft_decoder_destroy(sdec);
  int sok = (sdpos == NFRAMES * LEN);
  for (int f = 0; f < NFRAMES && sok; ++f) {
    for (int i = 0; i < LEN; ++i) {
      sok = sok && ex_hard_of(sout[f * LEN + i]) == payload[f][i];
    }
  }
  printf("soft decoder: recovered %d payload bits; payloads match: %s\n", sdpos,
         sok ? "yes" : "no");
  return 0;
}
