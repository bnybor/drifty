/*
 * 11 - Blind code detection: the `detect_clean` and `detect_noisy` meta-codecs.
 *
 * Every other codec here ENCODES or DECODES a known code. The two detect codecs are
 * different: each is a blind detector that, given an arbitrary bit stream with NO
 * prior knowledge or coordination (no code, rate, generators, or alignment), reports
 * per position how confident it is that a convolutional code is present. They do not
 * recover bit values - they answer "is there a code here?".
 *
 * Soft output repurposes two dt_soft_bit fields (the two need not sum to 1):
 *   c_erasure = consistency with "a convolutional code IS present"
 *   c_absent  = consistency with "no code / the stream is random"
 *
 * There are two, trading footprint for noise tolerance - this example shows when to
 * reach for which:
 *   detect_clean - exact GF(2) rank deficiency. A few KB, no transform; tolerates
 *                 indels and ~1% flips. The embeddable default (Parts A and B).
 *   detect_noisy - parity-check bias via a Walsh-Hadamard transform. A ~64 KB
 *                 histogram and somewhat more compute, but tolerates flips (~5-8%), indels
 *                 (~2-3%), and light combinations of the two (Part C).
 *
 * Part A: a pure coded stream reads "present", a pure random stream "absent" (clean).
 * Part B: splice a coded segment into a random stream and watch clean light up
 *         exactly the coded region - it finds the code with no hint of where it is.
 * Part C: add bit FLIPS - which break clean's exact parity - and watch noisy hold on
 *         where clean collapses, including through a combined flip+indel channel.
 * Part D: DT_INVALID symbols as present-axis evidence - un-encodable invalid placement
 *         damps code-present (toward the (low, low) corner) while leaving no-code alone.
 *
 * Run: ./11_detect
 */

#include "util.h"

#include <drifty/cc/detect_noisy.h>
#include <drifty/cc/detect_clean.h>

