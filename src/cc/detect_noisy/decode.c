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
 * detect_noisy decode engine - blind detection of convolutional-code structure in an
 * arbitrary bit stream, with no prior knowledge or coordination (no code, rate,
 * generators, or alignment), tolerant of flips, indels, and combinations of them.
 *
 * Method - PARITY-CHECK BIAS scored by a fast Walsh-Hadamard transform, over
 * SLIDING windows. A convolutional code is linear, so it has low-weight parity
 * checks: a binary vector c with c . (window of coded bits) = 0 for every phase-
 * aligned window. Under a per-bit flip rate p, such a check still holds with
 * probability (1 + (1 - 2p)^w)/2 (w = its weight), so its BIAS
 *   beta(c) = | E[ (-1)^(c . row) ] |  ~=  (1 - 2p)^w  >  0,
 * while for random data every c gives beta ~= 0. The detector's statistic is the
 * MAX bias over all candidate checks c. Crucially, a flip only SHRINKS (1-2p)^w
 * rather than destroying the check (contrast detect_clean's exact GF(2) rank, which
 * a single flip breaks) - so the bias degrades GRACEFULLY with flips, with indels
 * (rows after a slip fall out of phase and merely stop contributing bias, the
 * aligned rows still bias), and with combinations of the two.
 *
 * Computing the max bias over all 2^L_c checks c at once is exactly a Walsh-
 * Hadamard transform: histogram the L_c-bit row-slices of a window, transform in
 * place, and the largest |coefficient| (over c != 0) divided by the row count is
 * the max bias. The checks are PHASE-specific (they relate output bits at a fixed
 * position mod n, the block size n), so the rows are taken at a candidate STRIDE
 * s = n to stay phase-aligned; n is unknown, so we sweep s = 2..DET_SMAX.
 *
 * Windows SLIDE (length DET_L, step DET_STEP) and each stream position is assigned
 * the MAX evidence over the windows covering it, so a code is detected wherever a
 * sufficiently-aligned run exists and localization stays reasonably sharp. The raw
 * per-window-per-stride evidence is the EXCESS of the bias over the random floor,
 *   excess = beta / f0 - 1,   f0(N) = sqrt(2 * L_c * ln2 / N)  (N = rows at stride s),
 * f0 being the expected max bias of N random rows over 2^L_c candidates; excess ~= 0
 * for random, large for a clean code. The per-position max excess maps to two
 * INDEPENDENT consistency reads (see verdict_from):
 *   c_lost  = 1 - detectability*(1 - clamp(excess/DET_K_LOST,0,1))  (consistency with a code)
 *   c_absent = clamp(1 - excess, 0, 1)                              (consistency with random)
 * with (1, 1) where no window scored (the tail, or an all-non-bit run).
 *
 * One record is produced per input position; output trails input by up to one
 * window (DET_L).
 *
 * LIMITATIONS (see also doc/cc/detect_noisy.md):
 *  - Noise envelope: graceful but bounded. Reliable to ~5% flips (marginal to ~8%)
 *    and ~2-3% indels, plus light-moderate COMBINATIONS; heavy simultaneous
 *    flips+indels stay out of reach (fundamental to the underlying LPN + sync
 *    problem). For a clean / very-low-noise channel where footprint matters, the
 *    cheaper detect_clean is preferable.
 *  - Scope: parity-check bias senses LINEAR structure in general - a block linear
 *    code or an LFSR scrambler would also register. For the intended use (a stream
 *    is either uncoded/random or convolutionally coded) this is the right proxy.
 *  - Reach: checks longer than L_c bits, or codes with block size n > DET_SMAX, are
 *    not covered (this caps the detectable constraint length / rate).
 *  - Cost: one 2^L_c-entry histogram (~64 KB) and a Walsh-Hadamard transform per
 *    window-stride - roughly one to two orders more work per bit than detect_clean.
 */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h> /* dt_malloc / dt_free / dt_realloc / dt_memmove / dt_memset / dt_exp / dt_log */

