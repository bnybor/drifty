/* clang-format off */
/*
 * MIT License
 *
 * Copyright (c) 2026 Robyn Kirkman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* clang-format on */

/*
 * detect_clean decode engine - blind detection of convolutional-code structure in an
 * arbitrary bit stream, with no prior knowledge or coordination (no code, rate,
 * generators, or alignment).
 *
 * Method - GF(2) strided-window rank deficiency, over SLIDING windows. A
 * convolutional code is linear and time-invariant, so its output bits satisfy
 * parity-check relations: a width-W window of coded bits lives in a proper GF(2)
 * subspace, so a matrix built from such windows is rank-DEFICIENT, while random
 * bits span the full space (full rank). The checks are PHASE-specific (they relate
 * output bits at a fixed position mod n, the block size n), so the window rows must
 * be stacked at a stride equal to n to stay phase-aligned. n is unknown, so we
 * sweep candidate strides s = 2..DET_SMAX.
 *
 * Crucially the windows SLIDE (we do not take one fixed-origin matrix over a whole
 * block). Each width-W row that spans an insertion/deletion - or strays out of a
 * coded region into random bits - is linearly independent of the code's subspace,
 * so it fills the rank and erases the deficiency. A matrix is therefore deficient
 * only when ALL its rows lie inside one indel-free, phase-aligned coded run. By
 * sliding a short window of length L_s = s*(W + DET_MARGIN) + W (chosen so every
 * stride yields the same row count N = W + DET_MARGIN + 1, hence the same random
 * rejection 2^-(N-W)) and assigning each stream position the MAX deficiency over
 * the windows covering it, a code is detected wherever a clean aligned run exists -
 * tolerating sparse indels (an indel only kills the windows that span it, not the
 * runs between them). A window that straddles a code/random boundary has random
 * rows, so it reads d = 0; localization stays sharp.
 *
 *   d = 0  -> no structure here -> "no code" (c_absent high)
 *   d > 0  -> parity checks found -> "code present" (c_lost = 1 - 2^-d)
 *
 * One record is produced per input position (the per-position max deficiency);
 * output trails input by up to one longest window (DET_MAXL).
 *
 * LIMITATIONS (see also doc/cc/detect.md):
 *  - Noise: a single flipped bit is an independent row in every window that covers
 *    it, breaking that window's deficiency. Indels are tolerated (the runs between
 *    them are clean), but FLIPS are not - this targets the clean / very-low-noise
 *    regime: it holds to ~1% flips and to ~2-3% indels.
 *  - Scope: rank deficiency senses LINEAR structure in general - a block linear
 *    code or an LFSR scrambler would also register. For the intended use (a stream
 *    is either uncoded/random or convolutionally coded) this is the right proxy.
 *  - Codes with block size n > DET_SMAX are not covered by the stride sweep.
 */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h> /* dt_malloc / dt_free / dt_realloc / dt_memmove / dt_exp / dt_log */

/* Detector geometry. W must fit a uint64_t and exceed a code's parity span
 * (~2*K bits for the standard K<=7 codes). Each stride s uses a window of length
 * s*(W+MARGIN)+W so every stride has N = W+MARGIN+1 rows (uniform random
 * rejection ~2^-(MARGIN+1)). Windows slide by STEP. Validated empirically. */
#define DET_W 32       /* GF(2) window width, bits */
#define DET_SMAX 6     /* sweep strides 2..DET_SMAX (block sizes n in 2..6) */
#define DET_MARGIN 18  /* extra rows per stride window beyond W */
#define DET_STEP 32    /* window slide granularity, bits */
#define DET_LWIN(s) ((s) * (DET_W + DET_MARGIN) + DET_W) /* window length, stride s */
#define DET_MAXL DET_LWIN(DET_SMAX) /* longest window (s = SMAX): 332 */
#define DET_MINL DET_LWIN(2)        /* shortest window (s = 2): 132   */

#define DET_LN2 0.69314718055994531f

