/*
 * 06 - Acquisition, lock, and DT_ABSENT: the decoder knows when it is lost.
 *
 * drifty's lock-tracking decoders (bcjr, vindel, hybrid, maxir) acquire sync
 * BLINDLY - they lock on whether you start at the head of a stream or tap into one
 * mid-flight - and they report, per bit, how confident they are via c_locked. When
 * they are not tracking a valid stream of the code, they emit DT_ABSENT instead of
 * guessing a value.
 *
 * Part A: decode a normal stream and watch c_locked climb from ~0 (acquiring) to
 *         ~1 (locked) over the warm-up - that ramp IS blind acquisition.
 * Part B: feed a stream encoded with a DIFFERENT code. The decoder never locks
 *         (c_locked stays low) and reports DT_ABSENT - it refuses to emit
 *         confident garbage.
 *
 * Demonstrates blind acquisition, the lock signal, and DT_ABSENT.
 *
 * Run: ./06_acquisition
 */

#include "util.h"

#include <drifty/cc/hybrid.h>

int main(void) {
  /* A rate-1/5 K5 code has a sharp, code-specific lock, which makes part B vivid. */
  dt_cc_code *own = dt_cc_code_create_standard(DT_CC_CODE_K5_RATE_1_5);
  dt_cc_code *sibling = dt_cc_code_create_standard(DT_CC_CODE_K5_RATE_1_5_ALT1);
  if (!own || !sibling) {
    return 1;
  }
  const int n = dt_cc_code_n(own), K = dt_cc_code_k(own);
  const int depth = 6 * K;

  enum { N = 300 };
  uint64_t rng = 0xACC0DEu;
  dt_bit msg[N];
  ex_rand_bits(msg, N, &rng);

  const int cap = (N + K) * n + 64;
  dt_bit *coded = malloc((size_t)cap);
  dt_soft_bit *out = malloc((size_t)cap * sizeof *out);
  dt_cc_hybrid_stream_params p = {.decision_depth = depth, .p_flip = 0.02f};

  /* ---- Part A: blind acquisition on our OWN stream ---- */
  int clen = ex_encode(own, msg, N, coded, cap);
  ex_flip(coded, clen, 0.01, &rng);
  dt_stream_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(own, &p);
  ex_decode_soft(sd, coded, clen, out, cap);
  dt_cc_hybrid_soft_decoder_destroy(sd);

  printf("Part A - acquiring lock on our own stream (c_locked over time):\n");
  for (int i = 0; i <= 96; i += 16) {
    printf("  bit %3d:  c_locked=%.2f  -> %s\n", i, out[i].c_locked,
           ex_sym(ex_hard_of(out[i])));
  }
  printf("  (lock climbs from ~0 to ~1 - that warm-up ramp is blind acquisition;\n"
         "   joining a stream mid-flight acquires exactly the same way.)\n\n");

  /* ---- Part B: a SIBLING code's stream - the decoder cannot lock ---- */
  int slen = ex_encode(sibling, msg, N, coded, cap); /* encoded with the WRONG code */

  /* The SOFT decoder shows lock stays low. */
  dt_stream_soft_decoder *sd2 = dt_cc_hybrid_soft_decoder_create(own, &p);
  int got = ex_decode_soft(sd2, coded, slen, out, cap);
  dt_cc_hybrid_soft_decoder_destroy(sd2);
  double lock_sum = 0.0;
  for (int i = 0; i < got; ++i) {
    lock_sum += out[i].c_locked;
  }

  /* The HARD decoder, not locked, emits DT_ABSENT rather than guessing values. */
  dt_bit *hout = malloc((size_t)cap);
  dt_stream_decoder *hd = dt_cc_hybrid_decoder_create(own, &p);
  int hgot = ex_decode_hard(hd, coded, slen, hout, cap);
  dt_cc_hybrid_decoder_destroy(hd);
  int absent = 0;
  for (int i = 0; i < hgot; ++i) {
    if (hout[i] == DT_ABSENT) {
      ++absent;
    }
  }

  printf("Part B - decoding a SIBLING code's stream with the wrong decoder:\n");
  printf("  mean c_locked = %.2f (low: never locks)\n", lock_sum / got);
  printf("  %d of %d hard outputs read DT_ABSENT - the decoder reports it is not\n"
         "  tracking this code rather than emitting confident wrong bits.\n",
         absent, hgot);

  free(coded);
  free(out);
  free(hout);
  dt_cc_code_destroy(own);
  dt_cc_code_destroy(sibling);
  return 0;
}
