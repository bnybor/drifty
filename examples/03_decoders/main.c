/*
 * 03 - The decoder line-up: all five convolutional decoders, side by side.
 *
 * drifty shares ONE encoder but offers five decoders that differ in the channel
 * damage they correct. This runs all five and shows the key split:
 *
 *   - viterbi, bcjr   handle flips and erasures on a bit-ALIGNED channel.
 *   - vindel, hybrid, maxir  additionally track DRIFT: inserted / dropped bits
 *                            that slide the stream out of alignment.
 *
 * The same message is sent twice: once over a flip+erase channel (no drift) and
 * once over an insert+delete channel (drift). Watch the aligned-channel decoders
 * recover the first and lose the second, while the drift-tolerant trio recover
 * both. Demonstrates every cc decoder and the alignment-vs-value distinction.
 *
 * Run: ./03_decoders
 */

#include "util.h"

#include <drifty/cc/bcjr.h>
#include <drifty/cc/hybrid.h>
#include <drifty/cc/maxir.h>
#include <drifty/cc/vindel.h>
#include <drifty/cc/viterbi.h>

/* Decode rx[] with an already-created decoder and report the settled error count.
 * The caller still owns `dec` (each codec has its own _destroy). */
static void run(const char *name, dt_stream_decoder *dec, const dt_bit *rx,
                int rlen, const dt_bit *msg, int N, int warmup, int cap) {
  dt_bit *out = malloc((size_t)cap);
  int got = ex_decode_hard(dec, rx, rlen, out, cap);
  int hi = got < N ? got : N;
  int err = ex_count_errors(out, msg, warmup, hi);
  printf("  %-8s recovered %4d bits, %3d errors in [%d, %d)   %s\n", name, got,
         err, warmup, N, err <= 3 ? "OK" : "(could not track this channel)");
  free(out);
}

int main(void) {
  /* A rate-1/3 code: indel tolerance grows with redundancy, so a lower-rate code
   * gives the drift-tolerant decoders more margin (the metrics docs recommend an
   * even stronger code, K5 rate-1/5, for heavy drift). */
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_3);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int depth = 6 * K;        /* output delay / look-ahead */
  const int warmup = depth + 6 * K;

  enum { N = 500 };
  uint64_t rng = 0xD15EA5Eu;
  dt_bit msg[N];
  ex_rand_bits(msg, N, &rng);

  const int cap = (N + K) * n + 256;
  dt_bit *coded = malloc((size_t)cap);
  int clen = ex_encode(code, msg, N, coded, cap);
  if (clen < 0) {
    return 1;
  }

  /* ---- Channel 1: bit-aligned. 3% flips + 5% erasures, no drift. ---- */
  printf("aligned channel  (3%% flips, 5%% erasures, NO drift):\n");
  dt_bit *rx1 = malloc((size_t)clen);
  memcpy(rx1, coded, (size_t)clen);
  ex_flip(rx1, clen, 0.03, &rng);
  ex_erase(rx1, clen, 0.05, &rng);

  dt_stream_decoder *vit = dt_cc_viterbi_decoder_create(code);
  run("viterbi", vit, rx1, clen, msg, N, warmup, cap);
  dt_cc_viterbi_decoder_destroy(vit);

  dt_cc_bcjr_stream_params bp = {.decision_depth = depth, .p_flip = 0.03f,
                                 .p_erase = 0.05f};
  dt_stream_decoder *bcjr = dt_cc_bcjr_decoder_create(code, &bp);
  run("bcjr", bcjr, rx1, clen, msg, N, warmup, cap);
  dt_cc_bcjr_decoder_destroy(bcjr);

  /* ---- Channel 2: drifting. ~1% inserts + ~1% deletes + 2% flips. ----
   * Insertion can grow the stream, so size these buffers for the worst case. */
  printf("\ndrifting channel (1%% flips, ~1%% inserts, ~1%% deletes):\n");
  const int chcap = 2 * clen + 64;
  dt_bit *tmp = malloc((size_t)chcap), *rx2 = malloc((size_t)chcap);
  int t = ex_insert(coded, clen, 0.01, &rng, tmp); /* slide right */
  int rlen = ex_delete(tmp, t, 0.01, &rng, rx2);   /* slide left  */
  ex_flip(rx2, rlen, 0.01, &rng);
  printf("  (channel length %d -> %d bits from indels)\n", clen, rlen);

  dt_cc_vindel_stream_params vp = {.decision_depth = depth, .max_drift = 8,
                                   .p_sub = 0.02f, .p_ins = 0.01f, .p_del = 0.01f};
  dt_stream_decoder *vin = dt_cc_vindel_decoder_create(code, &vp);
  run("vindel", vin, rx2, rlen, msg, N, warmup, cap);
  dt_cc_vindel_decoder_destroy(vin);

  dt_cc_hybrid_stream_params hp = {.decision_depth = depth, .max_drift = 8,
                                   .p_flip = 0.02f, .p_ins_true = 0.005f,
                                   .p_ins_false = 0.005f, .p_del = 0.01f};
  dt_stream_decoder *hyb = dt_cc_hybrid_decoder_create(code, &hp);
  run("hybrid", hyb, rx2, rlen, msg, N, warmup, cap);
  dt_cc_hybrid_decoder_destroy(hyb);

  dt_cc_maxir_stream_params mp = {.decision_depth = depth, .max_drift = 8,
                                  .p_flip = 0.02f, .p_ins_true = 0.005f,
                                  .p_ins_false = 0.005f, .p_del = 0.01f};
  dt_stream_decoder *mx = dt_cc_maxir_decoder_create(code, &mp);
  run("maxir", mx, rx2, rlen, msg, N, warmup, cap);
  dt_cc_maxir_decoder_destroy(mx);

  printf("\nThe aligned-channel decoders (viterbi, bcjr) are not built to follow\n"
         "indels; the drift-tolerant trio (vindel, hybrid, maxir) recover both.\n");

  free(coded);
  free(rx1);
  free(tmp);
  free(rx2);
  dt_cc_code_destroy(code);
  return 0;
}
