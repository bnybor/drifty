/*
 * 07 - Block coding: the rs251 Reed-Solomon outer code.
 *
 * Besides the streaming convolutional codecs, drifty has a BLOCK codec: rs251, a
 * systematic Reed-Solomon RS(n, k) code over GF(251). k message symbols become an
 * n-symbol codeword with n-k parity symbols; it corrects errors and ERASURES
 * while 2*errors + erasures <= n-k. The block interface is buffer-based: you fill
 * the encoder's input buffer, call encode(), and read its output buffer (and the
 * mirror for decode).
 *
 * Shows a clean round trip, correction at the exact error+erasure budget, a
 * failure just past it, and the soft-input decoder. Demonstrates dt_block_encoder
 * / dt_block_decoder / dt_block_soft_decoder and rs251.
 *
 * Run: ./07_block_rs251
 */

#include "util.h"

#include <drifty/bc/rs251.h>
#include <drifty/block_decoder.h>
#include <drifty/block_encoder.h>
#include <drifty/block_soft_decoder.h>
#include <drifty/result.h>

/* A codeword symbol is 8 dt_bit, MSB first. */
static unsigned sym_get(const dt_bit *cw, int s) {
  unsigned v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 1) | (cw[s * 8 + i] == DT_TRUE ? 1u : 0u);
  }
  return v;
}
static void sym_put(dt_bit *cw, int s, unsigned v) {
  for (int i = 0; i < 8; ++i) {
    cw[s * 8 + i] = (v & (0x80u >> i)) ? DT_TRUE : DT_FALSE;
  }
}
static void sym_erase(dt_bit *cw, int s) {
  for (int i = 0; i < 8; ++i) {
    cw[s * 8 + i] = DT_ERASURE; /* a whole symbol whose value is lost */
  }
}

int main(void) {
  const uint16_t N = 40, Kk = 24; /* RS(40,24): 16 parity symbols, budget n-k=16 */

  dt_block_encoder *enc = dt_bc_rs251_block_encoder_create(N, Kk);
  dt_block_decoder *dec = dt_bc_rs251_block_decoder_create(N, Kk, /*spare=*/0);
  if (!enc || !dec) {
    return 1;
  }
  const size_t dlen = enc->decoded_len(enc); /* message bits  = (k-1)*8 */
  const size_t elen = enc->encoded_len(enc); /* codeword bits = n*8     */
  printf("RS(%u, %u): %zu message bits -> %zu codeword bits, budget n-k=%u\n\n",
         N, Kk, dlen, elen, N - Kk);

  /* Fill the encoder's input buffer with a random message and remember it. */
  uint64_t rng = 0xB10C00u;
  dt_bit *msg = malloc(dlen);
  dt_bit *in = enc->decoded_buf(enc);
  ex_rand_bits(in, (int)dlen, &rng);
  memcpy(msg, in, dlen);

  dt_result r;
  while ((r = enc->encode(enc)) == DT_AGAIN) {
  }
  if (r != DT_OK) {
    return 1;
  }
  dt_bit *codeword = malloc(elen);
  memcpy(codeword, enc->encoded_buf(enc), elen);

  /* Helper: load rx into the decoder, decode, report whether the message returns. */
#define TRY(label, rx)                                                          \
  do {                                                                          \
    memcpy(dec->encoded_buf(dec), (rx), elen);                                  \
    dt_result rr;                                                               \
    while ((rr = dec->decode(dec)) == DT_AGAIN) {                               \
    }                                                                           \
    int ok = (rr == DT_OK) &&                                                   \
             memcmp(dec->decoded_buf(dec), msg, dlen) == 0;                     \
    printf("  %-28s decode=%-7s message=%s\n", (label),                         \
           rr == DT_OK ? "OK" : "DECODE-ERR", ok ? "recovered" : "lost");       \
  } while (0)

  dt_bit *rx = malloc(elen);

  /* (a) clean */
  memcpy(rx, codeword, elen);
  TRY("clean", rx);

  /* (b) at budget: 4 errors + 8 erasures -> 2*4 + 8 = 16 = n-k */
  memcpy(rx, codeword, elen);
  for (int s = 0; s < 4; ++s) {
    sym_put(rx, s, (sym_get(rx, s) + 1) % 251); /* corrupt symbol s */
  }
  for (int s = 10; s < 18; ++s) {
    sym_erase(rx, s);
  }
  TRY("4 errors + 8 erasures", rx);

  /* (c) just past budget: 5 errors + 8 erasures -> 18 > 16, rejected */
  memcpy(rx, codeword, elen);
  for (int s = 0; s < 5; ++s) {
    sym_put(rx, s, (sym_get(rx, s) + 1) % 251);
  }
  for (int s = 10; s < 18; ++s) {
    sym_erase(rx, s);
  }
  TRY("5 errors + 8 erasures", rx);
#undef TRY

  /* (d) soft input: feed a dt_soft_bit codeword. The soft decoder reads each
   *     symbol by per-bit argmax and can iteratively erase its least reliable
   *     symbols on a failed decode. We mark a few symbols as soft-erasures. */
  dt_block_soft_decoder *sdec =
      dt_bc_rs251_block_soft_decoder_create(N, Kk, /*spare=*/0);
  dt_soft_bit *senc = sdec->encoded_buf(sdec);
  for (size_t i = 0; i < elen; ++i) {
    dt_soft_bit sb = {0, 0, 0, 0, 0, 0};
    if (codeword[i] == DT_TRUE) {
      sb.c_true = 1.0f;
    } else {
      sb.c_false = 1.0f;
    }
    sb.c_locked = 1.0f;
    senc[i] = sb;
  }
  for (int s = 10; s < 18; ++s) { /* 8 symbols arrive as soft erasures */
    for (int i = 0; i < 8; ++i) {
      dt_soft_bit sb = {0, 0, 1.0f, 0, 0, 0}; /* c_erasure = 1 */
      senc[s * 8 + i] = sb;
    }
  }
  dt_result sr;
  while ((sr = sdec->decode(sdec)) == DT_AGAIN) {
  }
  int sok = (sr == DT_OK) && memcmp(sdec->decoded_buf(sdec), msg, dlen) == 0;
  printf("  %-28s decode=%-7s message=%s\n", "soft input (8 erasures)",
         sr == DT_OK ? "OK" : "DECODE-ERR", sok ? "recovered" : "lost");

  dt_bc_rs251_block_encoder_destroy(enc);
  dt_bc_rs251_block_decoder_destroy(dec);
  dt_bc_rs251_block_soft_decoder_destroy(sdec);
  free(msg);
  free(codeword);
  free(rx);
  return 0;
}
