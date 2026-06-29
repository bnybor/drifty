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
 * detect decode engine - blind detection of convolutional-code structure in an
 * arbitrary bit stream, with no prior knowledge or coordination (no code, rate,
 * generators, or alignment).
 *
 * Method - GF(2) strided-window rank deficiency. A convolutional code is linear
 * and time-invariant, so its output bits satisfy parity-check relations: a
 * width-W window of coded bits lives in a proper GF(2) subspace, so a matrix built
 * from such windows is rank-DEFICIENT, while random bits span the full space
 * (full rank). The catch is that the code's parity checks are PHASE-specific (they
 * relate output bits at a fixed position mod n, the block size), so the window
 * rows must be stacked at a stride equal to n to stay phase-aligned. n is unknown,
 * so we SWEEP candidate strides s = 2..DET_SMAX and take the largest deficiency
 * d = max_s (W - rank_s); a code of block size n shows up at s = n (and multiples).
 *
 *   d = 0          -> no linear structure  -> "no code" (c_absent high)
 *   d > 0          -> parity checks found  -> "code present" (c_lost = 1 - 2^-d)
 *
 * Per block of DET_BLOCK input bits we compute one (c_lost, c_absent) verdict and
 * emit DET_BLOCK records carrying it (one per input position). Output trails input
 * by up to one block.
 *
 * LIMITATIONS (see also doc/cc/detect.md):
 *  - Noise: a single flipped bit breaks exact parity over the windows that overlap
 *    it, so exact GF(2) rank is brittle. This targets the clean / very-low-noise
 *    regime; it holds to ~0.5% bit flips and collapses to "absent" by ~1%.
 *  - Scope: rank deficiency senses LINEAR structure in general - a block linear
 *    code or an LFSR scrambler would also register. For the intended use (a stream
 *    is either uncoded/random or convolutionally coded) this is the right proxy.
 *  - Codes with block size n > DET_SMAX are not covered by the stride sweep.
 */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h> /* dt_malloc / dt_free / dt_realloc / dt_memmove / dt_exp */

/* Detector geometry. W must fit a uint64_t and exceed a code's parity span
 * (~2*K bits for the standard K<=7 codes). BLOCK is sized so even the largest
 * stride leaves N = (BLOCK - W)/SMAX + 1 >= W rows (here 59 >= 32) for a
 * well-determined null space. Validated empirically on the standard presets. */
#define DET_W 32      /* GF(2) window width, bits */
#define DET_SMAX 6    /* sweep strides 2..DET_SMAX (block sizes n in 2..6) */
#define DET_BLOCK 384 /* input bits analysed per detection block */

#define DET_LN2 0.69314718055994531f