/* Detector geometry. L_c is the parity-check span in bits (>= ~2*K for the standard
 * K<=7 rate-1/2 codes); the transform histogram has 2^L_c entries. Strides 2..SMAX
 * are the candidate block sizes n. Windows of DET_L bits slide by DET_STEP; the tail
 * is scored down to DET_MINL. Validated empirically (see doc/cc/detect_noisy.md). */
#define DET_LC 14    /* parity-check span / transform order, bits (2^14 histogram) */
#define DET_SMAX 6   /* sweep strides 2..DET_SMAX (block sizes n in 2..6)          */
#define DET_L 1200   /* analysis window length, bits                              */
#define DET_STEP 300 /* window slide granularity, bits (DET_L / 4)                */
#define DET_MINL 256 /* shortest tail window still scored, bits                   */
#define DET_M (1 << DET_LC) /* transform size / histogram entries                 */

#define DET_K_LOST 2.0f /* excess at which c_lost saturates to 1 (calibration)    */
#define DET_WREF 7      /* representative check weight, for the detectability factor */
#define DET_LN2 0.69314718055994531f
#define DET_FLOOR_C (2.0f * (float)DET_LC * DET_LN2) /* numerator of f0^2 * N      */
#define DET_UNCOVERED (-1.0e30f) /* per-position sentinel: no window scored it yet */

/* Sentinels stored in `in[]` for the two kinds of non-bit position; both are > 1,
 * and a transform slice containing either is skipped as a don't-know rather than
 * coerced to 0 (which would fake an all-zeros delta whose Walsh transform is flat
 * and reads as maximal bias). They are kept DISTINCT for the present axis:
 *   DET_NOTBIT - an UNBOUND non-bit (DT_ERASURE / DT_ABSENT / DT_NONE): neutral.
 *   DET_INVAL  - a BOUND non-boolean (DT_INVALID): off both the codeword and the
 *                random-boolean manifold, so its PLACEMENT is present-axis evidence
 *                (see invalid_units). */
#define DET_NOTBIT 2u
#define DET_INVAL  3u

/* Each unit of invalid-placement penalty damps c_lost by 2^-DET_INV_BITS = 1/4.
 * Soft: channel invalids are unmodeled but not impossible, so this is strong
 * present-axis EVIDENCE, never proof. */
#define DET_INV_BITS 2
#define DET_INV_MAXRUNS 64
#define DET_INV_MAXUNITS 32

/* 2^-x for x >= 0 (single-precision exp proxy), for the invalid-placement damping. */
static float pow2_neg(int x) {
  if (x <= 0) {
    return 1.0f;
  }
  if (x >= 64) {
    return 0.0f;
  }
  return dt_exp(-(float)x * DET_LN2);
}

/* Penalty units from the PLACEMENT of DT_INVAL symbols over win[0..count): present-
 * axis evidence that the invalid pattern is not one convolutional code's output.
 * Singletons (length-1 runs) are an un-encodable shape (a single invalid input
 * smears into a generator cluster, never a lone symbol); runs of DIFFERING length
 * impose independent trellis-state offsets a single code must satisfy jointly
 * (distinct_lengths - 1). A single run, or equal-length runs, contradict nothing.
 * Unbound non-bits (DET_NOTBIT, e.g. erasures) are NOT invalids and never count. */