/* Sentinel stored in `in[]` for a non-bit (erasure / invalid / absent) position:
 * any value > 1, so it can never be mistaken for a 0/1 payload. A window row that
 * contains one is a don't-know and is dropped from the rank matrix rather than
 * silently coerced to 0 (which would fake the all-zeros maximally-deficient row). */
#define DET_NOTBIT 2u

struct dt_cc_detect_clean_stream_decoder {
  /* Input bit FIFO (0/1 values, or DET_NOTBIT for a non-bit position) and two
   * parallel per-position pools: dmax = best structural deficiency so far, cmax =
   * largest usable row count of any covering window (for the fill margin). Both
   * signed char: -1 = position not yet covered by any window with a usable row.
   * All share head/len/cap; in[head + k] is absolute position `base + k`. */
  unsigned char *in;
  signed char *dmax;
  signed char *cmax;
  int head, len, cap;
  long long base;       /* absolute index of in[head]              */
  long long next_start; /* absolute index of next window start (multiple of STEP) */
  /* Pending output FIFO of per-position records, drained from the front. */
  dt_cc_detect_clean_decode_details *out;
  int out_head, out_len, out_cap;
  int finalized; /* the final tail has been processed */
  /* (1 - expected per-bit FLIP/overwrite corruption)^DET_W, from the channel
   * model: the chance a width-W window survives the expected flip noise intact,
   * hence the chance a code (if present) would still show as rank deficiency. It
   * scales the confidence of a "no code" verdict - a flip-noisy channel cannot
   * confidently rule a code out. Indels are NOT folded in: the sliding method
   * tolerates them, so they should not discount the no-code confidence. */
  float detectability;
};

/* 2^-x for x >= 0, via the freestanding single-precision exp proxy. */
static float pow2_neg(int x) {
  if (x <= 0) {
    return 1.0f;
  }
  if (x >= 64) {
    return 0.0f;
  }
  return dt_exp(-(float)x * DET_LN2);
}

/* Index of the most significant set bit of x (x != 0), 0..63. Portable - no
 * compiler builtins (the core is built -fno-builtin). */
static int msb64(uint64_t x) {
  int b = 0;
  while (x >>= 1) {
    ++b;
  }
  return b;
}

/* GF(2) rank of the matrix whose rows are the DET_W-bit windows of win[0..count)
 * taken at offsets 0, stride, 2*stride, ... (online Gaussian elimination against
 * an MSB-indexed pivot table). A row containing a non-bit (DET_NOTBIT, e.g. an
 * erasure) is a don't-know: it is dropped rather than constraining the space, and
 * the count of usable (dropped-free) rows is returned via *nrows. */
static int gf2_rank(const unsigned char *win, int count, int stride, int *nrows) {
  uint64_t pivot[DET_W];
  for (int i = 0; i < DET_W; ++i) {
    pivot[i] = 0;
  }
  int rank = 0, used = 0;
  for (int start = 0; start + DET_W <= count; start += stride) {
    uint64_t v = 0;
    int ok = 1;
    for (int j = 0; j < DET_W; ++j) {
      const unsigned char b = win[start + j];
      if (b > 1u) { /* non-bit: this row is a don't-know, skip it */
        ok = 0;
        break;
      }
      v = (v << 1) | (uint64_t)b;
    }
    if (!ok) {
      continue;
    }
    ++used;
    while (v) {
      const int b = msb64(v);
      if (pivot[b]) {
        v ^= pivot[b];
      } else {
        pivot[b] = v;
        ++rank;
        break;
      }
    }
  }
  *nrows = used;
  return rank;
}

