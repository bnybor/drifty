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
 * dt_detect_clean_metrics - Monte-Carlo measurement of the detect_clean blind
 * code-presence detector (exact GF(2) sliding-window rank deficiency) as a function
 * of the channel's flip / insert / delete / erase rates, for each standard code.
 *
 * detect_clean does not recover bits - it answers "is a convolutional code present?".
 * So for one point we run TWO streams through the channel: a CODED one (a random
 * message encoded with the code) and a pure RANDOM one of the same length. Each is
 * detected, and we report the mean of the detector's two soft confidences over the
 * stream interior (the head/tail abstain transient is trimmed):
 *
 *   present (c_erasure) - confidence a code IS present. High on coded, ~0 on random.
 *   absent  (c_absent)  - confidence a code is NOT present. ~0 on coded, high on
 *                         random.
 *
 * Emitting the random stream's two means alongside the coded ones gives the plotter
 * a pure-random BASELINE to compare against: detection works to the extent the coded
 * curves stand clear of the random ones.
 *
 * Each axis (flip, insert, delete, erase) is swept independently with the other
 * rates at zero. The detector takes a channel model, selected by a VARIATION:
 *
 *   pegged  - the model is fixed at a flat 1% on every impairment, regardless of
 *             axis or rate (the decoder never told what the channel does).
 *   matched - the swept impairment's model rate tracks the channel rate; the others
 *             stay at the 1% floor.
 *
 * The model only calibrates the no-code confidence (c_absent): the detector damps it
 * by a detectability factor when it expects flips/overwrites (a code could be hidden
 * by them), so matched FLIP and ERASE sweeps pull c_absent down as the rate climbs,
 * while the code-present confidence (c_erasure) and the INSERT/DELETE axes are
 * unaffected by the model (indels are tolerated, not a reason to doubt "no code").
 *
 * Output is CSV on stdout (see header row); feed it to plot_metrics.py.
 *
 * Usage: dt_detect_clean_metrics [trials] [info_bits] [seed] [variation] [grids]
 */

#include <drifty/bit.h>
#include <drifty/cc/ccode.h>
#include <drifty/cc/detect_clean.h>
#include <drifty/cc/encoder.h>
#include <drifty/soft_bit.h>
#include <drifty/stream_encoder.h>
#include <drifty/stream_soft_decoder.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* Head/tail abstain transient trimmed from each stream before averaging: at least
 * detect_clean's longest analysis window (~332 bits). */
#define DET_TRIM 400

/* -- deterministic PRNG (splitmix64) --------------------------------------- */

static uint64_t rng_next(uint64_t *state) {
  uint64_t value = (*state += 0x9E3779B97F4A7C15ULL);
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}
static double rng_unit(uint64_t *state) {
  return (double)(rng_next(state) >> 11) * (1.0 / 9007199254740992.0);
}
static uint8_t bit_sym(unsigned int v) { return (v & 1u) ? DT_TRUE : DT_FALSE; }

/* Independent, reproducible seed for work item `index` from the base seed, so each
 * point owns its own PRNG stream regardless of thread scheduling. */
