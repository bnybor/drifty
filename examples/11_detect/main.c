/*
 * 11 - Blind code detection: the `detect` meta-codec.
 *
 * Every other codec here ENCODES or DECODES a known code. `detect` is different:
 * it is a blind detector that, given an arbitrary bit stream with NO prior
 * knowledge or coordination (no code, rate, generators, or alignment), reports per
 * position how confident it is that a convolutional code is present. It does not
 * recover bit values - it answers "is there a code here?".
 *
 * Its soft output repurposes two dt_soft_bit fields (the two need not sum to 1):
 *   c_erasure = confidence a convolutional code IS present
 *   c_absent  = confidence a convolutional code is NOT present
 *
 * Part A: a pure coded stream reads as "present"; a pure random stream as "absent".
 * Part B: splice a coded segment into the middle of a random stream and watch
 *         detect light up exactly the coded region - it finds the code with no hint
 *         of where (or whether) it is.
 *
 * Method: a convolutional code is linear, so windows of coded bits are GF(2)
 * rank-deficient (parity checks) while random bits are full-rank. detect sweeps
 * candidate block sizes and measures the deficiency. It works in the clean /
 * very-low-noise regime (see doc/cc/detect.md).
 *
 * Run: ./11_detect
 */

#include "util.h"

#include <drifty/cc/detect.h>

/* mean coded-confidence (c_erasure) over recovered records [lo, hi) */
static double mean_coded(const dt_soft_bit *o, int lo, int hi) {
  double s = 0;
  for (int i = lo; i < hi; ++i) {
    s += o[i].c_erasure;
  }
  return hi > lo ? s / (hi - lo) : 0.0;
}
static double mean_absent(const dt_soft_bit *o, int lo, int hi) {
  double s = 0;
  for (int i = lo; i < hi; ++i) {
    s += o[i].c_absent;
  }
  return hi > lo ? s / (hi - lo) : 0.0;
}
static void bar(double v) {
  int k = (int)(v * 24.0 + 0.5);
  putchar('[');
  for (int i = 0; i < 24; ++i) {
    putchar(i < k ? '#' : ' ');
  }
  printf("] %.2f", v);
}

int main(void) {
  uint64_t rng = 0xDE7EC701u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);

  /* detect takes the same rich channel model as hybrid/maxir. We expect a clean
   * channel here (p_flip 0), so a "no code" verdict is fully trusted; telling it
   * to expect noise would damp the no-code confidence (a code could hide under
   * the noise). decision_depth is required (>= 1) but unused by the detector. */
  dt_cc_detect_stream_params dp = {0};
  dp.decision_depth = 40;

  /* ---- Part A: pure coded vs pure random ---- */
  printf("Part A - a whole stream, coded vs random:\n");
  enum { NINFO = 2000 };
  const int cap = (NINFO + K) * n + 512;
  dt_bit *coded = malloc((size_t)cap);
  dt_soft_bit *out = malloc((size_t)cap * sizeof *out);

  dt_bit *msg = malloc((size_t)NINFO);
  ex_rand_bits(msg, NINFO, &rng);
  int clen = ex_encode(code, msg, NINFO, coded, cap);
  {
    dt_stream_soft_decoder *sd = dt_cc_detect_soft_decoder_create(&dp);
    int got = ex_decode_soft(sd, coded, clen, out, cap);
    dt_cc_detect_soft_decoder_destroy(sd);
    printf("  coded  (%d bits): code-present ", clen);
    bar(mean_coded(out, 0, got));
    printf("   no-code %.2f\n", mean_absent(out, 0, got));
  }
  {
    dt_bit *r = malloc((size_t)clen);
    for (int i = 0; i < clen; ++i) {
      r[i] = (ex_rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
    }
    dt_stream_soft_decoder *sd = dt_cc_detect_soft_decoder_create(&dp);
    int got = ex_decode_soft(sd, r, clen, out, cap);
    dt_cc_detect_soft_decoder_destroy(sd);
    printf("  random (%d bits): code-present ", clen);
    bar(mean_coded(out, 0, got));
    printf("   no-code %.2f\n", mean_absent(out, 0, got));
    free(r);
  }

  /* ---- Part B: localize a coded segment hidden in random noise ---- */
  printf("\nPart B - a coded segment spliced into a random stream. detect is given\n"
         "no hint where (or whether) a code is; it lights up the coded region:\n\n");
  enum { PRE = 1536, MID_INFO = 1400, POST = 1536, SCAP = 16384 };
  dt_bit *stream = malloc(SCAP);
  int len = 0;
  for (int i = 0; i < PRE; ++i) { /* random prefix */
    stream[len++] = (ex_rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
  }
  dt_bit *mid = malloc((size_t)(MID_INFO + K) * (size_t)n + 64);
  ex_rand_bits(msg, MID_INFO, &rng);
  int midlen = ex_encode(code, msg, MID_INFO, mid, (MID_INFO + K) * n + 64);
  const int code_lo = len, code_hi = len + midlen; /* true coded span */
  memcpy(stream + len, mid, (size_t)midlen);
  len += midlen;
  for (int i = 0; i < POST; ++i) { /* random suffix */
    stream[len++] = (ex_rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
  }

  dt_soft_bit *sout = malloc((size_t)SCAP * sizeof *sout);
  dt_stream_soft_decoder *sd = dt_cc_detect_soft_decoder_create(&dp);
  int got = ex_decode_soft(sd, stream, len, sout, SCAP);
  dt_cc_detect_soft_decoder_destroy(sd);

  printf("  true coded region: bits [%d, %d)\n", code_lo, code_hi);
  const int step = 384; /* one detection block */
  for (int lo = 0; lo + step <= got; lo += step) {
    int mid_pos = lo + step / 2;
    int in_code = (mid_pos >= code_lo && mid_pos < code_hi);
    printf("  bits %5d..%5d %s ", lo, lo + step, in_code ? "(coded)" : "(random)");
    bar(mean_coded(sout, lo, lo + step));
    putchar('\n');
  }
  printf("\nThe bar tracks code-present confidence: ~0 in the random regions, ~1\n"
         "across the coded segment.\n");

  /* ---- Part C: detection survives indels (drift that desyncs a normal decoder) ---- */
  printf("\nPart C - detection through an insert/delete channel. Indels shift the\n"
         "bit phase, yet detect still finds the code (the runs between indels stay\n"
         "aligned, and it only needs one clean run to fire):\n");
  ex_rand_bits(msg, NINFO, &rng);
  clen = ex_encode(code, msg, NINFO, coded, cap);
  dt_bit *chan = malloc((size_t)cap);
  double idel[] = {0.0, 0.005, 0.01, 0.02};
  for (int k = 0; k < 4; ++k) {
    int rl = ex_delete(coded, clen, idel[k], &rng, chan); /* deletions = drift */
    dt_stream_soft_decoder *sd = dt_cc_detect_soft_decoder_create(&dp);
    int g = ex_decode_soft(sd, chan, rl, out, cap);
    dt_cc_detect_soft_decoder_destroy(sd);
    printf("  %.1f%% deletions (%d bits): code-present ", idel[k] * 100.0, rl);
    bar(mean_coded(out, 0, g));
    putchar('\n');
  }
  printf("\nDetection degrades gracefully with the indel rate (it is exact-parity\n"
         "based, so it holds in the low-noise regime - see doc/cc/detect.md).\n");

  free(coded);
  free(out);
  free(msg);
  free(stream);
  free(mid);
  free(sout);
  free(chan);
  dt_cc_code_destroy(code);
  return 0;
}