static int invalid_units(const unsigned char *win, int count) {
  int lengths[DET_INV_MAXRUNS];
  int nruns = 0, singles = 0;
  for (int i = 0; i < count;) {
    if (win[i] != DET_INVAL) {
      ++i;
      continue;
    }
    int run = 0;
    while (i < count && win[i] == DET_INVAL) {
      ++run;
      ++i;
    }
    if (run == 1) {
      ++singles;
    }
    if (nruns < DET_INV_MAXRUNS) {
      lengths[nruns++] = run;
    }
  }
  if (nruns == 0) {
    return 0;
  }
  int distinct = 0;
  for (int a = 0; a < nruns; ++a) {
    int seen = 0;
    for (int b = 0; b < a; ++b) {
      if (lengths[b] == lengths[a]) {
        seen = 1;
        break;
      }
    }
    distinct += !seen;
  }
  int units = singles + (distinct > 1 ? distinct - 1 : 0);
  return units > DET_INV_MAXUNITS ? DET_INV_MAXUNITS : units;
}

struct dt_cc_detect_noisy_stream_decoder {
  /* Input bit FIFO (0/1, or DET_NOTBIT / DET_INVAL for a non-bit position) and two
   * parallel per-position pools: emax = best bias excess so far (float; DET_UNCOVERED
   * = not yet covered), imax = worst invalid-placement penalty units of any covering
   * window (signed char; 0 = none, present-axis only). All share head/len/cap;
   * in[head + k] is absolute position `base + k`. */
  unsigned char *in;
  float *emax;
  signed char *imax;
  int head, len, cap;
  long long base;       /* absolute index of in[head]              */
  long long next_start; /* absolute index of next window start (multiple of STEP) */
  /* Pending output FIFO of per-position records, drained from the front. */
  dt_cc_detect_noisy_decode_details *out;
  int out_head, out_len, out_cap;
  int finalized; /* the final tail has been processed */
  /* 2^DET_LC-entry Walsh-Hadamard histogram, allocated once and reused across every
   * window and stride (cleared per use). ~64 KB. */
  int32_t *acc;
  /* (1 - 2p)^DET_WREF, p = expected per-bit FLIP/overwrite corruption, from the
   * channel model: roughly the bias a representative check would retain under the
   * expected flips, i.e. how visible a code would still be. It scales how far a
   * no-peak window rules a code OUT - a flip-noisy channel cannot, so c_erasure is
   * held up there (the flips could have eroded a real code's bias into the floor).
   * Indels are NOT folded in: the sliding method tolerates them. */
  float detectability;
};

/* The random-data floor f0(n) = sqrt(2 * L_c * ln2 / n): the expected max bias of n
 * random rows scored over 2^L_c candidate checks. Computed from the ACTUAL row count
 * so short tail windows get their (higher) floor. n >= 1. No dt_sqrt in the
 * freestanding shim, so sqrt(x) = exp(0.5 * log(x)). */
static float floor_bias(int n) {
  return dt_exp(0.5f * dt_log(DET_FLOOR_C / (float)n));
}

/* In-place fast Walsh-Hadamard transform of acc[0 .. DET_M). */
static void fwht(int32_t *acc) {
  for (int len = 1; len < DET_M; len <<= 1) {
    for (int i = 0; i < DET_M; i += len << 1) {
      for (int j = i; j < i + len; ++j) {
        const int32_t a = acc[j];
        const int32_t b = acc[j + len];
        acc[j] = a + b;
        acc[j + len] = a - b;
      }
    }
  }
}

/* Max parity-check bias over a window: histogram the DET_LC-bit slices of
 * win[0 .. span) taken at row stride `stride` (rows at 0, stride, 2*stride, ...),
 * transform, and return the largest |coefficient| over c != 0 divided by the row
 * count - i.e. max over all 2^DET_LC checks c of |E[(-1)^(c . row)]|. Writes the row
 * count to *nrows. Uses (and clobbers) d->acc. */