static uint64_t derive_seed(uint64_t base, int index) {
  uint64_t value = base + 0x9E3779B97F4A7C15ULL * (uint64_t)(index + 1);
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

static void *xmalloc(size_t size) {
  void *ptr = malloc(size);
  if (!ptr) {
    fprintf(stderr, "dt_detect_clean_metrics: out of memory\n");
    exit(1);
  }
  return ptr;
}
static double clamp_double(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* -- codes, axes, variations ----------------------------------------------- */

typedef struct {
  const char *name;
  dt_cc_standard_code which;
} code_entry;
static const code_entry CODES[] = {
    {"K3_R1_2", DT_CC_CODE_K3_RATE_1_2},
    {"K7_R1_2", DT_CC_CODE_K7_RATE_1_2},
    {"K7_R1_3", DT_CC_CODE_K7_RATE_1_3},
    {"K5_R1_5", DT_CC_CODE_K5_RATE_1_5},
};
#define N_CODES ((int)(sizeof(CODES) / sizeof(CODES[0])))

typedef enum { AXIS_FLIP, AXIS_INSERT, AXIS_DELETE, AXIS_ERASE } axis;
static const char *AXIS_NAME[] = {"flip", "insert", "delete", "erase"};
#define N_AXES ((int)(sizeof(AXIS_NAME) / sizeof(AXIS_NAME[0])))

typedef enum { VAR_PEGGED, VAR_MATCHED } variation;
static int parse_variation(const char *s) {
  if (strcmp(s, "pegged") == 0 || strcmp(s, "untuned") == 0) return VAR_PEGGED;
  if (strcmp(s, "matched") == 0 || strcmp(s, "tuned") == 0) return VAR_MATCHED;
  return -1;
}

/* -- per-axis channel rate grids (read from a file at startup) -------------- */

typedef struct {
  double *rates;
  int count;
} rate_grid;
static rate_grid g_grids[N_AXES];

static int name_index(const char *name, const char *const *names, int n) {
  for (int i = 0; i < n; ++i) {
    if (strcmp(name, names[i]) == 0) return i;
  }
  return -1;
}

/* Load per-axis grids: each non-blank, non-comment line is "<axis> <rate> ...";
 * '#' begins a comment. Both variations share one grid per axis (same channel).
 * Returns 0, or -1 on error. */
static int load_grids(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "dt_detect_clean_metrics: cannot open rate-grid file '%s'\n",
            path);
    return -1;
  }
  char line[8192];
  int lineno = 0, ok = 1;
  while (ok && fgets(line, sizeof(line), f)) {
    ++lineno;
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *first = strtok(line, " \t\r\n");
    if (!first) continue;
    int a = name_index(first, AXIS_NAME, N_AXES);
    if (a < 0) {
      fprintf(stderr, "dt_detect_clean_metrics: %s:%d: unknown axis '%s'\n", path,
              lineno, first);
      ok = 0;
      break;
    }
    int cap = 16, n = 0;
    double *rates = xmalloc((size_t)cap * sizeof(double));
    for (char *t = strtok(NULL, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
      char *end;
      double value = strtod(t, &end);
      if (*end != '\0') {
        fprintf(stderr, "dt_detect_clean_metrics: %s:%d: bad rate '%s'\n", path,
                lineno, t);
        ok = 0;
        break;
      }
      if (n == cap) {
        cap *= 2;
        rates = realloc(rates, (size_t)cap * sizeof(double));
        if (!rates) {
          fprintf(stderr, "dt_detect_clean_metrics: out of memory\n");
          exit(1);
        }
      }
      rates[n++] = value;
    }
    if (ok && n == 0) {
      fprintf(stderr, "dt_detect_clean_metrics: %s:%d: grid has no rates\n", path,
              lineno);
      ok = 0;
    }
    if (!ok) {
      free(rates);
      break;
    }
    free(g_grids[a].rates);
    g_grids[a].rates = rates;
    g_grids[a].count = n;
  }
  fclose(f);
  return ok ? 0 : -1;
}

/* -- channel and detector drive -------------------------------------------- */

static int encode_stream(const dt_cc_code *code, const uint8_t *msg, int info_bits,
                         uint8_t *out, int cap) {
  dt_stream_encoder *enc = dt_cc_encoder_create(code);
  if (!enc) {
    fprintf(stderr, "dt_detect_clean_metrics: encoder create failed\n");
    exit(1);
  }
  int len = enc->begin(enc, out, (size_t)cap);
  len += enc->encode(enc, out + len, (size_t)(cap - len), msg, (size_t)info_bits);
  len += enc->finalize(enc, out + len, (size_t)(cap - len));
  dt_cc_encoder_destroy(enc);
  return len;
}

/* Apply the channel to src[0..len): with probability p_ins emit a random bit
 * before each, with p_del drop the bit, otherwise emit it flipped with p_flip and
 * then, with p_erase, marked DT_ERASURE. Returns received length; stores a
 * malloc'd buffer in *out (caller frees). */
static int apply_channel(const uint8_t *src, int len, double p_flip, double p_ins,
                         double p_del, double p_erase, uint64_t *rng,
                         uint8_t **out) {
  int cap = len + len / 2 + 64;
  uint8_t *rx = xmalloc((size_t)cap);
  int rl = 0;
  for (int i = 0; i < len; ++i) {
    if (rl + 2 > cap) {
      cap *= 2;
      rx = realloc(rx, (size_t)cap);
      if (!rx) {
        fprintf(stderr, "dt_detect_clean_metrics: out of memory\n");
        exit(1);
      }
    }
    if (p_ins > 0.0 && rng_unit(rng) < p_ins) {
      rx[rl++] = bit_sym((unsigned int)rng_next(rng));
    }
    if (p_del > 0.0 && rng_unit(rng) < p_del) {
      continue;
    }
    uint8_t bit = src[i];
    if (p_flip > 0.0 && rng_unit(rng) < p_flip) {
      bit ^= DT_VALUE; /* DT_TRUE <-> DT_FALSE */
    }
    if (p_erase > 0.0 && rng_unit(rng) < p_erase) {
      bit = DT_ERASURE;
    }
    rx[rl++] = bit;
  }
  *out = rx;
  return rl;
}

/* The detector's channel model for one point, fixed by axis/rate/variation. */
typedef struct {
  dt_cc_detect_clean_stream_params params;
  int code_n, K;
} point_model;

static point_model make_model(const dt_cc_code *code, axis a, double rate,
                              variation var) {
  point_model m;
  m.code_n = dt_cc_code_n(code);
  m.K = dt_cc_code_k(code);
  const double floor = 0.01; /* the flat "pegged" level / matched's inactive floor */
  double p_flip = floor, p_ins = floor, p_del = floor, p_erase = floor;
  if (var == VAR_MATCHED) {
    if (a == AXIS_FLIP) p_flip = clamp_double(rate, 0.0, 0.999);
    else if (a == AXIS_INSERT) p_ins = clamp_double(rate, 0.0, 0.95);
    else if (a == AXIS_DELETE) p_del = clamp_double(rate, 0.0, 0.95);
    else /* AXIS_ERASE */ p_erase = clamp_double(rate, 0.0, 0.999);
  }
  dt_cc_detect_clean_stream_params p = {0};
  p.decision_depth = 8 * m.K; /* required >= 1 but unused by the rank method */
  p.max_drift = 8;            /* required >= 0 but unused */
  p.p_flip = (float)p_flip;
  p.p_ins_true = (float)(p_ins * 0.5);
  p.p_ins_false = (float)(p_ins * 0.5);
  p.p_del = (float)p_del;
  p.p_ovr_erase = (float)p_erase;
  m.params = p;
  return m;
}

/* Run detect_clean over rx[0..rlen) and average code-present (c_erasure) and no-code
 * (c_absent) over the interior [DET_TRIM, n-DET_TRIM). */
static void run_detect(const dt_cc_detect_clean_stream_params *params,
                       const uint8_t *rx, int rlen, double *present,
                       double *absent) {
  dt_stream_soft_decoder *sd = dt_cc_detect_clean_soft_decoder_create(params);
  if (!sd) {
    fprintf(stderr, "dt_detect_clean_metrics: detector create failed\n");
    exit(1);
  }
  dt_soft_bit *out = xmalloc((size_t)(rlen + 64) * sizeof(*out));
  int got = sd->begin(sd, NULL, 0);
  got += sd->decode(sd, out + got, (size_t)(rlen + 64 - got), rx, (size_t)rlen);
  for (;;) {
    int w = sd->decode(sd, out + got, (size_t)(rlen + 64 - got), NULL, 0);
    if (w <= 0) {
      if (w < 0) got = w;
      break;
    }
    got += w;
  }
  if (got >= 0) got += sd->finalize(sd, out + got, (size_t)(rlen + 64 - got));
  dt_cc_detect_clean_soft_decoder_destroy(sd);
  if (got < 0) {
    fprintf(stderr, "dt_detect_clean_metrics: decode error %d\n", got);
    exit(1);
  }
  int lo = DET_TRIM, hi = got - DET_TRIM;
  if (hi - lo < 64) { /* stream too short to trim */
    lo = 0;
    hi = got;
  }
  double sp = 0, sa = 0;
  int n = 0;
  for (int i = lo; i < hi; ++i) {
    sp += out[i].c_erasure;
    sa += out[i].c_absent;
    ++n;
  }
  free(out);
  *present = n ? sp / n : 0;
  *absent = n ? sa / n : 0;
}

/* -- experiment ------------------------------------------------------------ */

typedef struct {
  double coded_present, coded_absent, random_present, random_absent;
} point_result;

static point_result run_point(const dt_cc_code *code, axis a, double rate,
                              variation var, int info_bits, int trials,
                              uint64_t seed, int index) {
  const point_model m = make_model(code, a, rate, var);
  const int n = m.code_n, K = m.K;
  const int coded_cap = (info_bits + K) * n + 64;
  uint8_t *msg = xmalloc((size_t)info_bits);
  uint8_t *coded = xmalloc((size_t)coded_cap);
  const double c_flip = a == AXIS_FLIP ? rate : 0.0;
  const double c_ins = a == AXIS_INSERT ? rate : 0.0;
  const double c_del = a == AXIS_DELETE ? rate : 0.0;
  const double c_erase = a == AXIS_ERASE ? rate : 0.0;
  point_result acc = {0, 0, 0, 0};

  for (int t = 0; t < trials; ++t) {
    uint64_t rng = derive_seed(seed, index * trials + t);

    /* coded stream through the channel */
    for (int i = 0; i < info_bits; ++i) {
      msg[i] = bit_sym((unsigned int)rng_next(&rng));
    }
    int clen = encode_stream(code, msg, info_bits, coded, coded_cap);
    uint8_t *rx = NULL;
    int rl = apply_channel(coded, clen, c_flip, c_ins, c_del, c_erase, &rng, &rx);
    double cp, ca;
    run_detect(&m.params, rx, rl, &cp, &ca);
    free(rx);

    /* pure-random stream of the same pre-channel length through the same channel */
    uint8_t *rnd = xmalloc((size_t)clen);
    for (int i = 0; i < clen; ++i) {
      rnd[i] = bit_sym((unsigned int)rng_next(&rng));
    }
    uint8_t *rrx = NULL;
    int rrl = apply_channel(rnd, clen, c_flip, c_ins, c_del, c_erase, &rng, &rrx);
    double rp, ra;
    run_detect(&m.params, rrx, rrl, &rp, &ra);
    free(rrx);
    free(rnd);

    acc.coded_present += cp;
    acc.coded_absent += ca;
    acc.random_present += rp;
    acc.random_absent += ra;
  }
  free(msg);
  free(coded);
  acc.coded_present /= trials;
  acc.coded_absent /= trials;
  acc.random_present /= trials;
  acc.random_absent /= trials;
  return acc;
}

typedef struct {
  int code_idx;
  axis channel_axis;
  double rate;
} work_item;

int main(int argc, char **argv) {
  int trials = argc > 1 ? atoi(argv[1]) : 16;
  int info_bits = argc > 2 ? atoi(argv[2]) : 4000;
  uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 0xC0FFEEULL;
  variation var = VAR_PEGGED;
  if (argc > 4) {
    int parsed = parse_variation(argv[4]);
    if (parsed < 0) {
      fprintf(stderr, "dt_detect_clean_metrics: unknown variation '%s' "
                      "(use pegged|matched)\n",
              argv[4]);
      return 2;
    }
    var = (variation)parsed;
  }
  const char *grids_path =
      argc > 5 ? argv[5] : "metrics/detect_clean/rate_grids.txt";
  if (trials < 1 || info_bits < 1000) {
    fprintf(stderr, "usage: %s [trials>=1] [info_bits>=1000] [seed] "
                    "[variation=pegged|matched] [rate_grids_file]\n",
            argv[0]);
    return 2;
  }
  if (load_grids(grids_path) < 0) {
    return 2;
  }

  dt_cc_code *codes[N_CODES];
  for (int c = 0; c < N_CODES; ++c) {
    codes[c] = dt_cc_code_create_standard(CODES[c].which);
    if (!codes[c]) {
      fprintf(stderr, "dt_detect_clean_metrics: code create failed\n");
      return 1;
    }
  }

  int n_points = 0;
  for (int a = 0; a < N_AXES; ++a) {
    if (g_grids[a].count == 0) {
      fprintf(stderr, "dt_detect_clean_metrics: %s: no grid for axis %s\n",
              grids_path, AXIS_NAME[a]);
      return 2;
    }
    n_points += N_CODES * g_grids[a].count;
  }
  work_item *items = xmalloc((size_t)n_points * sizeof(*items));
  int filled = 0;
  for (int c = 0; c < N_CODES; ++c) {
    for (int a = 0; a < N_AXES; ++a) {
      for (int r = 0; r < g_grids[a].count; ++r) {
        items[filled].code_idx = c;
        items[filled].channel_axis = (axis)a;
        items[filled].rate = g_grids[a].rates[r];
        ++filled;
      }
    }
  }

  const char *var_name = var == VAR_MATCHED ? "matched" : "pegged";
#ifdef _OPENMP
  fprintf(stderr, "detect_clean: %d points x %d trials (%s) on %d threads ...\n",
          n_points, trials, var_name, omp_get_max_threads());
#else
  fprintf(stderr,
          "detect_clean: %d points x %d trials (%s, single-threaded) ...\n",
          n_points, trials, var_name);
#endif

  point_result *results = xmalloc((size_t)n_points * sizeof(*results));
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
  for (int i = 0; i < n_points; ++i) {
    results[i] = run_point(codes[items[i].code_idx], items[i].channel_axis,
                           items[i].rate, var, info_bits, trials, seed, i);
  }

  printf("code,variation,axis,rate,dec_p_flip,dec_p_ins,dec_p_del,"
         "dec_p_ovr_erase,trials,coded_present,coded_absent,"
         "random_present,random_absent\n");
  for (int i = 0; i < n_points; ++i) {
    const point_model m =
        make_model(codes[items[i].code_idx], items[i].channel_axis,
                   items[i].rate, var);
    const point_result *p = &results[i];
    printf("%s,%s,%s,%.6g,%.6g,%.6g,%.6g,%.6g,%d,%.6f,%.6f,%.6f,%.6f\n",
           CODES[items[i].code_idx].name, var_name,
           AXIS_NAME[items[i].channel_axis], items[i].rate, m.params.p_flip,
           m.params.p_ins_true + m.params.p_ins_false, m.params.p_del,
           m.params.p_ovr_erase, trials, p->coded_present, p->coded_absent,
           p->random_present, p->random_absent);
  }
  fflush(stdout);

  free(results);
  free(items);
  for (int c = 0; c < N_CODES; ++c) {
    dt_cc_code_destroy(codes[c]);
  }
  return 0;
}
