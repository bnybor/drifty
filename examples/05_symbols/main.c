/*
 * 05 - The non-boolean symbols: DT_INVALID and DT_ERASURE round-trip.
 *
 * Input bits are usually DT_TRUE / DT_FALSE, but drifty's alphabet has two more
 * symbols a sender can encode, and they survive the channel:
 *
 *   DT_INVALID  - a deliberate non-value ("poison"). The encoder marks the coded
 *                 bits that carry it; the decoder recovers the slot as DT_INVALID
 *                 (c_invalid ~ 1), never silently as a 0 or 1.
 *   DT_ERASURE  - a value deferred to the channel. Told to expect erasures
 *                 (p_ovr_erase), the decoder recovers the slot as DT_ERASURE.
 *
 * This encodes a message containing both and shows each comes back as itself while
 * the ordinary bits around them decode normally. Demonstrates the full transmit
 * alphabet end to end.
 *
 * Run: ./05_symbols
 */

#include "util.h"

#include <drifty/cc/hybrid.h>

int main(void) {
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int depth = 6 * K;

  enum { N = 240 };
  uint64_t rng = 0x1A2B3C4Du;
  dt_bit msg[N];
  ex_rand_bits(msg, N, &rng); /* ordinary DT_TRUE / DT_FALSE */

  /* Plant a deliberate non-value and a deferred (erasure) value in the message. */
  const int poison_at = 90, erase_at = 150;
  msg[poison_at] = DT_INVALID;
  msg[erase_at] = DT_ERASURE;
  printf("sent: msg[%d] = DT_INVALID (poison), msg[%d] = DT_ERASURE (deferred)\n\n",
         poison_at, erase_at);

  const int cap = (N + K) * n + 64;
  dt_bit *coded = malloc((size_t)cap);
  int clen = ex_encode(code, msg, N, coded, cap);
  ex_flip(coded, clen, 0.01, &rng); /* light noise; both symbols still survive */

  /* p_ovr_erase tells the decoder erased coded bits are expected, so a deferred
   * value reads back as DT_ERASURE rather than being guessed. */
  dt_cc_hybrid_stream_params p = {.decision_depth = depth, .p_flip = 0.02f,
                                  .p_ovr_erase = 0.05f};
  dt_stream_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(code, &p);
  dt_soft_bit *out = malloc((size_t)cap * sizeof *out);
  int got = ex_decode_soft(sd, coded, clen, out, cap);
  dt_cc_hybrid_soft_decoder_destroy(sd);
  (void)got;

  printf("recovered:\n");
  printf("  poison slot %3d -> %s   (c_invalid=%.2f)\n", poison_at,
         ex_sym(ex_hard_of(out[poison_at])), out[poison_at].c_invalid);
  printf("  erase  slot %3d -> %s   (c_erasure=%.2f, c_invalid=%.2f)\n", erase_at,
         ex_sym(ex_hard_of(out[erase_at])), out[erase_at].c_erasure,
         out[erase_at].c_invalid);

  int around_ok = 1;
  for (int i = depth + 4 * K; i < poison_at - 1; ++i) {
    if (ex_hard_of(out[i]) != msg[i]) {
      around_ok = 0;
    }
  }
  printf("  ordinary bits around them decode normally: %s\n",
         around_ok ? "yes" : "no");

  free(coded);
  free(out);
  dt_cc_code_destroy(code);
  return 0;
}