static float window_max_bias(dt_cc_detect_noisy_stream_decoder *d,
                             const unsigned char *win, int span, int stride,
                             int *nrows) {
  dt_memset(d->acc, 0, (size_t)DET_M * sizeof(*d->acc));
  int n = 0;
  for (int t = 0; t + DET_LC <= span; t += stride) {
    uint32_t v = 0;
    int ok = 1;
    for (int j = 0; j < DET_LC; ++j) {
      const unsigned char b = win[t + j];
      if (b > 1u) { /* non-bit: this slice is a don't-know, skip it */
        ok = 0;
        break;
      }
      v = (v << 1) | (uint32_t)b;
    }
    if (!ok) {
      continue;
    }
    ++d->acc[v];
    ++n;
  }
  *nrows = n;
  if (n <= 0) {
    return 0.0f;
  }
  fwht(d->acc);
  int32_t best = 0; /* skip c = 0 (the DC term, always == n) */
  for (int c = 1; c < DET_M; ++c) {
    int32_t a = d->acc[c] < 0 ? -d->acc[c] : d->acc[c];
    if (a > best) {
      best = a;
    }
  }
  return (float)best / (float)n;
}

/* Evidence that the window win[0 .. span) carries a code: the MAX over candidate
 * strides s = 2..SMAX of the bias excess over that stride's random floor,
 * excess = beta / f0(n_s) - 1 (~= 0 for random, large for an aligned code). */
static float window_excess(dt_cc_detect_noisy_stream_decoder *d,
                           const unsigned char *win, int span) {
  float best = DET_UNCOVERED;
  for (int s = 2; s <= DET_SMAX; ++s) {
    int n = 0;
    const float beta = window_max_bias(d, win, span, s, &n);
    if (n < 1) {
      continue;
    }
    const float ex = beta / floor_bias(n) - 1.0f;
    if (ex > best) {
      best = ex;
    }
  }
  return best;
}

/* Map a position's pooled max excess to TWO INDEPENDENT consistencies (not a
 * split): c_lost = consistency with "a code IS present" (rides out as c_erasure),
 * c_absent = consistency with "no code / random". Each is a goodness-of-fit, so
 * they need NOT sum to 1, and the no-evidence state is (1, 1): with nothing to
 * judge, both hypotheses remain un-contradicted. */
static dt_cc_detect_noisy_decode_details verdict_from(float excess, int iunits,
                                                     float detectability) {
  dt_cc_detect_noisy_decode_details det;
  if (excess <= DET_UNCOVERED * 0.5f) {
    /* No window scored this position (leading/trailing tail, or every row was a
     * don't-know such as a stream of erasures - or of invalids, which form a single
     * run and so add no penalty): nothing discriminates the two hypotheses. */
    det.c_lost = 1.0f;
    det.c_absent = 1.0f;
    return det;
  }
  /* present-evidence: how far the peak bias clears the random floor. */
  float p_ev = excess / DET_K_LOST;
  p_ev = p_ev < 0.0f ? 0.0f : (p_ev > 1.0f ? 1.0f : p_ev);
  /* c_lost = consistency with a code: a clear peak fits one outright; the absence
   * of a peak rules a code out only insofar as the channel is clean enough that a
   * real code's bias would have survived - a flip-noisy channel could have eroded
   * it into the floor (1 - detectability stays plausible). */
  det.c_lost = 1.0f - detectability * (1.0f - p_ev);
  /* c_absent = consistency with random: a peak contradicts random; its absence
   * never does. An observed peak is real whatever the channel, so detectability
   * does NOT scale this (contrast the old (1 - excess) * detectability, which
   * damped the wrong axis and coupled the two). */
  float ca = 1.0f - excess;
  ca = ca < 0.0f ? 0.0f : (ca > 1.0f ? 1.0f : ca);
  det.c_absent = ca;
  /* Invalid placement is present-axis-only evidence: its pattern can contradict a
   * code (damp c_lost) but an invalid is off the random-boolean manifold too, so it
   * never raises c_absent. Soft (channel invalids are unmodeled, not impossible). */
  if (iunits > 0) {
    det.c_lost *= pow2_neg(DET_INV_BITS * iunits);
  }
  return det;
}

/* -- growable buffers ------------------------------------------------------ */

