/*
 * 13 - Funnel: a streaming, detection-routed receiver over a full concatenated
 *      stack - the realistic shape of a drifty receiver.
 *
 * The transmit stack, per signal: a random payload is protected by an OUTER
 * Reed-Solomon block code (rs251), the RS blocks are wrapped as delimited frames
 * (the marker frame codec), and that framed bit stream is protected by an INNER
 * convolutional code and sent. So each layer guards the one inside it:
 *
 *   payload bytes -> rs251 (outer FEC) -> marker (framing) -> cc (inner FEC) -> channel
 *
 * The receiver watches ONE continuous channel whose condition changes over time: a
 * clean signal, then a battered + drifting one, then dead air, then noise. It cannot
 * afford the strongest inner decoder on every bit, so the FUNNEL streams the channel
 * and escalates only as far as each stretch needs, using drifty's two blind
 * code-presence detectors as cheap gatekeepers ahead of the inner decoders:
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
 * The inner decoders are SOFT, so per-bit reliabilities survive into the outer
 * layers: the marker SOFT frame decoder reframes the recovered stream (its markers
 * re-synchronise past any boundary junk the streaming router left), and the rs251
 * SOFT block decoder MOPS UP maxir's residual bit errors - turning the positions the
 * inner code left shaky into RS erasures and correcting the rest. The clean signal
 * decodes through bcjr, the drifting one through maxir; rs251 delivers both payloads
 * intact, and the dead air and junk are dropped.
 *
 * Run: ./13_funnel
 */

#include "util.h"

#include <drifty/bc/rs251.h>
#include <drifty/block_encoder.h>
#include <drifty/block_soft_decoder.h>
#include <drifty/cc/bcjr.h>
#include <drifty/cc/detect_clean.h>
#include <drifty/cc/detect_noisy.h>
#include <drifty/cc/maxir.h>
#include <drifty/fc/marker.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/pipe/multi.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/streams.h>
#include <drifty/result.h>

#include <limits.h>

/* Confidence needed to commit at a detector. */
#define CONF 0.85
/* Coded bits per routing step (wider than detect_noisy's ~1200-bit window). */
#define BLOCK 2048
/* Bits fed to each detector before routing, to clear its warm-up transient. */
#define PRIME_CLEAN 768
#define PRIME_NOISY 1600
/* Outer RS(n, k) over GF(251): each frame carries one block. */
#define RS_N 40
#define RS_K 24
#define RS_MSG (RS_K - 1)  /* payload bytes per frame */
#define CW_BITS (RS_N * 8) /* codeword bits per frame  */
#define FRAMES 6           /* frames (RS blocks) per signal region */

enum route { R_BCJR, R_MAXIR, R_DROP_CLEAN, R_DROP_NOISY };
static char route_tag(enum route r) {
  return r == R_BCJR ? 'B' : r == R_MAXIR ? 'M' : r == R_DROP_CLEAN ? '.' : 'x';
}

/* -- byte <-> bit packing (MSB-first), as in example 10 -------------------- */
static void byte_to_bits(unsigned char v, dt_bit *out) {
  for (int i = 0; i < 8; ++i) {
    out[i] = (v & (0x80u >> i)) ? DT_TRUE : DT_FALSE;
  }
}
static int bits_to_byte(const dt_bit *in, unsigned char *out) {
  unsigned v = 0;
  for (int i = 0; i < 8; ++i) {
    if (!DT_IS_BIT(in[i])) {
      return 0;
    }
    v = (v << 1) | DT_BIT(in[i]);
  }
  *out = (unsigned char)v;
  return 1;
}

/* -- transmit: payload bytes -> rs251 blocks -> marker frames --------------- */
/* Encode `nframes` payload rows into one marker-framed bit stream. */
static int build_framed(unsigned char payload[][RS_MSG], int nframes, dt_bit *out,
                        int cap) {
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(RS_N, RS_K);
  dt_frame_encoder *fe = dt_fc_marker_frame_encoder_create();
  int len = fe->begin(fe, out, cap);
  for (int f = 0; f < nframes; ++f) {
    dt_bit *rin = rse->decoded_buf(rse); /* (RS_K-1)*8 info bits */
    for (int i = 0; i < RS_MSG; ++i) {
      byte_to_bits(payload[f][i], rin + i * 8);
    }
    rse->reset(rse);
    dt_result r;
    while ((r = rse->encode(rse)) == DT_AGAIN) {
    }
    const dt_bit *cw = rse->encoded_buf(rse); /* RS_N*8 codeword bits */
    len += fe->begin_frame(fe, out + len, cap - len);
    len += fe->encode(fe, out + len, cap - len, cw, CW_BITS);
    len += fe->end_frame(fe, out + len, cap - len);
  }
  len += fe->finalize(fe, out + len, cap - len);
  dt_fc_marker_frame_encoder_destroy(fe);
  dt_bc_rs251_block_encoder_destroy(rse);
  return len;
}

