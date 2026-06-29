/*
 * 01 - Basics: encode a message, send it over a noisy channel, decode it back.
 *
 * The "hello world" of drifty. Pick a convolutional code, encode a bit stream
 * through the shared encoder, corrupt it with random bit flips, and recover the
 * original with the simplest decoder (Viterbi). Demonstrates the core
 * encode -> channel -> decode round trip and that a convolutional code corrects
 * flipped bits.
 *
 * Build: see examples/README.md. Run: ./01_basics
 */

#include "util.h"

#include <drifty/cc/viterbi.h>

int main(void) {
  /* 1. Pick a ready-made code. The sender and receiver must use the SAME code.
   *    K7 rate-1/2 emits 2 coded bits per input bit and has memory length K=7. */
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code); /* coded bits per input bit */
  const int K = dt_cc_code_k(code); /* memory length            */
  printf("code: K%d rate-1/%d  (%d coded bits per input bit)\n", K, n, n);

  /* 2. A random message of N information bits. */
  enum { N = 240 };
  uint64_t rng = 0xC0FFEE01u;
  dt_bit msg[N];
  ex_rand_bits(msg, N, &rng);

  /* 3. Encode. The coded stream is at most (N + K) * n bits (the + K is the
   *    flush tail the encoder appends to drain its shift register). */
  const int cap = (N + K) * n + 64;
  dt_bit *coded = malloc((size_t)cap);
  int clen = ex_encode(code, msg, N, coded, cap);
  printf("encoded %d info bits -> %d coded bits\n\n", N, clen);

  /* The output trails the input by a look-ahead delay, so the first few recovered
   * bits are warm-up. Discard a generous prefix and check the settled remainder. */
  const int warmup = 6 * K;
  dt_bit *out = malloc((size_t)cap);

  /* 4a. Clean channel: perfect round trip. */
  {
    dt_stream_decoder *dec = dt_cc_viterbi_decoder_create(code);
    int got = ex_decode_hard(dec, coded, clen, out, cap);
    dt_cc_viterbi_decoder_destroy(dec);
    int err = ex_count_errors(out, msg, warmup, N);
    printf("clean channel:   recovered %d bits, %d errors in [%d, %d)\n", got,
           err, warmup, N);
  }

  /* 4b. ~3% of the coded bits flipped: the code corrects them all. */
  {
    dt_bit *rx = malloc((size_t)clen);
    memcpy(rx, coded, (size_t)clen);
    ex_flip(rx, clen, 0.03, &rng);

    dt_stream_decoder *dec = dt_cc_viterbi_decoder_create(code);
    int got = ex_decode_hard(dec, rx, clen, out, cap);
    dt_cc_viterbi_decoder_destroy(dec);
    int err = ex_count_errors(out, msg, warmup, N);
    printf("3%% bit flips:    recovered %d bits, %d errors in [%d, %d)  %s\n", got,
           err, warmup, N, err == 0 ? "(all corrected)" : "");
    free(rx);
  }

  free(coded);
  free(out);
  dt_cc_code_destroy(code);
  return 0;
}
