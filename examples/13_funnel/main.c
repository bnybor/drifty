/*
 * 13 - Funnel: a streaming, detection-routed receiver over a full concatenated stack -
 *      the realistic shape of a drifty receiver.
 *
 * The transmit stack, per signal: a random payload is protected by an OUTER
 * Reed-Solomon block code (rs251), the RS blocks are wrapped as delimited frames (the
 * marker frame codec), and that framed bit stream is protected by an INNER
 * convolutional code and sent (see stack.c). The channel (channel.c) is ONE continuous
 * stream whose condition changes over time: a clean signal, a battered + drifting one,
 * dead air, then noise.
 *
 * This file is the FUNNEL: it streams the channel and escalates only as far as each
 * stretch needs, using drifty's two blind code-presence detectors as cheap gatekeepers
 * ahead of the inner decoders:
 *
 *   detect_clean (cheap) settles the easy calls: a clean signal -> bcjr (cheap).
 *   ...under noise its exact parity can't tell a battered signal from random, so it
 *   defers: uncertain -> detect_noisy (expensive), which asks "is there a signal at
 *   all?" - yes -> maxir (robust: it alone tracks the inserted/dropped bits bcjr
 *   cannot); no -> drop it.
 *
 *   detect_clean:  c_absent - c_erasure > 0.85 -> DROP;  c_erasure - c_absent > 0.85
 *                  -> bcjr;  else -> detect_noisy: c_absent - c_erasure > 0.85 ->
 *                  DROP; else -> maxir.
 *
 * The inner decoders are SOFT, so per-bit reliabilities survive outward: stack.c
 * reframes the recovered stream (the marker soft decoder resynchronises past boundary
 * junk) and rs251 mops up maxir's residual bit errors. Split across main.c (this
 * funnel loop), channel.c (the channel), and stack.c (the rs251 + marker layers).
 *
 * Run: ./13_funnel
 */

#include "util.h"

#include "channel.h"
#include "stack.h"

#include <drifty/cc/bcjr.h>
#include <drifty/cc/detect_clean.h>
#include <drifty/cc/detect_noisy.h>
#include <drifty/cc/maxir.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/streams.h>

/* Confidence needed to commit at a detector. */
#define CONF 0.85
/* Bits fed to each detector before routing, to clear its warm-up transient. */
#define PRIME_CLEAN 768
#define PRIME_NOISY 1600

enum route { R_BCJR, R_MAXIR, R_DROP_CLEAN, R_DROP_NOISY };
static char route_tag(enum route r) {
  return r == R_BCJR ? 'B' : r == R_MAXIR ? 'M' : r == R_DROP_CLEAN ? '.' : 'x';
}

/* Push one block into a persistent pipe, tick, drain every ready soft record. */
static int feed_pull(dt_pipe *p, const dt_bit *in, int len, dt_soft_bit *out,
                     int cap) {
  dt_pipe_sink *sink = p->sink(p);
  for (int u = 0; u < len;) {
    int w = sink->push(sink, in + u, (size_t)(len - u));
    if (w <= 0) {
      break;
    }
    u += w;
  }
  p->tick(p);
  dt_pipe_source *src = p->source(p);
  int got = 0, g;
  while ((g = src->soft_pull(src, out + got, (size_t)(cap - got))) > 0) {
    got += g;
  }
  return got;
}

/* Mean (c_erasure, c_absent) over a detector's block of soft records - the routing
 * signal (code-present, no-code). */
static void mean_conf(const dt_soft_bit *r, int n, double *ce, double *ca) {
  double se = 0, sa = 0;
  for (int i = 0; i < n; ++i) {
    se += r[i].c_erasure;
    sa += r[i].c_absent;
  }
  *ce = n ? se / n : 1.0;
  *ca = n ? sa / n : 1.0;
}

/* Warm a detector past its transient: push noise, tick, discard the output. */
static void prime(dt_pipe *p, int nbits, uint64_t *rng, dt_bit *noise,
                  dt_soft_bit *sink, int cap) {
  ex_rand_bits(noise, nbits, rng);
  feed_pull(p, noise, nbits, sink, cap);
}

