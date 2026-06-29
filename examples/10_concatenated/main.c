/*
 * 10 - The full stack: an rs251 outer block code over a hybrid soft inner code,
 *      across a drifting channel. This is where the layers compose.
 *
 *   message -> rs251 encode -> cc encode -> [channel] -> cc decode -> rs251 decode
 *               (outer)         (inner)                   (inner)       (outer)
 *
 * The inner hybrid SOFT decoder absorbs the drift and most flips and reports, per
 * bit, a graded consistency rather than a hard symbol. The outer rs251 soft
 * decoder consumes that, turns the positions the inner code could not place into
 * Reed-Solomon erasures (the "erasure bridge"), and corrects the rest. The HARD
 * path runs on the identical channel for contrast - on this burst pattern it
 * fails where the soft path recovers, because the hard decision discards the value
 * information the soft output keeps.
 *
 * This is the worked example from doc/concatenated.md. Demonstrates the inner ->
 * outer hand-off and why soft output matters end to end.
 *
 * Run: ./10_concatenated
 */

#include "util.h"

#include <drifty/bc/rs251.h>
#include <drifty/block_decoder.h>
#include <drifty/block_encoder.h>
#include <drifty/block_soft_decoder.h>
#include <drifty/cc/hybrid.h>
#include <drifty/result.h>

static void byte_to_bits(unsigned char v, dt_bit *out) {
  for (int i = 0; i < 8; i++) {
    out[i] = (v & (0x80u >> i)) ? DT_TRUE : DT_FALSE;
  }
}
static int bits_to_byte(const dt_bit *in, unsigned char *out) {
  unsigned v = 0;
  for (int i = 0; i < 8; i++) {
    if (!DT_IS_BIT(in[i])) {
      return 0; /* erasure / absent / invalid -> not a clean byte */
    }
    v = (v << 1) | DT_BIT(in[i]);
  }
  *out = (unsigned char)v;
  return 1;
}
/* Plain argmax projection of a soft record (this diagnostic gauges what the
 * ordinary hard rs251 decoder would see if fed the soft decoder's hard guesses;
 * the recovery itself uses the soft rs251 decoder on the soft codeword, below). */
static dt_bit hard_of(dt_soft_bit b) {
  float mx = b.c_false;
  dt_bit a = DT_FALSE;
  if (b.c_true > mx) { mx = b.c_true; a = DT_TRUE; }
  if (b.c_erasure > mx) { mx = b.c_erasure; a = DT_ERASURE; }
  if (b.c_invalid > mx) { mx = b.c_invalid; a = DT_INVALID; }
  if (b.c_absent > mx) { mx = b.c_absent; a = DT_ABSENT; }
  return a;
}
/* count erased / silently-wrong symbols of a hard codeword vs truth */
static void residue(const dt_bit *rcw, const dt_bit *cw, int N, int *pe,
                    int *perr) {
  int e = 0, err = 0;
  for (int s = 0; s < N; s++) {
    int fl = 0, wrong = 0;
    for (int i = 0; i < 8; i++) {
      if (!DT_IS_BIT(rcw[s * 8 + i])) {
        fl = 1;
      } else if (rcw[s * 8 + i] != cw[s * 8 + i]) {
        wrong = 1;
      }
    }
    if (fl) {
      e++;
    } else if (wrong) {
      err++;
    }
  }
  *pe = e;
  *perr = err;
}
static int msg_ok(const dt_bit *dec, const unsigned char *msg, int MSG) {
  for (int i = 0; i < MSG; i++) {
    unsigned char b;
    if (!bits_to_byte(dec + i * 8, &b) || b != msg[i]) {
      return 0;
    }
  }
  return 1;
}