struct dt_cc_detect_stream_decoder {
  /* Input bit FIFO (0/1 values), drained from the front by `head`. */
  unsigned char *in;
  int in_head, in_len, in_cap;
  /* Pending output FIFO of per-position records, drained from the front. */
  dt_cc_detect_decode_details *out;
  int out_head, out_len, out_cap;
  int finalized; /* the short final block has been processed */
  /* (1 - expected per-bit corruption)^DET_W, from the channel model: the chance a
   * width-W window survives the expected noise intact, hence the chance a code
   * (if present) would still show as rank deficiency. Scales the confidence of a
   * "no code" verdict - a noisy channel cannot confidently rule a code out. */
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

/* GF(2) rank of the matrix whose rows are the DET_W-bit windows of bits[0..count)
 * taken at offsets 0, stride, 2*stride, ... (online Gaussian elimination against
 * an MSB-indexed pivot table). Writes the row count to *nrows. */
static int gf2_rank(const unsigned char *bits, int count, int stride, int *nrows) {
  uint64_t pivot[DET_W];
  for (int i = 0; i < DET_W; ++i) {
    pivot[i] = 0;
  }
  int rank = 0, rows = 0;
  for (int start = 0; start + DET_W <= count; start += stride) {
    uint64_t v = 0;
    for (int j = 0; j < DET_W; ++j) {
      v = (v << 1) | (uint64_t)(bits[start + j] & 1u);
    }
    ++rows;
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
  *nrows = rows;
  return rank;
}

/* Analyse `count` bits (count <= DET_BLOCK) and produce the (c_lost, c_absent)
 * verdict. d = max deficiency over the stride sweep; N_min = fewest rows over the
 * sweep (the weakest-evidence stride), which gauges data sufficiency.
 * `detectability` (channel-derived, in [0, 1]) scales how much a NULL result can
 * be trusted as "no code". */
static dt_cc_detect_decode_details analyse(const unsigned char *bits, int count,
                                           float detectability) {
  dt_cc_detect_decode_details det;
  int d = 0, n_min = 1 << 30;
  for (int s = 2; s <= DET_SMAX; ++s) {
    int rows;
    const int rank = gf2_rank(bits, count, s, &rows);
    const int def = DET_W - rank;
    if (def > d) {
      d = def;
    }
    if (rows < n_min) {
      n_min = rows;
    }
  }
  if (n_min < DET_W) {
    /* Too few rows to determine the code's null space at some stride: the
     * deficiency could be mere under-determination, not structure. Abstain. */
    det.c_lost = 0.0f;
    det.c_absent = 0.0f;
    return det;
  }
  /* c_lost: strength of the structural evidence (parity checks found). Found
   * checks are real regardless of expected noise, so detectability never scales
   * this up. */
  det.c_lost = 1.0f - pow2_neg(d);
  /* c_absent: when no structure is found, confidence grows with how many rows
   * beyond W ruled a code out, but is discounted by `detectability` - if the
   * channel is noisy enough to hide a code, a clean-looking block cannot
   * confidently be called code-free. When structure IS found it is the small
   * residual (independent of expected noise). */
  det.c_absent = (d == 0) ? (1.0f - pow2_neg(n_min - DET_W)) * detectability
                          : pow2_neg(d);
  return det;
}

/* -- small growable FIFOs -------------------------------------------------- */

static int in_reserve(dt_cc_detect_stream_decoder *d, int extra) {
  if (d->in_head > 0 && d->in_len + extra > d->in_cap) {
    dt_memmove(d->in, d->in + d->in_head, (size_t)(d->in_len - d->in_head));
    d->in_len -= d->in_head;
    d->in_head = 0;
  }
  if (d->in_len + extra > d->in_cap) {
    int nc = d->in_cap ? d->in_cap * 2 : 1024;
    while (nc < d->in_len + extra) {
      nc *= 2;
    }
    unsigned char *nb = dt_realloc(d->in, (size_t)nc);
    if (!nb) {
      return 0;
    }
    d->in = nb;
    d->in_cap = nc;
  }
  return 1;
}

static int out_reserve(dt_cc_detect_stream_decoder *d, int extra) {
  if (d->out_head > 0 && d->out_len + extra > d->out_cap) {
    dt_memmove(d->out, d->out + d->out_head,
               (size_t)(d->out_len - d->out_head) * sizeof(*d->out));
    d->out_len -= d->out_head;
    d->out_head = 0;
  }
  if (d->out_len + extra > d->out_cap) {
    int nc = d->out_cap ? d->out_cap * 2 : DET_BLOCK * 2;
    while (nc < d->out_len + extra) {
      nc *= 2;
    }
    dt_cc_detect_decode_details *nb =
        dt_realloc(d->out, (size_t)nc * sizeof(*nb));
    if (!nb) {
      return 0;
    }
    d->out = nb;
    d->out_cap = nc;
  }
  return 1;
}

/* Analyse the next `count` buffered input bits and enqueue `count` output records
 * carrying the block's verdict; consume those input bits. Returns 0 on OOM. */
static int emit_block(dt_cc_detect_stream_decoder *d, int count) {
  const dt_cc_detect_decode_details v =
      analyse(d->in + d->in_head, count, d->detectability);
  if (!out_reserve(d, count)) {
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    d->out[d->out_len++] = v;
  }
  d->in_head += count;
  return 1;
}

static int drain(dt_cc_detect_stream_decoder *d,
                 dt_cc_detect_decode_details *details, int max_out) {
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

/* Aggregate per-bit corruption probability the channel model implies: a bit is
 * "wrong/unknown" if overwritten (any kind) or, failing that, flipped; an
 * insertion or deletion additionally shifts the strided-window phase. Any of these
 * breaks a window's exact parity, so they all count toward what could hide a code.
 * Returns detectability = (1 - p_corrupt)^DET_W in [0, 1]. */
static float detectability_from(const dt_cc_detect_stream_params *p) {
  const float p_ovr = p->p_ovr_true + p->p_ovr_false + p->p_ovr_erase;
  const float p_ins = p->p_ins_true + p->p_ins_false + p->p_ins_erase;
  float p_corrupt = p_ovr + (1.0f - p_ovr) * p->p_flip + p_ins + p->p_del;
  if (p_corrupt <= 0.0f) {
    return 1.0f; /* clean channel: a code would show through fully */
  }
  if (p_corrupt > 0.999f) {
    p_corrupt = 0.999f;
  }
  /* (1 - p_corrupt)^DET_W via the freestanding single-precision proxies. */
  return dt_exp((float)DET_W * dt_log(1.0f - p_corrupt));
}

dt_cc_detect_stream_decoder *dt_cc_detect_stream_decoder_create(
    const dt_cc_detect_stream_params *params) {
  if (!params) {
    return NULL;
  }
  /* Validate the channel model. p_flip may be 0 (expect a clean channel); the
   * rates are non-negative and the overwrite / indel families each sum to < 1.
   * decision_depth and max_drift are accepted for interface uniformity (the rank
   * method does not use them). */
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
  dt_cc_detect_stream_decoder *d = dt_malloc(sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->in = NULL;
  d->in_head = d->in_len = d->in_cap = 0;
  d->out = NULL;
  d->out_head = d->out_len = d->out_cap = 0;
  d->finalized = 0;
  d->detectability = detectability_from(params);
  return d;
}

void dt_cc_detect_stream_decoder_destroy(dt_cc_detect_stream_decoder *d) {
  if (!d) {
    return;
  }
  dt_free(d->in);
  dt_free(d->out);
  dt_free(d);
}

int dt_cc_detect_stream_decode(dt_cc_detect_stream_decoder *d, const uint8_t *in,
                            int n_in, dt_cc_detect_decode_details *details,
                            int max_out) {
  if (!d || (n_in > 0 && !in) || n_in < 0 || max_out < 0) {
    return DT_ERR_ARG;
  }
  if (n_in > 0) {
    if (!in_reserve(d, n_in)) {
      return DT_ERR_ALLOC;
    }
    for (int i = 0; i < n_in; ++i) {
      d->in[d->in_len++] = (unsigned char)DT_BIT(in[i]); /* non-bits -> 0 */
    }
  }
  /* Process every complete block now buffered. */
  while (d->in_len - d->in_head >= DET_BLOCK) {
    if (!emit_block(d, DET_BLOCK)) {
      return DT_ERR_ALLOC;
    }
  }
  return drain(d, details, max_out);
}

int dt_cc_detect_stream_decode_flush(dt_cc_detect_stream_decoder *d,
                                 dt_cc_detect_decode_details *details,
                                 int max_out) {
  if (!d || max_out < 0) {
    return DT_ERR_ARG;
  }
  if (!d->finalized) {
    /* Drain any complete blocks, then the short final block (its records carry a
     * verdict if there is enough data, else an abstain). */
    while (d->in_len - d->in_head >= DET_BLOCK) {
      if (!emit_block(d, DET_BLOCK)) {
        return DT_ERR_ALLOC;
      }
    }
    const int rem = d->in_len - d->in_head;
    if (rem > 0) {
      if (!emit_block(d, rem)) {
        return DT_ERR_ALLOC;
      }
    }
    d->finalized = 1;
  }
  return drain(d, details, max_out);
}