/* Which region of the channel a block's midpoint falls in, for the trace. */
static const char *region_of(const struct channel_regions *r, int mid) {
  if (mid >= r->a_lo && mid < r->a_hi) return "clean signal";
  if (mid >= r->b_lo && mid < r->b_hi) return "noisy+drift";
  if (mid >= r->gap_lo && mid < r->gap_hi) return "dead air";
  if (mid >= r->noise_lo && mid < r->noise_hi) return "dead air";
  if (mid >= r->junk_lo && mid < r->junk_hi) return "corrupt junk";
  return "-";
}

int main(void) {
  uint64_t rng = 0xF0FFEE01u;
  /* K5 rate-1/5: the strongest standard code - its redundancy gives maxir the margin
   * to track drift. */
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K5_RATE_1_5);
  if (!code) {
    return 1;
  }
  const int K = dt_cc_code_k(code);

  /* Channel models. detect_clean is told to EXPECT noise (p_flip up) so a battered
   * stream reads UNCERTAIN and escalates rather than being ruled out; detect_noisy
   * expects a clean channel (p_flip 0) so it firmly rejects real noise while still
   * confirming a noisy code. maxir carries a drift model; bcjr is bit-aligned. */
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

  /* Two framed payloads and the channel that carries them. */
  static unsigned char payA[FRAMES][RS_MSG], payB[FRAMES][RS_MSG];
  for (int f = 0; f < FRAMES; ++f) {
    for (int b = 0; b < RS_MSG; ++b) {
      payA[f][b] = (unsigned char)ex_rng_next(&rng);
      payB[f][b] = (unsigned char)ex_rng_next(&rng);
    }
  }
  const int cap = channel_stream_cap(code);
  dt_bit *stream = malloc((size_t)cap);
  dt_soft_bit *rec = malloc((size_t)cap * sizeof *rec);  /* detector output scratch */
  dt_soft_bit *arec = malloc((size_t)cap * sizeof *arec); /* bcjr's decoded stream */
  dt_soft_bit *brec = malloc((size_t)cap * sizeof *brec); /* maxir's decoded stream */
  dt_bit noise[PRIME_NOISY];
  struct channel_regions reg;
  int len = channel_build(code, payA, payB, &rng, stream, cap, &reg);

  /* Persistent pipes, held open across the whole stream. */
  dt_stream_soft_decoder *dclean = dt_cc_detect_clean_soft_decoder_create(&clean);
  dt_stream_soft_decoder *dnoisy = dt_cc_detect_noisy_soft_decoder_create(&noisy);
  dt_stream_soft_decoder *bcjr = dt_cc_bcjr_soft_decoder_create(code, &bp);
  dt_stream_soft_decoder *maxir = dt_cc_maxir_soft_decoder_create(code, &mp);
  dt_pipe *pclean = dt_pipe_soft_decoder_create(dclean);
  dt_pipe *pnoisy = dt_pipe_soft_decoder_create(dnoisy);
  dt_pipe *bpipe = dt_pipe_soft_decoder_create(bcjr);
  dt_pipe *mpipe = dt_pipe_soft_decoder_create(maxir);
  pclean->begin(pclean);
  pnoisy->begin(pnoisy);
  bpipe->begin(bpipe);
  mpipe->begin(mpipe);
  prime(pclean, PRIME_CLEAN, &rng, noise, rec, cap);
  prime(pnoisy, PRIME_NOISY, &rng, noise, rec, cap);

  printf("Streaming one channel whose condition changes over time. Stack per signal:\n"
         "  payload -> rs251 (outer) -> marker (frames) -> cc (inner) -> channel\n"
         "Each row is a %d-bit block - the funnel's live routing as the stream flows.\n"
         "(c_erasure, c_absent) = (code-present, no-code); B=bcjr M=maxir "
         ".=drop(clean) x=drop(noisy).\n\n", BLOCK);
  printf("  block   channel        detect_clean      detect_noisy     -> route\n");

  int nA = 0, nB = 0, used_noisy = 0, blk = 0, prevB = 0, prevM = 0;
  for (int pos = 0; pos < len; pos += BLOCK, ++blk) {
    int blen = (pos + BLOCK <= len) ? BLOCK : (len - pos);
    const dt_bit *b = stream + pos;

    /* Stage 1 - detect_clean, on every block. */
    int gc = feed_pull(pclean, b, blen, rec, cap);
    double cce, cca;
    mean_conf(rec, gc, &cce, &cca);

    enum route route;
    double nce = -1, nca = -1;
    if (cca - cce > CONF) {
      route = R_DROP_CLEAN;
    } else if (cce - cca > CONF) {
      route = R_BCJR;
    } else {
      /* Uncertain: escalate to detect_noisy (expensive) - only these blocks pay. */
      ++used_noisy;
      int gn = feed_pull(pnoisy, b, blen, rec, cap);
      mean_conf(rec, gn, &nce, &nca);
      route = (nca - nce > CONF) ? R_DROP_NOISY : R_MAXIR;
    }

    /* Decode on the routed branch; re-acquire when a signal run restarts after a gap
     * (the decoder must lock onto the new preamble, not carry stale state). */
    if (route == R_BCJR) {
      if (!prevB) {
        bpipe->begin(bpipe);
      }
      nA += feed_pull(bpipe, b, blen, arec + nA, cap - nA);
    } else if (route == R_MAXIR) {
      if (!prevM) {
        mpipe->begin(mpipe);
      }
      nB += feed_pull(mpipe, b, blen, brec + nB, cap - nB);
    }
    prevB = (route == R_BCJR);
    prevM = (route == R_MAXIR);

    printf("  %3d   %-14s  (%.2f, %.2f)", blk, region_of(&reg, pos + blen / 2), cce,
           cca);
    if (nce >= 0) {
      printf("     (%.2f, %.2f)  ->  %c\n", nce, nca, route_tag(route));
    } else {
      printf("     %-13s ->  %c\n", "(skipped)", route_tag(route));
    }
  }

  /* Flush the decoders' in-flight tails. */
  bpipe->finalize(bpipe);
  mpipe->finalize(mpipe);
  {
    dt_pipe_source *bs = bpipe->source(bpipe), *ms = mpipe->source(mpipe);
    int g;
    while ((g = bs->soft_pull(bs, arec + nA, (size_t)(cap - nA))) > 0) {
      nA += g;
    }
    while ((g = ms->soft_pull(ms, brec + nB, (size_t)(cap - nB))) > 0) {
      nB += g;
    }
  }

  /* Outer recovery: reframe each decoded stream and rs251-decode its frames. */
  struct stack_result a = stack_recover(arec, nA, payA, FRAMES);
  struct stack_result b = stack_recover(brec, nB, payB, FRAMES);
  printf("\nOuter recovery (marker reframing + rs251 soft decode) of the decoded "
         "signal streams:\n");
  printf("  clean  -> bcjr   %d/%d frames delimited, %d full-length; rs251 recovered "
         "%d/%d payloads, mopping up %d inner-decoder bit error%s\n",
         a.found, FRAMES, a.full, a.recovered, FRAMES, a.residual,
         a.residual == 1 ? "" : "s");
  printf("  noisy  -> maxir  %d/%d frames delimited, %d full-length; rs251 recovered "
         "%d/%d payloads, mopping up %d inner-decoder bit error%s\n",
         b.found, FRAMES, b.full, b.recovered, FRAMES, b.residual,
         b.residual == 1 ? "" : "s");
  printf("  dead air + junk : dropped (no decoder, no frames)\n");
  printf("\ndetect_noisy ran on %d of %d blocks - only the ambiguous stretches paid "
         "for it. bcjr\ncarried the clean signal; maxir tracked the 5%% drift and rs251 "
         "mopped up its\nresidual errors (including an erasure burst). A few frames the "
         "drift shook loose\nare not recovered here - upstream of rs251 a fountain code "
         "reconstructs the whole\nmessage from the frames that do survive.\n",
         used_noisy, blk);

  dt_pipe_soft_decoder_destroy(bpipe);
  dt_pipe_soft_decoder_destroy(mpipe);
  dt_pipe_soft_decoder_destroy(pclean);
  dt_pipe_soft_decoder_destroy(pnoisy);
  dt_cc_bcjr_soft_decoder_destroy(bcjr);
  dt_cc_maxir_soft_decoder_destroy(maxir);
  dt_cc_detect_clean_soft_decoder_destroy(dclean);
  dt_cc_detect_noisy_soft_decoder_destroy(dnoisy);
  free(stream);
  free(rec);
  free(arec);
  free(brec);
  dt_cc_code_destroy(code);
  return 0;
}