int main(void) {
  const uint16_t N = 40, K = 24; /* RS(40,24): 16 parity symbols */
  const int MSG = K - 1;         /* 23 message bytes (GF(251) packing) */
  enum { DEPTH = 42, DRIFT = 4 };

  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  if (!code) {
    return 1;
  }
  const int ncc = dt_cc_code_n(code), kcc = dt_cc_code_k(code);

  unsigned char msg[64];
  for (int i = 0; i < MSG; i++) {
    msg[i] = (unsigned char)(0x41 + i);
  }

  /* ---- OUTER encode: rs251 -> the 8*N codeword bits are the inner payload ---- */
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(N, K);
  dt_bit *rse_in = rse->decoded_buf(rse);
  for (int i = 0; i < MSG; i++) {
    byte_to_bits(msg[i], rse_in + i * 8);
  }
  rse->encode(rse);
  dt_bit *cw = rse->encoded_buf(rse);
  const int CWBITS = 8 * N;

  /* ---- INNER encode: prepend DEPTH known warm-up bits, then the codeword ---- */
  const int WARM = DEPTH, INFO = WARM + CWBITS;
  dt_bit *info = malloc((size_t)INFO);
  for (int i = 0; i < WARM; i++) {
    info[i] = DT_FALSE;
  }
  memcpy(info + WARM, cw, (size_t)CWBITS);
  const int CAP = (INFO + kcc) * ncc + 64;
  dt_bit *coded = malloc((size_t)CAP);
  dt_stream_encoder *cce = dt_cc_encoder_create(code);
  int clen = cce->begin(cce, coded, CAP);
  clen += cce->encode(cce, coded + clen, CAP - clen, info, INFO);
  clen += cce->finalize(cce, coded + clen, CAP - clen);
  dt_cc_encoder_destroy(cce);

  /* ---- CHANNEL: two 16-bit erasure bursts + one indel pair ---- */
  dt_bit *rx = malloc((size_t)clen + 8);
  int rlen = 0, di = clen / 5, ii = 4 * clen / 5, b0 = clen / 3, b1 = 2 * clen / 3;
  for (int i = 0; i < clen; i++) {
    if (i == di) {
      continue; /* delete one coded bit */
    }
    dt_bit b = coded[i];
    if ((i >= b0 && i < b0 + 16) || (i >= b1 && i < b1 + 16)) {
      b = DT_ERASURE; /* burst: evidence gone here */
    } else if (i % 50 == 0 && DT_IS_BIT(b)) {
      b = (b == DT_TRUE) ? DT_FALSE : DT_TRUE; /* ~2% background flips */
    }
    rx[rlen++] = b;
    if (i == ii) {
      rx[rlen++] = DT_TRUE; /* insert one spurious bit */
    }
  }

  dt_cc_hybrid_stream_params hp = {
      .decision_depth = DEPTH, .max_drift = DRIFT, .p_flip = 0.03f,
      .p_ins_true = 0.004f, .p_ins_false = 0.004f, .p_del = 0.008f,
      .p_ovr_erase = 0.02f}; /* erasures can appear */
  const int OCAP = INFO + 64;

  /* ===== SOFT path: hybrid SOFT decoder -> soft rs251 ===== */
  dt_stream_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(code, &hp);
  dt_soft_bit *sout = malloc((size_t)OCAP * sizeof *sout);
  sd->begin(sd, NULL, 0);
  int sn = sd->decode(sd, sout, OCAP, rx, rlen);
  for (;;) {
    int g = sd->decode(sd, sout + sn, OCAP - sn, NULL, 0);
    if (g <= 0) {
      break;
    }
    sn += g;
  }
  sn += sd->finalize(sd, sout + sn, OCAP - sn); /* drain + flush the tail */
  dt_cc_hybrid_soft_decoder_destroy(sd);

  dt_soft_bit *srcw = sout + WARM; /* skip the warm-up prefix */
  dt_bit sproj[4096];              /* argmax, to gauge residue */
  for (int i = 0; i < CWBITS; i++) {
    sproj[i] = hard_of(srcw[i]);
  }
  int se, serr;
  residue(sproj, cw, N, &se, &serr);

  dt_block_soft_decoder *srs =
      dt_bc_rs251_block_soft_decoder_create(N, K, /*s=*/0);
  memcpy(srs->encoded_buf(srs), srcw, (size_t)CWBITS * sizeof(dt_soft_bit));
  int sok = (srs->decode(srs) == DT_OK) && msg_ok(srs->decoded_buf(srs), msg, MSG);

  printf("SOFT  (hybrid soft -> soft rs251):\n");
  printf("  inner argmax residue: %d erased + %d silently-wrong of %d symbols\n",
         se, serr, N);
  printf("  message recovered exactly: %s\n\n", sok ? "YES" : "NO");

  /* ===== HARD path on the SAME channel, for contrast ===== */
  dt_stream_decoder *hd = dt_cc_hybrid_decoder_create(code, &hp);
  dt_bit *hout = malloc((size_t)OCAP);
  hd->begin(hd, NULL, 0);
  int hn = hd->decode(hd, hout, OCAP, rx, rlen);
  for (;;) {
    int g = hd->decode(hd, hout + hn, OCAP - hn, NULL, 0);
    if (g <= 0) {
      break;
    }
    hn += g;
  }
  hn += hd->finalize(hd, hout + hn, OCAP - hn);
  dt_cc_hybrid_decoder_destroy(hd);

  dt_bit *hrcw = hout + WARM;
  int he, herr;
  residue(hrcw, cw, N, &he, &herr);
  dt_block_decoder *hrs = dt_bc_rs251_block_decoder_create(N, K, /*s=*/0);
  memcpy(hrs->encoded_buf(hrs), hrcw, (size_t)CWBITS);
  int hok = (hrs->decode(hrs) == DT_OK) && msg_ok(hrs->decoded_buf(hrs), msg, MSG);

  printf("HARD  (hybrid hard -> hard rs251), same channel:\n");
  printf("  inner residue: %d erased + %d silently-wrong; 2*err+eras = %d, RS "
         "budget n-k = %d\n",
         he, herr, 2 * herr + he, N - K);
  printf("  message recovered exactly: %s\n", hok ? "YES" : "NO");

  dt_bc_rs251_block_soft_decoder_destroy(srs);
  dt_bc_rs251_block_decoder_destroy(hrs);
  dt_bc_rs251_block_encoder_destroy(rse);
  dt_cc_code_destroy(code);
  free(info);
  free(coded);
  free(rx);
  free(sout);
  free(hout);
  return sok ? 0 : 3;
}