/* Grow the input/emax FIFO to hold `extra` more positions, compacting first. */
static int buf_reserve(dt_cc_detect_noisy_stream_decoder *d, int extra) {
  if (d->head > 0 && d->len + extra > d->cap) {
    dt_memmove(d->in, d->in + d->head, (size_t)(d->len - d->head));
    dt_memmove(d->emax, d->emax + d->head,
               (size_t)(d->len - d->head) * sizeof(*d->emax));
    dt_memmove(d->imax, d->imax + d->head, (size_t)(d->len - d->head));
    d->len -= d->head;
    d->head = 0;
  }
  if (d->len + extra > d->cap) {
    int nc = d->cap ? d->cap * 2 : 2048;
    while (nc < d->len + extra) {
      nc *= 2;
    }
    unsigned char *ni = dt_realloc(d->in, (size_t)nc);
    if (!ni) {
      return 0;
    }
    d->in = ni;
    float *ne = dt_realloc(d->emax, (size_t)nc * sizeof(*ne));
    if (!ne) {
      return 0;
    }
    d->emax = ne;
    signed char *nim = dt_realloc(d->imax, (size_t)nc);
    if (!nim) {
      return 0;
    }
    d->imax = nim;
    d->cap = nc;
  }
  return 1;
}

static int out_reserve(dt_cc_detect_noisy_stream_decoder *d, int extra) {
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
    dt_cc_detect_noisy_decode_details *nb =
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

/* Score the window of `span` bits starting at absolute index `start` and fold its
 * evidence (max bias excess) into emax over the positions it covers. A window
 * straddling an indel or a code/random boundary loses phase alignment over part of
 * its rows, so its bias - and excess - falls, which keeps localization reasonable. */
static void process_start(dt_cc_detect_noisy_stream_decoder *d, long long start,
                          int span) {
  const int lo = d->head + (int)(start - d->base);
  const float ex = window_excess(d, d->in + lo, span);
  const int iunits = invalid_units(d->in + lo, span);
  for (int j = 0; j < span; ++j) {
    if (ex > d->emax[lo + j]) {
      d->emax[lo + j] = ex;
    }
    if (iunits > (int)d->imax[lo + j]) {
      d->imax[lo + j] = (signed char)iunits;
    }
  }
}

/* Emit (finalize) every position in [base, upto): its emax is settled because all
 * window starts <= position have been processed. */
static int emit_upto(dt_cc_detect_noisy_stream_decoder *d, long long upto) {
  int count = (int)(upto - d->base);
  if (count <= 0) {
    return 1;
  }
  if (!out_reserve(d, count)) {
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    d->out[d->out_len++] =
        verdict_from(d->emax[d->head], (int)d->imax[d->head], d->detectability);
    ++d->head;
    ++d->base;
  }
  return 1;
}

static int drain(dt_cc_detect_noisy_stream_decoder *d,
                 dt_cc_detect_noisy_decode_details *details, int max_out) {
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

/* detectability = (1 - 2p)^DET_WREF, p = expected per-bit FLIP/overwrite corruption:
 * the bias a representative check would retain. Indels (p_ins / p_del) are excluded -
 * the sliding method tolerates them, so they must not feed detectability.
 * p >= 0.5 erases all bias -> detectability 0 (a code cannot be ruled
 * out at all). */
static float detectability_from(const dt_cc_detect_noisy_stream_params *p) {
  const float p_ovr = p->p_ovr_true + p->p_ovr_false + p->p_ovr_erase;
  float p_corrupt = p_ovr + (1.0f - p_ovr) * p->p_flip;
  const float one_minus_2p = 1.0f - 2.0f * p_corrupt;
  if (one_minus_2p >= 1.0f) {
    return 1.0f; /* clean channel: a code would bias fully */
  }
  if (one_minus_2p <= 0.0f) {
    return 0.0f; /* corruption >= 50%: no bias survives, cannot rule a code out */
  }
  return dt_exp((float)DET_WREF * dt_log(one_minus_2p));
}

dt_cc_detect_noisy_stream_decoder *dt_cc_detect_noisy_stream_decoder_create(
    const dt_cc_detect_noisy_stream_params *params) {
  if (!params) {
    return NULL;
  }
  /* Validate the channel model. p_flip may be 0 (expect a clean channel); the
   * rates are non-negative and the overwrite / indel families each sum to < 1.
   * decision_depth and max_drift are accepted for interface uniformity (the bias
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
  dt_cc_detect_noisy_stream_decoder *d = dt_malloc(sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->acc = dt_malloc((size_t)DET_M * sizeof(*d->acc));
  if (!d->acc) {
    dt_free(d);
    return NULL;
  }
  d->in = NULL;
  d->emax = NULL;
  d->imax = NULL;
  d->head = d->len = d->cap = 0;
  d->base = 0;
  d->next_start = 0;
  d->out = NULL;
  d->out_head = d->out_len = d->out_cap = 0;
  d->finalized = 0;
  d->detectability = detectability_from(params);
  return d;
}

void dt_cc_detect_noisy_stream_decoder_destroy(dt_cc_detect_noisy_stream_decoder *d) {
  if (!d) {
    return;
  }
  dt_free(d->in);
  dt_free(d->emax);
  dt_free(d->imax);
  dt_free(d->out);
  dt_free(d->acc);
  dt_free(d);
}

int dt_cc_detect_noisy_stream_decode(dt_cc_detect_noisy_stream_decoder *d, const uint8_t *in,
                            int n_in, dt_cc_detect_noisy_decode_details *details,
                            int max_out) {
  if (!d || (n_in > 0 && !in) || n_in < 0 || max_out < 0) {
    return DT_ERR_ARG;
  }
  if (n_in > 0) {
    if (!buf_reserve(d, n_in)) {
      return DT_ERR_ALLOC;
    }
    for (int i = 0; i < n_in; ++i) {
      d->in[d->len] = DT_IS_BIT(in[i]) ? (unsigned char)DT_BIT(in[i])
                      : DT_IS_BOUND(in[i]) ? DET_INVAL  /* DT_INVALID: bound non-bit */
                                           : DET_NOTBIT; /* erasure/absent: unbound  */
      d->emax[d->len] = DET_UNCOVERED; /* not yet covered */
      d->imax[d->len] = 0;             /* no invalid evidence yet */
      ++d->len;
    }
  }
  /* Score every window whose full span is now buffered, then finalize the positions
   * those windows have settled (everything below next_start). */
  const long long bend = d->base + (d->len - d->head);
  while (d->next_start + DET_L <= bend) {
    process_start(d, d->next_start, DET_L);
    d->next_start += DET_STEP;
  }
  if (!emit_upto(d, d->next_start)) {
    return DT_ERR_ALLOC;
  }
  return drain(d, details, max_out);
}

int dt_cc_detect_noisy_stream_decode_flush(dt_cc_detect_noisy_stream_decoder *d,
                                 dt_cc_detect_noisy_decode_details *details,
                                 int max_out) {
  if (!d || max_out < 0) {
    return DT_ERR_ARG;
  }
  if (!d->finalized) {
    const long long bend = d->base + (d->len - d->head);
    /* Full-span windows, then the tail's shorter windows (down to DET_MINL), each
     * scored over whatever bits remain - fewer rows, so a higher floor and a more
     * conservative verdict, computed from the actual row count. */
    while (d->next_start + DET_L <= bend) {
      process_start(d, d->next_start, DET_L);
      d->next_start += DET_STEP;
    }
    while (d->next_start + DET_MINL <= bend) {
      process_start(d, d->next_start, (int)(bend - d->next_start));
      d->next_start += DET_STEP;
    }
    d->finalized = 1;
    if (!emit_upto(d, bend)) { /* emit all remaining positions */
      return DT_ERR_ALLOC;
    }
  }
  return drain(d, details, max_out);
}
