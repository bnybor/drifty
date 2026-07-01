/*
 * 14 - Pipeline funnel: example 13's detection-routed receiver, rebuilt so ALL the
 *      funnel logic lives INSIDE a composed pipe graph (see layout.dot). This driver
 *      only builds the time-varying channel, then pushes blocks into the funnel, ticks
 *      it, and reads recovered frames from its final frame pipe - it never sees the
 *      detectors, diverters, or decoders inside.
 *
 * Split across files: funnel.{c,h} is the decode graph (splitters, two diverters, two
 * controlling executors, combiner, frame pipe, all in a pipe container); stack.{c,h}
 * is the outer rs251 + marker transmit/receive; this file is the channel and the trace.
 *
 * Run: ./14_pipeline
 */

#include "util.h"

#include "funnel.h"
#include "stack.h"

#include <drifty/cc/ccode.h>

int main(void) {
  uint64_t rng = 0xF0FFEE01u;
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K5_RATE_1_5);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int WARM = 256; /* drift-free bit-sync preamble (see example 13) */

  dt_cc_detect_clean_stream_params clean = {0};
  clean.decision_depth = 40;
  clean.p_flip = 0.06f;
  dt_cc_detect_noisy_stream_params noisy = {0};
  noisy.decision_depth = 40;
  noisy.p_flip = 0.0f;
  dt_cc_bcjr_stream_params bp = {0};
  bp.decision_depth = 6 * K;
  bp.p_flip = 0.01f;
  dt_cc_maxir_stream_params mp = {0};
  mp.decision_depth = 96;
  mp.max_drift = 24;
  mp.p_flip = 0.02f;
  mp.p_ins_true = 0.0025f;
  mp.p_ins_false = 0.0025f;
  mp.p_del = 0.05f;
  mp.p_ovr_erase = 0.01f;

  /* -- Transmit: two framed payloads; keep a flat copy to match against on receive. -- */
  static unsigned char payA[FRAMES][RS_MSG], payB[FRAMES][RS_MSG];
  static unsigned char allpay[2 * FRAMES][RS_MSG];
  for (int f = 0; f < FRAMES; ++f) {
    for (int b = 0; b < RS_MSG; ++b) {
      payA[f][b] = (unsigned char)ex_rng_next(&rng);
      payB[f][b] = (unsigned char)ex_rng_next(&rng);
    }
  }
  memcpy(allpay, payA, sizeof payA);
  memcpy(allpay + FRAMES, payB, sizeof payB);

  enum { FCAP = 4096, GAP = 4096, JUNK = 4096, BLOCK = FUNNEL_BLOCK };
  const int mcap = (FCAP + WARM + K) * n + 512;
  const int scap = 6 * mcap;
  dt_bit *framed = malloc((size_t)FCAP);
  dt_bit *info = malloc((size_t)(FCAP + WARM));
  dt_bit *coded = malloc((size_t)mcap);
  dt_bit *tmp = malloc((size_t)mcap);
  dt_bit *stream = malloc((size_t)scap);

  /* One continuous channel: clean signal, dead air, noisy+drift signal, dead air,
   * corrupt junk. Regions are padded so each signal starts on a block boundary. */
  int len = 0;
  int a_lo = len;
  {
    int fl = stack_build_framed(payA, FRAMES, framed, FCAP);
    for (int i = 0; i < WARM; ++i) {
      info[i] = DT_FALSE;
    }
    memcpy(info + WARM, framed, (size_t)fl);
    len += ex_encode(code, info, WARM + fl, coded, mcap);
    memcpy(stream, coded, (size_t)len);
  }
  int a_hi = len;
  int gap_lo = len;
  len = (len + BLOCK - 1) / BLOCK * BLOCK + GAP;
  ex_rand_bits(stream + gap_lo, len - gap_lo, &rng);
  int gap_hi = len;
  int b_lo = len;
  {
    int fl = stack_build_framed(payB, FRAMES, framed, FCAP);
    for (int i = 0; i < WARM; ++i) {
      info[i] = DT_FALSE;
    }
    memcpy(info + WARM, framed, (size_t)fl);
    int cl = ex_encode(code, info, WARM + fl, coded, mcap);
    int lead = WARM * n;
    memcpy(stream + len, coded, (size_t)lead);
    int t = ex_insert(coded + lead, cl - lead, 0.005, &rng, tmp);
    int held = ex_delete(tmp, t, 0.05, &rng, stream + len + lead);
    held += lead;
    ex_flip(stream + len, held, 0.02, &rng);
    for (int i = 0; i < 64 && lead + 1200 + i < held; ++i) {
      stream[len + lead + 1200 + i] = DT_ERASURE; /* burst for rs251 to mop up */
    }
    len += held;
  }
  int b_hi = len;
  int noise_lo = len;
  len = (len + BLOCK - 1) / BLOCK * BLOCK + GAP;
  ex_rand_bits(stream + noise_lo, len - noise_lo, &rng);
  int noise_hi = len;
  int junk_lo = len;
  ex_rand_bits(stream + len, JUNK, &rng);
  for (int i = 0; i < JUNK; i += 60) {
    stream[len + i] = DT_INVALID;
  }
  len += JUNK;
  int junk_hi = len;

  /* -- The funnel: build once, then only push / tick / read frames. -- */
  dt_funnel *funnel = dt_funnel_create(code, &clean, &noisy, &bp, &mp);
  if (!funnel) {
    return 1;
  }
  dt_pipe_sink *in = dt_funnel_input(funnel);

  printf("The whole funnel is one composed pipe graph (see layout.dot); this driver only\n"
         "pushes channel blocks and reads frames back. Each row is a %d-bit block.\n"
         "(c_erasure, c_absent) = (code-present, no-code); B=bcjr M=maxir .=drop(clean) "
         "x=drop(noisy).\n\n", BLOCK);
  printf("  block   channel        detect_clean      detect_noisy     -> route\n");

  int used_noisy = 0, blk = 0;
  for (int pos = 0; pos < len; pos += BLOCK, ++blk) {
    int blen = (pos + BLOCK <= len) ? BLOCK : (len - pos);
    for (int u = 0; u < blen;) {
      int w = in->push(in, stream + pos + u, (size_t)(blen - u));
      if (w <= 0) {
        break;
      }
      u += w;
    }
    dt_funnel_tick(funnel);
    struct dt_funnel_trace tr = dt_funnel_last(funnel);
    used_noisy += tr.ran_noisy;

    int mid = pos + blen / 2;
    const char *chan = (mid >= a_lo && mid < a_hi)          ? "clean signal"
                       : (mid >= b_lo && mid < b_hi)         ? "noisy+drift"
                       : (mid >= gap_lo && mid < gap_hi)     ? "dead air"
                       : (mid >= noise_lo && mid < noise_hi) ? "dead air"
                       : (mid >= junk_lo && mid < junk_hi)   ? "corrupt junk"
                                                             : "-";
    printf("  %3d   %-14s  (%.2f, %.2f)", blk, chan, tr.cce, tr.cca);
    if (tr.ran_noisy) {
      printf("     (%.2f, %.2f)  ->  %c\n", tr.nce, tr.nca, tr.route);
    } else {
      printf("     %-13s ->  %c\n", "(skipped)", tr.route);
    }
  }
  dt_funnel_finalize(funnel);

  /* -- Receive: walk the funnel's final frame pipe; rs251-decode each frame. -- */
  int found, recovered, residual;
  stack_recover(dt_funnel_frames(funnel), allpay, 2 * FRAMES, &found, &recovered,
                &residual);

  printf("\nFrom the funnel's final frame pipe: %d frames delimited, %d of %d payloads "
         "recovered;\nrs251 mopped up %d inner-decoder bit error%s. detect_noisy ran on "
         "%d of %d blocks\n(only where detect_clean could not decide). A few frames the "
         "drift shook loose are\nnot recovered here - a fountain code upstream of rs251 "
         "rebuilds the whole message.\n",
         found, recovered, 2 * FRAMES, residual, residual == 1 ? "" : "s", used_noisy,
         blk);

  dt_funnel_destroy(funnel);
  free(framed);
  free(info);
  free(coded);
  free(tmp);
  free(stream);
  dt_cc_code_destroy(code);
  return 0;
}
