/*
 * 04 - Soft output: per-bit consistencies instead of a hard 0/1.
 *
 * A soft decoder reports, for every recovered position, a dt_soft_bit: a graded
 * "consistency" in [0,1] for each hypothesis (true / false / erasure / invalid /
 * absent) plus c_locked (is the decoder tracking this code?). These are
 * goodness-of-fit values, NOT a probability split - they need not sum to 1. Soft
 * output keeps the value information a hard decision throws away, which is what an
 * outer code (example 10) feeds on.
 *
 * This prints the soft record for confident bits on a clean stretch, then for bits
 * inside an erasure burst, so you can see the consistencies move with reliability.
 * Demonstrates the dt_stream_soft_decoder and the dt_soft_bit fields.
 *
 * Run: ./04_soft_output
 */

#include "util.h"

#include <drifty/cc/bcjr.h>
#include <drifty/cc/hybrid.h>
#include <drifty/cc/maxir.h>

static void show(int i, const dt_soft_bit *s) {
  printf("  bit %3d:  true=%.2f false=%.2f erasure=%.2f invalid=%.2f absent=%.2f"
         "  locked=%.2f  -> %s\n",
         i, s->c_true, s->c_false, s->c_erasure, s->c_invalid, s->c_absent,
         s->c_locked, ex_sym(ex_hard_of(*s)));
}

int main(void) {
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int depth = 6 * K;

  enum { N = 300 };
  uint64_t rng = 0x50F750F7u;
  dt_bit msg[N];
  ex_rand_bits(msg, N, &rng);

  const int cap = (N + K) * n + 64;
  dt_bit *coded = malloc((size_t)cap);
  int clen = ex_encode(code, msg, N, coded, cap);
  if (clen < 0) {
    return 1;
  }

  /* Drop an erasure burst onto the coded bits for info slots [150, 168). */
  dt_bit *rx = malloc((size_t)clen);
  memcpy(rx, coded, (size_t)clen);
  ex_flip(rx, clen, 0.01, &rng); /* light background flips */
  for (int i = 150 * n; i < 168 * n && i < clen; ++i) {
    rx[i] = DT_ERASURE;
  }

  /* The model expects light flips and the erasures we injected. */
  dt_cc_hybrid_stream_params p = {.decision_depth = depth, .p_flip = 0.02f,
                                  .p_ovr_erase = 0.05f};
  dt_stream_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(code, &p);
  dt_soft_bit *out = malloc((size_t)cap * sizeof *out);
  int got = ex_decode_soft(sd, rx, clen, out, cap);
  dt_cc_hybrid_soft_decoder_destroy(sd);
  printf("decoded %d soft records from %d coded bits\n\n", got, clen);

  printf("confident bits on a clean stretch (winner ~1, loser ~0, locked ~1):\n");
  for (int i = 60; i < 65; ++i) {
    show(i, &out[i]);
  }

  printf("\nbits inside the erasure burst [150,168) - the value evidence is gone,\n"
         "so true/false both stay high (c_erasure leads) or lock dips:\n");
  for (int i = 156; i < 162; ++i) {
    show(i, &out[i]);
  }

  printf("\nThe hard symbol (-> column) is the recoverability-first projection of\n"
         "these fields. A soft outer decoder instead uses the graded values\n"
         "directly, keeping information the hard decision discards (see example 10).\n");

  /* The same dt_soft_bit interface is offered by the other two soft decoders -
   * bcjr (bit-aligned) and maxir (drift-tolerant, fullest detail). Same factory
   * shape, same fields, same drive. Decode the clean stream with each: */
  printf("\nbcjr and maxir expose the identical soft interface; settled bit 60:\n");
  dt_cc_bcjr_stream_params bp = {.decision_depth = depth, .p_flip = 0.02f};
  dt_stream_soft_decoder *bsd = dt_cc_bcjr_soft_decoder_create(code, &bp);
  ex_decode_soft(bsd, coded, clen, out, cap);
  printf("  bcjr : -> %s (locked=%.2f)\n", ex_sym(ex_hard_of(out[60])),
         out[60].c_locked);
  dt_cc_bcjr_soft_decoder_destroy(bsd);
  dt_cc_maxir_stream_params mp = {.decision_depth = depth, .p_flip = 0.02f};
  dt_stream_soft_decoder *msd = dt_cc_maxir_soft_decoder_create(code, &mp);
  ex_decode_soft(msd, coded, clen, out, cap);
  printf("  maxir: -> %s (locked=%.2f)\n", ex_sym(ex_hard_of(out[60])),
         out[60].c_locked);
  dt_cc_maxir_soft_decoder_destroy(msd);

  free(coded);
  free(rx);
  free(out);
  dt_cc_code_destroy(code);
  return 0;
}