/* Map a position's pooled evidence to TWO INDEPENDENT consistencies (not a split):
 *   c_lost   = consistency with "a code IS present"  (rides out as c_erasure)
 *   c_absent = consistency with "no code / random"
 * Each is a goodness-of-fit - how well the data fails to contradict that hypothesis
 * - so they need NOT sum to 1, and the no-evidence state is (1, 1), not (0, 0):
 * with nothing to judge, both hypotheses remain un-contradicted.
 *   d   = pooled structural deficiency min(n_eff, W) - rank (>= 0), or -1 if the
 *         position was never covered by a window with a usable row.
 *   cov = largest usable row count of any covering window (sets the fill margin).
 * A structural deficiency contradicts random (lowers c_absent) and is real whatever
 * the channel. A genuinely full rank contradicts a code (lowers c_lost) - but only
 * insofar as the fill is confirmed (margin) and the channel is clean enough that a
 * real code's parity could not have been flipped into full rank (detectability). */
static dt_cc_detect_clean_decode_details verdict_from(int d, int cov,
                                                      float detectability) {
  dt_cc_detect_clean_decode_details det;
  if (d < 0) {
    /* No usable window ever covered this position (leading/trailing tail, or an
     * all-non-bit run such as a stream of erasures): nothing discriminates the two
     * hypotheses, so both stay fully consistent. */
    det.c_lost = 1.0f;
    det.c_absent = 1.0f;
    return det;
  }
  if (d >= 1) {
    /* Structure found: a code fits (c_lost = 1); random would need a fluke of
     * ~2^-d to produce this deficiency, and that is true regardless of expected
     * noise (noise erodes structure, it never manufactures it). */
    det.c_lost = 1.0f;
    det.c_absent = pow2_neg(d);
    return det;
  }
  /* d == 0: no structure. The absence of structure never contradicts random, so
   * c_absent stays 1. A code is ruled out only to the extent the space was surely
   * filled (margin rows beyond W) AND a flip-noisy channel could not have filled it
   * by breaking a real code's parity (detectability). A thinly covered window
   * (margin < 0) confirms no fill, so it rules nothing out: c_lost stays 1. */
  const int margin = cov - DET_W;
  const float fillconf = (margin >= 0) ? (1.0f - pow2_neg(margin + 1)) : 0.0f;
  det.c_lost = 1.0f - detectability * fillconf;
  det.c_absent = 1.0f;
  return det;
}

/* -- growable buffers ------------------------------------------------------ */

/* Grow the input/dmax FIFO to hold `extra` more positions, compacting first. */
static int buf_reserve(dt_cc_detect_clean_stream_decoder *d, int extra) {
  if (d->head > 0 && d->len + extra > d->cap) {
    dt_memmove(d->in, d->in + d->head, (size_t)(d->len - d->head));
    dt_memmove(d->dmax, d->dmax + d->head, (size_t)(d->len - d->head));
    dt_memmove(d->cmax, d->cmax + d->head, (size_t)(d->len - d->head));
    d->len -= d->head;
    d->head = 0;
  }
  if (d->len + extra > d->cap) {
    int nc = d->cap ? d->cap * 2 : 1024;
    while (nc < d->len + extra) {
      nc *= 2;
    }
    unsigned char *ni = dt_realloc(d->in, (size_t)nc);
    if (!ni) {
      return 0;
    }
    d->in = ni;
    signed char *nd = dt_realloc(d->dmax, (size_t)nc);
    if (!nd) {
      return 0;
    }
    d->dmax = nd;
    signed char *ncm = dt_realloc(d->cmax, (size_t)nc);
    if (!ncm) {
      return 0;
    }
    d->cmax = ncm;
    d->cap = nc;
  }
  return 1;
}

static int out_reserve(dt_cc_detect_clean_stream_decoder *d, int extra) {
  if (d->out_head > 0 && d->out_len + extra > d->out_cap) {
    dt_memmove(d->out, d->out + d->out_head,
               (size_t)(d->out_len - d->out_head) * sizeof(*d->out));
    d->out_len -= d->out_head;
    d->out_head = 0;
  }
  if (d->out_len + extra > d->out_cap) {
    int nc = d->out_cap ? d->out_cap * 2 : 512;
    while (nc < d->out_len + extra) {
      nc *= 2;
    }
    dt_cc_detect_clean_decode_details *nb =
        dt_realloc(d->out, (size_t)nc * sizeof(*nb));
    if (!nb) {
      return 0;
    }
    d->out = nb;
    d->out_cap = nc;
  }
  return 1;
}