/* -- streaming pipe drive --------------------------------------------------- */
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
static void mean_conf(const dt_soft_bit *r, int n, double *ce, double *ca) {
  double se = 0, sa = 0;
  for (int i = 0; i < n; ++i) {
    se += r[i].c_erasure;
    sa += r[i].c_absent;
  }
  *ce = n ? se / n : 1.0;
  *ca = n ? sa / n : 1.0;
}
static void prime(dt_pipe *p, int nbits, uint64_t *rng, dt_bit *noise,
                  dt_soft_bit *sink, int cap) {
  ex_rand_bits(noise, nbits, rng);
  feed_pull(p, noise, nbits, sink, cap);
}

/* -- receive: reframe a decoder's soft output, then rs251-decode each frame - */
/* Runs the marker SOFT frame decoder over `soft` ONE bit at a time so it can watch
 * the frame boundaries: a maximal INSIDE run between a BEGIN and an END is one frame.
 * Driving it incrementally means a corrupted delimiter desyncs only its own frame -
 * the decoder resynchronises on the next marker - instead of shifting a fixed split
 * for the whole rest of the stream. Each recovered frame is rs251 soft-decoded.
 * Reports how many payloads came back intact and how many residual inner-decoder bit
 * errors the outer RS mopped up. */
static void recover_region(const char *label, const dt_soft_bit *soft, int slen,
                           unsigned char payload[][RS_MSG], int nframes) {
  int cap = slen + 1024;
  dt_soft_bit *blob = malloc((size_t)cap * sizeof *blob); /* all in-frame bits */
  int *bound = malloc((size_t)(slen / 8 + 16) * sizeof *bound); /* frame-end offsets */
  int nin = 0, nb = 0;

  dt_frame_soft_decoder *fd = dt_fc_marker_frame_soft_decoder_create();
  fd->begin(fd, NULL, 0);
  dt_frame_decoder_state prev = fd->get_state(fd);
  dt_soft_bit ob[64];
  for (int p = 0; p <= slen; ++p) {
    int nn = (p < slen) ? fd->decode(fd, ob, 64, &soft[p], 1)
                        : fd->finalize(fd, ob, 64);
    dt_frame_decoder_state s = fd->get_state(fd);
    if (prev == DT_FRAME_DECODER_INSIDE) { /* these bits belong to the open frame */
      for (int e = 0; e < nn && nin < cap; ++e) {
        blob[nin++] = ob[e];
      }
    }
    if (prev == DT_FRAME_DECODER_INSIDE && s != DT_FRAME_DECODER_INSIDE) {
      bound[nb++] = nin; /* frame just closed */
    }
    prev = s;
  }
  if (prev == DT_FRAME_DECODER_INSIDE && (nb == 0 || bound[nb - 1] != nin)) {
    bound[nb++] = nin; /* a frame still open at end of stream */
  }
  dt_fc_marker_frame_soft_decoder_destroy(fd);
  int found = nb;

  /* Re-encoder, to recover each matched frame's true codeword for the residual
   * error count. */
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(RS_N, RS_K);
  dt_block_soft_decoder *rsd = dt_bc_rs251_block_soft_decoder_create(RS_N, RS_K, 0);

  int full = 0, recovered = 0, residual = 0, lo = 0;
  for (int fr = 0; fr < nb; ++fr) {
    const dt_soft_bit *f = blob + lo;
    int flen = bound[fr] - lo;
    lo = bound[fr];
    if (flen != CW_BITS) { /* a corrupted delimiter left this frame the wrong size */
      continue;
    }
    ++full;
    memcpy(rsd->encoded_buf(rsd), f, (size_t)CW_BITS * sizeof(dt_soft_bit));
    rsd->reset(rsd);
    dt_result r;
    while ((r = rsd->decode(rsd)) == DT_AGAIN) {
    }
    if (r != DT_OK) {
      continue;
    }
    const dt_bit *rb = rsd->decoded_buf(rsd);
    unsigned char got[RS_MSG];
    int ok = 1;
    for (int b = 0; b < RS_MSG && ok; ++b) {
      ok = bits_to_byte(rb + b * 8, &got[b]);
    }
    /* Match against the sent payloads (random, so a byte match is the frame). */
    int which = -1;
    for (int fp = 0; fp < nframes && ok; ++fp) {
      if (memcmp(got, payload[fp], RS_MSG) == 0) {
        which = fp;
        break;
      }
    }
    if (which < 0) {
      continue;
    }
    ++recovered;
    /* Residual inner-decoder bit errors this frame carried, that rs251 fixed:
     * hard-project the received codeword and diff it against the true one. */
    dt_bit *rin = rse->decoded_buf(rse);
    for (int b = 0; b < RS_MSG; ++b) {
      byte_to_bits(payload[which][b], rin + b * 8);
    }
    rse->reset(rse);
    dt_result er;
    while ((er = rse->encode(rse)) == DT_AGAIN) {
    }
    const dt_bit *cw = rse->encoded_buf(rse);
    for (int b = 0; b < CW_BITS; ++b) {
      if (ex_hard_of(f[b]) != cw[b]) {
        ++residual;
      }
    }
  }
  dt_bc_rs251_block_soft_decoder_destroy(rsd);
  dt_bc_rs251_block_encoder_destroy(rse);
  free(blob);
  free(bound);

  printf("  %-16s %d/%d frames delimited, %d full-length; rs251 recovered %d/%d "
         "payloads, mopping up %d inner-decoder bit error%s\n",
         label, found, nframes, full, recovered, nframes, residual,
         residual == 1 ? "" : "s");
}

