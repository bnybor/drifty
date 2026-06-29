/*
 * 02 - Custom codes: define your own convolutional code instead of a preset.
 *
 * drifty ships a catalogue of standard codes (used in 01), but you can also build
 * a custom one from generator polynomials with dt_cc_code_create(). This shows the
 * generator format and that a custom code encodes and decodes exactly like a
 * standard one. Demonstrates dt_cc_code_create / dt_cc_code_n / dt_cc_code_k.
 *
 * Run: ./02_custom_code
 */

#include "util.h"

#include <drifty/cc/viterbi.h>

int main(void) {
  /* A convolutional code is defined by K (memory length) and a set of generator
   * tap masks, one per output bit. Each generator is a K-bit mask over the shift
   * register; an output bit is the parity (XOR) of the register bits it taps.
   *
   * Here is the classic K=7, rate-1/2 code (generators 0171 and 0133 octal). With
   * two generators it emits 2 coded bits per input bit. */
  const unsigned int generators[] = {0171u, 0133u};
  const int K = 7;
  const int num_gen = (int)(sizeof generators / sizeof generators[0]);

  dt_cc_code *code = dt_cc_code_create(K, generators, num_gen);
  if (!code) {
    fprintf(stderr, "custom code rejected (check K in 2..9 and the generators)\n");
    return 1;
  }
  const int n = dt_cc_code_n(code);
  printf("custom code: K=%d, %d generators -> rate-1/%d (n=%d, k reported as %d)\n",
         K, num_gen, n, dt_cc_code_n(code), dt_cc_code_k(code));

  /* Encode + decode a message exactly as with a standard code. */
  enum { N = 200 };
  uint64_t rng = 0xA5A5A5A5u;
  dt_bit msg[N];
  ex_rand_bits(msg, N, &rng);

  const int cap = (N + K) * n + 64;
  dt_bit *coded = malloc((size_t)cap);
  dt_bit *out = malloc((size_t)cap);
  int clen = ex_encode(code, msg, N, coded, cap);

  ex_flip(coded, clen, 0.02, &rng); /* 2% flips */
  dt_stream_decoder *dec = dt_cc_viterbi_decoder_create(code);
  int got = ex_decode_hard(dec, coded, clen, out, cap);
  dt_cc_viterbi_decoder_destroy(dec);

  const int warmup = 6 * K;
  int err = ex_count_errors(out, msg, warmup, N);
  printf("encoded %d bits -> %d coded; decoded %d, %d errors after 2%% flips %s\n",
         N, clen, got, err, err == 0 ? "(recovered)" : "");

  /* A custom code interoperates with every decoder and the whole stack, just like
   * a standard one - the only requirement is that sender and receiver share it. */

  free(coded);
  free(out);
  dt_cc_code_destroy(code);
  return 0;
}