/* -- sliding-window processing --------------------------------------------- */

/* Process the windows that start at absolute index `start` (one per stride that
 * fits the buffer), folding each window's deficiency into dmax over the positions
 * it covers. A window of length L_s straddling an indel or a code/random boundary
 * contains independent rows, so it reads d = 0 there - only a fully-clean aligned
 * run shows d > 0, which keeps localization sharp. */
static void process_start(dt_cc_detect_clean_stream_decoder *d, long long start) {
  const long long bend = d->base + (d->len - d->head);
  const int lo = d->head + (int)(start - d->base);
  for (int s = 2; s <= DET_SMAX; ++s) {
    const int L = DET_LWIN(s);
    if (start + L > bend) {
      continue; /* this stride's window does not fit the buffer */
    }
    int n_eff = 0;
    const int rank = gf2_rank(d->in + lo, L, s, &n_eff);
    if (n_eff <= 0) {
      continue; /* every row was a don't-know (e.g. erasures): no evidence to fold */
    }
    const int fill = (n_eff < DET_W) ? n_eff : DET_W; /* max attainable rank */
    int def = fill - rank;                            /* structural deficiency */
    if (def < 0) {
      def = 0;
    }
    for (int j = 0; j < L; ++j) { /* def>=0 / n_eff>=1 also mark "covered" (-1 -> .) */
      if ((int)d->dmax[lo + j] < def) {
        d->dmax[lo + j] = (signed char)def;
      }
      if ((int)d->cmax[lo + j] < n_eff) {
        d->cmax[lo + j] = (signed char)n_eff;
      }
    }
  }
}

/* Emit (finalize) every position in [base, upto): its dmax is settled because all
 * window starts <= position have been processed. */
static int emit_upto(dt_cc_detect_clean_stream_decoder *d, long long upto) {
  int count = (int)(upto - d->base);
  if (count <= 0) {
    return 1;
  }
  if (!out_reserve(d, count)) {
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    d->out[d->out_len++] = verdict_from((int)d->dmax[d->head],
                                        (int)d->cmax[d->head], d->detectability);
    ++d->head;
    ++d->base;
  }
  return 1;
}

static int drain(dt_cc_detect_clean_stream_decoder *d,
                 dt_cc_detect_clean_decode_details *details, int max_out) {
  int w = 0;
  while (w < max_out && d->out_head < d->out_len) {
    if (details) {
      details[w] = d->out[d->out_head];
    }
    ++d->out_head;
    ++w;
  }
  if (d->out_head == d->out_len) {
    d->out_head = d->out_len = 0; /* fully drained: reset to front */
  }
  return w;
}

/* -- public engine API ----------------------------------------------------- */

/* detectability = (1 - p)^DET_W, p = expected per-bit FLIP/overwrite corruption.
 * Indels (p_ins / p_del) are deliberately excluded - the sliding method tolerates
 * them, so they must not discount the no-code confidence. */
static float detectability_from(const dt_cc_detect_clean_stream_params *p) {
  const float p_ovr = p->p_ovr_true + p->p_ovr_false + p->p_ovr_erase;
  float p_corrupt = p_ovr + (1.0f - p_ovr) * p->p_flip;
  if (p_corrupt <= 0.0f) {
    return 1.0f; /* clean channel: a code would show through fully */
  }
  if (p_corrupt > 0.999f) {
    p_corrupt = 0.999f;
  }
  return dt_exp((float)DET_W * dt_log(1.0f - p_corrupt));
}