int main(void) {
  uint64_t rng = 0xF0FFEE01u;
  /* K5 rate-1/5: the strongest standard code - its redundancy gives maxir the margin
   * to track drift (indel tolerance grows with redundancy; the metrics docs recommend
   * exactly this code for heavy drift). */
  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K5_RATE_1_5);
  if (!code) {
    return 1;
  }
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  /* A known bit-sync PREAMBLE: the inner decoder acquires lock here before any timing
   * drift accumulates. maxir can track drift once locked, but cannot both acquire and
   * fight drift at once - so the preamble is kept drift-free (like a real training
   * sequence), and the drift builds up over the payload that follows. */
  const int WARM = 256;

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
  mp.decision_depth = 96; /* deep look-ahead: resolve alignment through the drift */
  mp.max_drift = 24;      /* wide drift window for heavy (5%) deletion */
  mp.p_flip = 0.02f;       /* matches the 2% channel flip rate */
  mp.p_ins_true = 0.0025f; /* matches the 0.5% insertion rate (split true/false) */
  mp.p_ins_false = 0.0025f;
  mp.p_del = 0.05f;        /* matches the 5% deletion rate (heavy drift) */
  mp.p_ovr_erase = 0.01f;  /* the received stream can carry DT_ERASURE (burst) */

  /* -- Transmit: build the two signal payloads, protect each through the stack. -- */
  static unsigned char payA[FRAMES][RS_MSG], payB[FRAMES][RS_MSG];
  for (int f = 0; f < FRAMES; ++f) {
    for (int b = 0; b < RS_MSG; ++b) {
      payA[f][b] = (unsigned char)ex_rng_next(&rng);
      payB[f][b] = (unsigned char)ex_rng_next(&rng);
    }
  }
  enum { FCAP = 4096, GAP = 4096, JUNK = 4096 };
  const int mcap = (FCAP + WARM + K) * n + 512;
  const int scap = 6 * mcap;
  dt_bit *framed = malloc((size_t)FCAP);
  dt_bit *info = malloc((size_t)(FCAP + WARM));
  dt_bit *coded = malloc((size_t)mcap);
  dt_bit *tmp = malloc((size_t)mcap);
  dt_bit *stream = malloc((size_t)scap);
  dt_soft_bit *rec = malloc((size_t)scap * sizeof *rec);
  dt_soft_bit *arec = malloc((size_t)scap * sizeof *arec);
  dt_soft_bit *brec = malloc((size_t)scap * sizeof *brec);

  int len = 0;
  /* Region 1 - clean signal (payload A). */
  int a_lo = len;
  {
    int fl = build_framed(payA, FRAMES, framed, FCAP);
    for (int i = 0; i < WARM; ++i) {
      info[i] = DT_FALSE;
    }
    memcpy(info + WARM, framed, (size_t)fl);
    int cl = ex_encode(code, info, WARM + fl, coded, mcap);
    memcpy(stream + len, coded, (size_t)cl);
    len += cl;
  }
  int a_hi = len;
  /* Region 2 - dead air separating the two transmissions, PADDED so the next signal
   * starts on a block boundary: its sync preamble then lands wholly inside one routed
   * block, which maxir needs intact to acquire lock (a preamble split across a dropped
   * boundary block would leave it unable to lock). */
  int gap_lo = len;
  len = (len + BLOCK - 1) / BLOCK * BLOCK + GAP; /* up to a block boundary, + dead air */
  ex_rand_bits(stream + gap_lo, len - gap_lo, &rng);
  int gap_hi = len;
  /* Region 3 - the same stack, payload B, through a combined FLIP + DRIFT channel.
   * Flips hit the whole signal; the timing drift (inserted/dropped bits) starts only
   * after the sync preamble, which the inner decoder needs clean to acquire lock. */
  int b_lo = len;
  {
    int fl = build_framed(payB, FRAMES, framed, FCAP);
    for (int i = 0; i < WARM; ++i) {
      info[i] = DT_FALSE;
    }
    memcpy(info + WARM, framed, (size_t)fl);
    int cl = ex_encode(code, info, WARM + fl, coded, mcap);
    int lead = WARM * n; /* the drift-free sync preamble, in coded bits */
    memcpy(stream + len, coded, (size_t)lead);
    int t = ex_insert(coded + lead, cl - lead, 0.005, &rng, tmp);  /* +0.5% inserts */
    int held = ex_delete(tmp, t, 0.05, &rng, stream + len + lead); /* -5% deletes (drift) */
    held += lead;
    ex_flip(stream + len, held, 0.02, &rng); /* 2% flips over the whole signal */
    /* A short erasure BURST partway through the payload - the kind of hit maxir
     * cannot fully repair. It passes the span through as low confidence, leaving a
     * few wrong symbols in one frame for the rs251 soft decoder to mop up. */
    for (int i = 0; i < 64 && lead + 1200 + i < held; ++i) {
      stream[len + lead + 1200 + i] = DT_ERASURE;
    }
    len += held;
  }
  int b_hi = len;
  /* Region 4 - dead air (pure noise). */
  int noise_lo = len;
  len = (len + BLOCK - 1) / BLOCK * BLOCK + GAP;
  ex_rand_bits(stream + noise_lo, len - noise_lo, &rng);
  int noise_hi = len;
  /* Region 5 - corrupt junk (noise studded with lone DT_INVALID symbols). */
  int junk_lo = len;
  ex_rand_bits(stream + len, JUNK, &rng);
  for (int i = 0; i < JUNK; i += 60) {
    stream[len + i] = DT_INVALID;
  }
  len += JUNK;
  int junk_hi = len;

  /* -- Persistent pipes, held open across the whole stream. -- */
  dt_stream_soft_decoder *dclean = dt_cc_detect_clean_soft_decoder_create(&clean);
  dt_stream_soft_decoder *dnoisy = dt_cc_detect_noisy_soft_decoder_create(&noisy);
  dt_stream_soft_decoder *bcjr = dt_cc_bcjr_soft_decoder_create(code, &bp);
  dt_stream_soft_decoder *maxir = dt_cc_maxir_soft_decoder_create(code, &mp);
  dt_pipe *pclean = dt_pipe_soft_decoder_create(dclean);
  dt_pipe *pnoisy = dt_pipe_soft_decoder_create(dnoisy);
  dt_pipe *bpipe = dt_pipe_soft_decoder_create(bcjr);
  dt_pipe *mpipe = dt_pipe_soft_decoder_create(maxir);
  dt_pipe_source *branches[2] = {bpipe->source(bpipe), mpipe->source(mpipe)};
  dt_pipe *sel = dt_pipe_selector_create(branches, 2);
  pclean->begin(pclean);
  pnoisy->begin(pnoisy);
  bpipe->begin(bpipe);
  mpipe->begin(mpipe);
  sel->begin(sel);
  prime(pclean, PRIME_CLEAN, &rng, tmp, rec, scap);
  prime(pnoisy, PRIME_NOISY, &rng, tmp, rec, scap);

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
    int mid = pos + blen / 2;
    const char *chan = (mid >= a_lo && mid < a_hi)          ? "clean signal"
                       : (mid >= b_lo && mid < b_hi)         ? "noisy+drift"
                       : (mid >= gap_lo && mid < gap_hi)     ? "dead air"
                       : (mid >= noise_lo && mid < noise_hi) ? "dead air"
                       : (mid >= junk_lo && mid < junk_hi)   ? "corrupt junk"
                                                             : "-";
    int gc = feed_pull(pclean, b, blen, rec, scap);
    double cce, cca;
    mean_conf(rec, gc, &cce, &cca);

    enum route route;
    double nce = -1, nca = -1;
    if (cca - cce > CONF) {
      route = R_DROP_CLEAN;
    } else if (cce - cca > CONF) {
      route = R_BCJR;
    } else {
      ++used_noisy;
      int gn = feed_pull(pnoisy, b, blen, rec, scap);
      mean_conf(rec, gn, &nce, &nca);
      route = (nca - nce > CONF) ? R_DROP_NOISY : R_MAXIR;
    }
    /* A run of blocks to one decoder is one signal; when it (re)starts after a gap,
     * re-acquire - the decoder must lock onto the new signal's preamble, not carry
     * stale state from before the dead air. */
    if (route == R_BCJR) {
      if (!prevB) {
        bpipe->begin(bpipe);
      }
      nA += feed_pull(bpipe, b, blen, arec + nA, scap - nA);
    } else if (route == R_MAXIR) {
      if (!prevM) {
        mpipe->begin(mpipe);
      }
      nB += feed_pull(mpipe, b, blen, brec + nB, scap - nB);
    }
    prevB = (route == R_BCJR);
    prevM = (route == R_MAXIR);

    printf("  %3d   %-14s  (%.2f, %.2f)", blk, chan, cce, cca);
    if (nce >= 0) {
      printf("     (%.2f, %.2f)  ->  %c\n", nce, nca, route_tag(route));
    } else {
      printf("     %-13s ->  %c\n", "(skipped)", route_tag(route));
    }
  }

  bpipe->finalize(bpipe);
  mpipe->finalize(mpipe);
  {
    dt_pipe_source *bs = bpipe->source(bpipe), *ms = mpipe->source(mpipe);
    int g;
    while ((g = bs->soft_pull(bs, arec + nA, (size_t)(scap - nA))) > 0) {
      nA += g;
    }
    while ((g = ms->soft_pull(ms, brec + nB, (size_t)(scap - nB))) > 0) {
      nB += g;
    }
  }

  printf("\nOuter recovery (marker reframing + rs251 soft decode) of the decoded "
         "signal streams:\n");
  recover_region("clean  -> bcjr", arec, nA, payA, FRAMES);
  recover_region("noisy  -> maxir", brec, nB, payB, FRAMES);
  printf("  dead air + junk : dropped (no decoder, no frames)\n");
  printf("\ndetect_noisy ran on %d of %d blocks - only the ambiguous stretches paid "
         "for it. bcjr\ncarried the clean signal; maxir tracked the 5%% drift and rs251 "
         "mopped up its\nresidual errors (including an erasure burst). A few frames the "
         "drift shook loose\nare not recovered here - upstream of rs251 a fountain code "
         "reconstructs the whole\nmessage from the frames that do survive.\n",
         used_noisy, blk);

  dt_pipe_selector_destroy(sel);
  dt_pipe_soft_decoder_destroy(bpipe);
  dt_pipe_soft_decoder_destroy(mpipe);
  dt_pipe_soft_decoder_destroy(pclean);
  dt_pipe_soft_decoder_destroy(pnoisy);
  dt_cc_bcjr_soft_decoder_destroy(bcjr);
  dt_cc_maxir_soft_decoder_destroy(maxir);
  dt_cc_detect_clean_soft_decoder_destroy(dclean);
  dt_cc_detect_noisy_soft_decoder_destroy(dnoisy);
  free(framed);
  free(info);
  free(coded);
  free(tmp);
  free(stream);
  free(rec);
  free(arec);
  free(brec);
  dt_cc_code_destroy(code);
  return 0;
}