/* mean code-present consistency (c_erasure) over recovered records [lo, hi) */
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

  /* Both detectors take the same rich channel model as hybrid/maxir. We expect a
   * clean channel here (p_flip 0), so an unstructured window rules a code out
   * (code-present ~0); telling a detector to expect noise would instead hold the
   * code-present read up there (a code could be hidden by the noise), leaving the
   * no-code read alone. decision_depth is required (>= 1) but unused by the detectors. */
  dt_cc_detect_clean_stream_params clean = {0};
  clean.decision_depth = 40;
  dt_cc_detect_noisy_stream_params noisy = {0};
  noisy.decision_depth = 40;

  /* ---- Part A: pure coded vs pure random (detect_clean) ---- */
  printf("Part A - a whole stream, coded vs random (detect_clean):\n");
  enum { NINFO = 2000 };
  const int cap = (NINFO + K) * n + 512;
  dt_bit *coded = malloc((size_t)cap);
  dt_soft_bit *out = malloc((size_t)cap * sizeof *out);

  dt_bit *msg = malloc((size_t)NINFO);
  ex_rand_bits(msg, NINFO, &rng);
  int clen = ex_encode(code, msg, NINFO, coded, cap);
  {
    dt_stream_soft_decoder *sd = dt_cc_detect_clean_soft_decoder_create(&clean);
    int got = ex_decode_soft(sd, coded, clen, out, cap);
    dt_cc_detect_clean_soft_decoder_destroy(sd);
    printf("  coded  (%d bits): code-present ", clen);
    bar(mean_coded(out, 0, got));
    printf("   no-code %.2f\n", mean_absent(out, 0, got));
  }
  {
    dt_bit *r = malloc((size_t)clen);
    for (int i = 0; i < clen; ++i) {
      r[i] = (ex_rng_next(&rng) & 1) ? DT_TRUE : DT_FALSE;
    }
    dt_stream_soft_decoder *sd = dt_cc_detect_clean_soft_decoder_create(&clean);
    int got = ex_decode_soft(sd, r, clen, out, cap);
    dt_cc_detect_clean_soft_decoder_destroy(sd);
    printf("  random (%d bits): code-present ", clen);
    bar(mean_coded(out, 0, got));
    printf("   no-code %.2f\n", mean_absent(out, 0, got));
    free(r);
  }

  /* ---- Part B: localize a coded segment hidden in random noise (detect_clean) ---- */
  printf("\nPart B - a coded segment spliced into a random stream. detect_clean is\n"
         "given no hint where (or whether) a code is; it lights up the coded region:\n\n");
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
  dt_stream_soft_decoder *sd = dt_cc_detect_clean_soft_decoder_create(&clean);
  int got = ex_decode_soft(sd, stream, len, sout, SCAP);
  dt_cc_detect_clean_soft_decoder_destroy(sd);

  printf("  true coded region: bits [%d, %d)\n", code_lo, code_hi);
  const int step = 384; /* one detection block */
  for (int lo = 0; lo + step <= got; lo += step) {
    int mid_pos = lo + step / 2;
    int in_code = (mid_pos >= code_lo && mid_pos < code_hi);
    printf("  bits %5d..%5d %s ", lo, lo + step, in_code ? "(coded)" : "(random)");
    bar(mean_coded(sout, lo, lo + step));
    putchar('\n');
  }
  printf("\nThe bar tracks code-present consistency: ~0 in the random regions, ~1\n"
         "across the coded segment.\n");

  /* ---- Part C: bit flips - where clean collapses and noisy holds on ---- */
  printf("\nPart C - the same coded stream through a bit-FLIP channel. clean uses\n"
         "exact parity, so a few flips erase the structure it looks for; noisy scores\n"
         "parity BIAS, which flips only weaken. code-present consistency, clean vs noisy:\n\n");
  ex_rand_bits(msg, NINFO, &rng);
  clen = ex_encode(code, msg, NINFO, coded, cap);
  dt_bit *chan = malloc((size_t)cap);
  double fr[] = {0.0, 0.01, 0.03, 0.05};
  for (int k = 0; k < 4; ++k) {
    memcpy(chan, coded, (size_t)clen);
    for (int i = 0; i < clen; ++i) { /* flip each bit with probability fr[k] */
      if (ex_rng_unit(&rng) < fr[k]) {
        chan[i] = (chan[i] == DT_TRUE) ? DT_FALSE : DT_TRUE;
      }
    }
    dt_stream_soft_decoder *sl = dt_cc_detect_clean_soft_decoder_create(&clean);
    int gl = ex_decode_soft(sl, chan, clen, out, cap);
    double ml = mean_coded(out, 0, gl);
    dt_cc_detect_clean_soft_decoder_destroy(sl);
    dt_stream_soft_decoder *sf = dt_cc_detect_noisy_soft_decoder_create(&noisy);
    int gf = ex_decode_soft(sf, chan, clen, out, cap);
    double mf = mean_coded(out, 0, gf);
    dt_cc_detect_noisy_soft_decoder_destroy(sf);
    printf("  %.0f%% flips:  clean ", fr[k] * 100.0);
    bar(ml);
    printf("   noisy ");
    bar(mf);
    putchar('\n');
  }

  /* combined flip + indel: the case that motivates one codec carrying both */
  printf("\n  combined 3%% flip + 0.5%% deletion (flips AND drift at once):\n");
  {
    int rl = ex_delete(coded, clen, 0.005, &rng, chan); /* deletions = drift */
    for (int i = 0; i < rl; ++i) {
      if (ex_rng_unit(&rng) < 0.03) {
        chan[i] = (chan[i] == DT_TRUE) ? DT_FALSE : DT_TRUE;
      }
    }
    dt_stream_soft_decoder *sl = dt_cc_detect_clean_soft_decoder_create(&clean);
    int gl = ex_decode_soft(sl, chan, rl, out, cap);
    double ml = mean_coded(out, 0, gl);
    dt_cc_detect_clean_soft_decoder_destroy(sl);
    dt_stream_soft_decoder *sf = dt_cc_detect_noisy_soft_decoder_create(&noisy);
    int gf = ex_decode_soft(sf, chan, rl, out, cap);
    double mf = mean_coded(out, 0, gf);
    dt_cc_detect_noisy_soft_decoder_destroy(sf);
    printf("  %d bits:    clean ", rl);
    bar(ml);
    printf("   noisy ");
    bar(mf);
    putchar('\n');
  }
  /* ---- Part D: DT_INVALID symbols as present-axis evidence (detect_clean) ---- */
  printf("\nPart D - a DT_INVALID symbol is present-axis EVIDENCE: a value no code\n"
         "could emit at that spot argues AGAINST a code. Lone invalids (un-encodable)\n"
         "damp code-present while leaving no-code alone; an encodable invalid RUN does\n"
         "not. The same coded stream, clean vs sprinkled with invalids:\n\n");
  ex_rand_bits(msg, NINFO, &rng);
  clen = ex_encode(code, msg, NINFO, coded, cap);
  struct { const char *label; int every; int run; } dcase[] = {
      {"clean            ", 0, 0},   /* baseline: code-present ~1, no-code ~0      */
      {"+ lone invalids  ", 100, 0}, /* un-encodable singletons: code-present -> 0 */
      {"+ one invalid run", 0, 60},  /* encodable shape: no penalty                */
  };
  for (int k = 0; k < 3; ++k) {
    memcpy(chan, coded, (size_t)clen);
    for (int i = dcase[k].every; dcase[k].every && i < clen; i += dcase[k].every) {
      chan[i] = DT_INVALID; /* a lone invalid every `every` bits */
    }
    for (int i = 200; i < 200 + dcase[k].run && i < clen; ++i) {
      chan[i] = DT_INVALID; /* one contiguous invalid run */
    }
    dt_stream_soft_decoder *s = dt_cc_detect_clean_soft_decoder_create(&clean);
    int g = ex_decode_soft(s, chan, clen, out, cap);
    dt_cc_detect_clean_soft_decoder_destroy(s);
    printf("  coded %s  code-present ", dcase[k].label);
    bar(mean_coded(out, 0, g));
    printf("   no-code %.2f\n", mean_absent(out, 0, g));
  }
  printf("\nLone invalids crush code-present yet no-code stays low - the (low, low)\n"
         "corner: code-like structure no single code could have emitted. A single\n"
         "invalid RUN is an encodable shape, so it carries no such penalty.\n");

  printf("\nclean stays the cheap pick for clean / very-low-noise streams; noisy earns\n"
         "its ~64 KB when the channel flips or drifts. See doc/cc/detect_clean.md and\n"
         "doc/cc/detect_noisy.md.\n");

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