dt_cc_detect_clean_stream_decoder *dt_cc_detect_clean_stream_decoder_create(
    const dt_cc_detect_clean_stream_params *params) {
  if (!params) {
    return NULL;
  }
  /* Validate the channel model. p_flip may be 0 (expect a clean channel); the
   * rates are non-negative and the overwrite / indel families each sum to < 1.
   * decision_depth and max_drift are accepted for interface uniformity (the rank
   * method does not use them - indel tolerance is intrinsic, not a drift window). */
  if (params->decision_depth < 1 || params->max_drift < 0) {
    return NULL;
  }
  if (params->p_flip < 0.0f || params->p_flip >= 1.0f || params->p_del < 0.0f ||
      params->p_ins_true < 0.0f || params->p_ins_false < 0.0f ||
      params->p_ins_erase < 0.0f || params->p_ovr_true < 0.0f ||
      params->p_ovr_false < 0.0f || params->p_ovr_erase < 0.0f) {
    return NULL;
  }
  if (!(params->p_ovr_true + params->p_ovr_false + params->p_ovr_erase < 1.0f) ||
      !(params->p_ins_true + params->p_ins_false + params->p_ins_erase +
            params->p_del <
        1.0f)) {
    return NULL;
  }
  dt_cc_detect_clean_stream_decoder *d = dt_malloc(sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->in = NULL;
  d->dmax = NULL;
  d->cmax = NULL;
  d->head = d->len = d->cap = 0;
  d->base = 0;
  d->next_start = 0;
  d->out = NULL;
  d->out_head = d->out_len = d->out_cap = 0;
  d->finalized = 0;
  d->detectability = detectability_from(params);
  return d;
}

void dt_cc_detect_clean_stream_decoder_destroy(dt_cc_detect_clean_stream_decoder *d) {
  if (!d) {
    return;
  }
  dt_free(d->in);
  dt_free(d->dmax);
  dt_free(d->cmax);
  dt_free(d->out);
  dt_free(d);
}

int dt_cc_detect_clean_stream_decode(dt_cc_detect_clean_stream_decoder *d, const uint8_t *in,
                            int n_in, dt_cc_detect_clean_decode_details *details,
                            int max_out) {
  if (!d || (n_in > 0 && !in) || n_in < 0 || max_out < 0) {
    return DT_ERR_ARG;
  }
  if (n_in > 0) {
    if (!buf_reserve(d, n_in)) {
      return DT_ERR_ALLOC;
    }
    for (int i = 0; i < n_in; ++i) {
      d->in[d->len] =
          DT_IS_BIT(in[i]) ? (unsigned char)DT_BIT(in[i]) : DET_NOTBIT;
      d->dmax[d->len] = -1; /* not yet covered */
      d->cmax[d->len] = -1;
      ++d->len;
    }
  }
  /* Process every window whose full stride sweep is now buffered, then finalize
   * the positions those windows have settled (everything below next_start). */
  const long long bend = d->base + (d->len - d->head);
  while (d->next_start + DET_MAXL <= bend) {
    process_start(d, d->next_start);
    d->next_start += DET_STEP;
  }
  if (!emit_upto(d, d->next_start)) {
    return DT_ERR_ALLOC;
  }
  return drain(d, details, max_out);
}

int dt_cc_detect_clean_stream_decode_flush(dt_cc_detect_clean_stream_decoder *d,
                                 dt_cc_detect_clean_decode_details *details,
                                 int max_out) {
  if (!d || max_out < 0) {
    return DT_ERR_ARG;
  }
  if (!d->finalized) {
    const long long bend = d->base + (d->len - d->head);
    /* Full-fit windows, then the tail's partial-fit windows (process_start skips
     * the strides whose window no longer fits). */
    while (d->next_start + DET_MAXL <= bend) {
      process_start(d, d->next_start);
      d->next_start += DET_STEP;
    }
    while (d->next_start + DET_MINL <= bend) {
      process_start(d, d->next_start);
      d->next_start += DET_STEP;
    }
    d->finalized = 1;
    if (!emit_upto(d, bend)) { /* emit all remaining positions */
      return DT_ERR_ALLOC;
    }
  }
  return drain(d, details, max_out);
}
