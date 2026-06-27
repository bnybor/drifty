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
 * dt_bcjr_metrics - Monte-Carlo measurement of the decoding-mistake rate as a
 * function of the channel's flip and erase rates, for each standard code, for the
 * bcjr codec (a max-log-MAP / forward-backward hard-decision decoder).
 *
 * It is the bcjr counterpart of metrics/hybrid and metrics/vindel, restricted to
 * the channel bcjr faces:
 *   - bcjr does not track inserted or dropped bits, so only the flip and erase
 *     axes are swept (insert / delete are meaningless here - that is what vindel
 *     and hybrid are for).
 *   - The decoder takes a channel model (decision_depth, p_flip, p_erase); this
 *     harness runs the *matched* model, setting the decoder's rate for the swept
 *     impairment to the channel's rate (the decoder's best case).
 *   - The decoder blind-acquires (it does not assume the encoder's start state),
 *     so the first ~decision_depth decoded bits are an acquisition transient and
 *     are dropped from both sequences before comparison.
 *
 * The reported metric is the normalized edit (Levenshtein) distance between the
 * decoded bits and the original message over the kept window, divided by the
 * number of message bits; the trailing flush bits are trimmed off the decoded
 * tail first. (bcjr also emits a soft lock probability; this coarse harness
 * measures edit distance only.) Drive it through the public API. Output is CSV on
 * stdout (see header row); feed it to metrics/bcjr/plot_metrics.py.
 *
 * Usage: dt_bcjr_metrics [trials] [info_bits] [seed] [rate_grids_file]
 */

#include <drifty/cc/bcjr.h>
#include <drifty/cc/encoders.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* -- deterministic PRNG (splitmix64) --------------------------------------- */

static uint64_t rng_next(uint64_t *state) {
  uint64_t value = (*state += 0x9E3779B97F4A7C15ULL);
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

/* Uniform double in [0, 1). */
static double rng_unit(uint64_t *state) {
  return (double)(rng_next(state) >> 11) * (1.0 / 9007199254740992.0);
}

/* Map a raw 0/1 (only the low bit is read) to its DT_TRUE/DT_FALSE symbol. */
static uint8_t bit_sym(unsigned int v) { return (v & 1u) ? DT_TRUE : DT_FALSE; }

/* Independent, reproducible seed for work item `index` from the base seed. Each
 * point owns its own PRNG stream, so results don't depend on thread scheduling
 * or how many draws other points made - parallel runs match serial ones. */
static uint64_t derive_seed(uint64_t base, int index) {
  uint64_t value = base + 0x9E3779B97F4A7C15ULL * (uint64_t)(index + 1);
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

/* -- helpers --------------------------------------------------------------- */

static void *xmalloc(size_t size) {
  void *ptr = malloc(size);
  if (!ptr) {
    fprintf(stderr, "dt_bcjr_metrics: out of memory\n");
    exit(1);
  }
  return ptr;
}

/* Clamp into [lo, hi]; the decoder needs p_flip strictly inside (0, 1), so the
 * matched model floors it and caps the active rate just shy of 1. */
static double clamp_double(double value, double lo, double hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

/* Apply the channel to `coded` (coded_len bits) into `received` (also coded_len
 * bits - there is no drift, so the length is unchanged): flip each bit with
 * probability p_flip, then mark it DT_ERASURE with probability p_erase. */
static void apply_channel(const uint8_t *coded, int coded_len, double p_flip,
                          double p_erase, uint64_t *rng, uint8_t *received) {
  for (int i = 0; i < coded_len; ++i) {
    uint8_t bit = coded[i];
    if (p_flip > 0.0 && rng_unit(rng) < p_flip) {
      bit ^= DT_VALUE; /* toggle the value bit: DT_TRUE <-> DT_FALSE */
    }
    if (p_erase > 0.0 && rng_unit(rng) < p_erase) {
      bit = DT_ERASURE;
    }
    received[i] = bit;
  }
}

/* -- edit distance --------------------------------------------------------- */

/* Banded edit distance: returns the true distance if it is <= band, otherwise
 * band + 1. Only the diagonal band |row - col| <= band is computed, so the cost
 * is O((len_a + 1) * band). `prev` and `cur` are caller-owned scratch of length
 * len_b + 1. */
static int edit_threshold(const uint8_t *seq_a, int len_a, const uint8_t *seq_b,
                          int len_b, int band, int *prev, int *cur) {
  if (len_a - len_b > band || len_b - len_a > band) {
    return band + 1; /* lengths alone already differ by more than band edits */
  }
  const int INF = INT_MAX / 4;

  int init_end = band + 1 < len_b ? band + 1 : len_b;
  for (int col = 0; col <= init_end; ++col) {
    prev[col] = col <= band ? col : INF;
  }

  for (int row = 1; row <= len_a; ++row) {
    int col_lo = row - band > 1 ? row - band : 1;
    int col_hi = row + band < len_b ? row + band : len_b;
    int right_bound = col_hi + 1 < len_b ? col_hi + 1 : len_b;
    for (int col = col_lo - 1; col <= right_bound; ++col) {
      cur[col] = INF; /* clear only the band cells we will read or write */
    }
    if (row <= band) {
      cur[0] = row; /* delete the first `row` chars of seq_a */
    }
    for (int col = col_lo; col <= col_hi; ++col) {
      int subst_cost =
          prev[col - 1] + (seq_a[row - 1] != seq_b[col - 1] ? 1 : 0);
      int del_cost = prev[col] + 1;
      int ins_cost = cur[col - 1] + 1;
      int best = subst_cost < del_cost ? subst_cost : del_cost;
      cur[col] = ins_cost < best ? ins_cost : best;
    }
    int *tmp = prev;
    prev = cur;
    cur = tmp;
  }

  int dist = prev[len_b];
  return dist > band ? band + 1 : dist;
}

/* Edit distance between seq_a and seq_b, computed with an exponentially growing
 * band so that close sequences (the common case) cost only O(distance *
 * length). */
static long edit_distance(const uint8_t *seq_a, int len_a, const uint8_t *seq_b,
                          int len_b, int *prev, int *cur) {
  const int max_dist = len_a > len_b ? len_a : len_b;
  for (int band = 8;; band *= 2) {
    if (band > max_dist) {
      band = max_dist;
    }
    int dist = edit_threshold(seq_a, len_a, seq_b, len_b, band, prev, cur);
    if (dist <= band || band >= max_dist) {
      return dist;
    }
  }
}

/* -- experiment ------------------------------------------------------------ */

typedef struct {
  const char *name;
  dt_standard_code which;
} code_entry;

static const code_entry CODES[] = {
    {"K3_R1_2", DT_CODE_K3_RATE_1_2},
    {"K7_R1_2", DT_CODE_K7_RATE_1_2},
    {"K7_R1_3", DT_CODE_K7_RATE_1_3},
    {"K5_R1_5", DT_CODE_K5_RATE_1_5},
};
#define N_CODES ((int)(sizeof(CODES) / sizeof(CODES[0])))

/* The bcjr decoder does not track drift, so only flip and erase are swept.
 */
typedef enum { AXIS_FLIP, AXIS_ERASE } axis;
static const char *AXIS_NAME[] = {"flip", "erase"};
#define N_AXES ((int)(sizeof(AXIS_NAME) / sizeof(AXIS_NAME[0])))

/* One rate grid per axis, read at startup from a text file (load_grids; default
 * metrics/bcjr/rate_grids.txt) so a sweep can be retuned without
 * recompiling. */
typedef struct {
  double *rates;
  int count;
} rate_grid;
static rate_grid g_grids[N_AXES];

/* Index of `name` in AXIS_NAME, or -1 if absent. */
static int axis_index(const char *name) {
  for (int i = 0; i < N_AXES; ++i) {
    if (strcmp(name, AXIS_NAME[i]) == 0) {
      return i;
    }
  }
  return -1;
}

/* Load the rate grids from `path` into g_grids. Each non-blank, non-comment
 * line is "<axis>  <rate> <rate> ..."; '#' begins a comment. Returns 0 on
 * success, -1 after printing a diagnostic. */
static int load_grids(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "dt_bcjr_metrics: cannot open rate-grid file '%s'\n",
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
    if (!first) continue; /* blank or comment-only line */
    int a = axis_index(first);
    if (a < 0) {
      fprintf(stderr, "dt_bcjr_metrics: %s:%d: bad axis '%s'\n", path,
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
        fprintf(stderr, "dt_bcjr_metrics: %s:%d: bad rate '%s'\n", path,
                lineno, t);
        ok = 0;
        break;
      }
      if (n == cap) {
        cap *= 2;
        double *grown = realloc(rates, (size_t)cap * sizeof(double));
        if (!grown) {
          fprintf(stderr, "dt_bcjr_metrics: out of memory\n");
          exit(1);
        }
        rates = grown;
      }
      rates[n++] = value;
    }
    if (!ok) {
      free(rates);
      break;
    }
    if (n == 0) {
      fprintf(stderr, "dt_bcjr_metrics: %s:%d: grid has no rates\n", path,
              lineno);
      free(rates);
      ok = 0;
      break;
    }
    free(g_grids[a].rates); /* last definition wins */
    g_grids[a].rates = rates;
    g_grids[a].count = n;
  }
  fclose(f);
  return ok ? 0 : -1;
}

/* Per-trial partial sums; summed across a point's trials to form its CSV row.
 */
typedef struct {
  long long edits, ref_bits;
} trial_result;

/* Run one Monte-Carlo trial for a (code, axis, rate) point with its own PRNG
 * stream seeded from `seed`. Each trial is fully independent, so trials
 * parallelize across cores (see main). Drives the public encoder/decoder. */
static trial_result run_one_trial(const dt_ccode *code, axis channel_axis,
                                  double rate, int info_bits, uint64_t seed) {
  uint64_t rng_state = seed;
  uint64_t *rng = &rng_state;
  const int code_n = dt_ccode_n(code), constraint_len = dt_ccode_k(code);
  const double p_flip = channel_axis == AXIS_FLIP ? rate : 0.0;
  const double p_erase = channel_axis == AXIS_ERASE ? rate : 0.0;

  /* Matched decoder model: anticipate the channel by setting the swept
   * impairment's probability to its rate (floored into (0, 1)); the inactive one
   * sits at a small floor (p_flip) or zero (p_erase). decision_depth ~ 6 * K. */
  const int decision_depth = 6 * constraint_len;
  const double min_prob = 1e-3, max_prob = 1.0 - 1e-3;
  const dt_bcjr_stream_params params = {
      .decision_depth = decision_depth,
      .p_flip = channel_axis == AXIS_FLIP ? clamp_double(rate, min_prob, max_prob)
                                          : 0.005,
      .p_erase =
          channel_axis == AXIS_ERASE ? clamp_double(rate, 0.0, max_prob) : 0.0,
  };

  uint8_t *message = xmalloc((size_t)info_bits);
  for (int i = 0; i < info_bits; ++i) {
    message[i] = bit_sym((unsigned int)rng_next(rng));
  }

  /* Encode with the full encoder: begin, then encode, then finalize (flush).
   */
  const int coded_cap = (info_bits + constraint_len) * code_n;
  uint8_t *coded = xmalloc((size_t)coded_cap);
  dt_encoder *encoder = dt_cc_full_encoder_create(code);
  if (!encoder) {
    fprintf(stderr, "dt_bcjr_metrics: encoder create failed\n");
    exit(1);
  }
  int coded_len = encoder->begin(encoder, coded, (size_t)coded_cap);
  coded_len += encoder->encode(encoder, coded + coded_len,
                               (size_t)(coded_cap - coded_len), message,
                               (size_t)info_bits);
  coded_len += encoder->finalize(encoder, coded + coded_len,
                                 (size_t)(coded_cap - coded_len));
  dt_cc_full_encoder_destroy(encoder);

  uint8_t *received = xmalloc((size_t)coded_len);
  apply_channel(coded, coded_len, p_flip, p_erase, rng, received);

  /* Decode through the public API, feeding in 64-bit chunks then flushing. */
  const int decoded_cap = info_bits + 256;
  uint8_t *decoded = xmalloc((size_t)decoded_cap);
  dt_decoder *dec = dt_bcjr_decoder_create(code, &params);
  if (!dec) {
    fprintf(stderr, "dt_bcjr_metrics: decoder create failed\n");
    exit(1);
  }
  int n_decoded = dec->begin(dec, decoded, (size_t)decoded_cap);
  for (int read_pos = 0; read_pos < coded_len && n_decoded < decoded_cap;) {
    int chunk = coded_len - read_pos < 64 ? coded_len - read_pos : 64;
    int w =
        dec->decode(dec, decoded + n_decoded, (size_t)(decoded_cap - n_decoded),
                    received + read_pos, (size_t)chunk);
    if (w < 0) {
      n_decoded = w;
      break;
    }
    n_decoded += w;
    read_pos += chunk;
  }
  if (n_decoded >= 0) {
    int tail = dec->finalize(dec, decoded + n_decoded,
                             (size_t)(decoded_cap - n_decoded));
    n_decoded = tail < 0 ? tail : n_decoded + tail;
  }
  dt_bcjr_decoder_destroy(dec);
  free(received);
  if (n_decoded < 0) {
    fprintf(stderr, "dt_bcjr_metrics: decode error %d\n", n_decoded);
    exit(1);
  }

  /* Drop the blind-acquisition warm-up (from both sequences) and trim the
   * constraint_len-1 flush bits off the decoded tail, then compare. */
  const int warmup = decision_depth;
  int decoded_end = n_decoded - (constraint_len - 1);
  if (decoded_end < warmup) {
    decoded_end = warmup;
  }
  int decoded_len = decoded_end - warmup;
  int ref_len = info_bits - warmup;
  trial_result r = {0, 0};
  if (ref_len > 0) {
    int *dp_prev = xmalloc((size_t)(decoded_cap + 1) * sizeof(int));
    int *dp_cur = xmalloc((size_t)(decoded_cap + 1) * sizeof(int));
    r.edits = edit_distance(decoded + warmup, decoded_len, message + warmup,
                            ref_len, dp_prev, dp_cur);
    r.ref_bits = ref_len;
    free(dp_prev);
    free(dp_cur);
  }

  free(message);
  free(coded);
  free(decoded);
  return r;
}

/* Format the CSV row for a point from its summed trial results. */
static void format_row(char *out, size_t cap, const char *code_name,
                       axis channel_axis, double rate, int trials,
                       trial_result agg) {
  double edit_rate =
      agg.ref_bits > 0 ? (double)agg.edits / (double)agg.ref_bits : 0.0;
  snprintf(out, cap, "%s,edit,%s,%.6g,%d,%lld,%lld,%.8g\n", code_name,
           AXIS_NAME[channel_axis], rate, trials, agg.ref_bits, agg.edits,
           edit_rate);
}

int main(int argc, char **argv) {
  int trials = argc > 1 ? atoi(argv[1]) : 50;
  int info_bits = argc > 2 ? atoi(argv[2]) : 1000;
  uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 0xC0FFEEULL;
  if (trials < 1 || info_bits < 1) {
    fprintf(stderr,
            "usage: %s [trials>=1] [info_bits>=1] [seed] [rate_grids_file]\n",
            argv[0]);
    return 2;
  }

  /* Channel rate grids are read from a file (default
   * metrics/bcjr/rate_grids.txt, overridden by a 4th argument) so a sweep
   * can be retuned without recompiling. */
  const char *grids_path =
      argc > 4 ? argv[4] : "metrics/bcjr/rate_grids.txt";
  if (load_grids(grids_path) < 0) {
    return 2;
  }

  /* The trellis tables in a dt_ccode are read-only once built, so all threads
   * share the four codes; each decode allocates its own decoder state. */
  dt_ccode *codes[N_CODES];
  for (int code_idx = 0; code_idx < N_CODES; ++code_idx) {
    codes[code_idx] = dt_ccode_create_standard(CODES[code_idx].which);
    if (!codes[code_idx]) {
      fprintf(stderr, "dt_bcjr_metrics: code create failed\n");
      return 1;
    }
  }

  /* Each (code, axis, rate) point is an independent work item. */
  int n_points = 0;
  for (int axis_idx = 0; axis_idx < N_AXES; ++axis_idx) {
    if (g_grids[axis_idx].count == 0) {
      fprintf(stderr, "dt_bcjr_metrics: %s: no grid for axis %s\n",
              grids_path, AXIS_NAME[axis_idx]);
      return 2;
    }
    n_points += N_CODES * g_grids[axis_idx].count;
  }

  typedef struct {
    int code_idx;
    axis channel_axis;
    double rate;
  } work_item;
  work_item *items = xmalloc((size_t)n_points * sizeof(*items));
  int filled = 0;
  for (int code_idx = 0; code_idx < N_CODES; ++code_idx) {
    for (int axis_idx = 0; axis_idx < N_AXES; ++axis_idx) {
      const rate_grid *g = &g_grids[axis_idx];
      for (int rate_idx = 0; rate_idx < g->count; ++rate_idx) {
        items[filled].code_idx = code_idx;
        items[filled].channel_axis = (axis)axis_idx;
        items[filled].rate = g->rates[rate_idx];
        ++filled;
      }
    }
  }

#ifdef _OPENMP
  fprintf(stderr, "running %d points x %d trials on %d threads ...\n", n_points,
          trials, omp_get_max_threads());
#else
  fprintf(stderr, "running %d points x %d trials (single-threaded) ...\n",
          n_points, trials);
#endif

  printf("code,metric,axis,rate,trials,ref_bits,edit_distance,edit_rate\n");
  fflush(stdout);

  /* Fan every (point, trial) out as an independent task (collapse(2)). Each
   * trial owns a seed derived from its flat (point, trial) index, so values are
   * reproducible regardless of thread count. The task that finishes a point's
   * last trial (tracked by an atomic counter) sums the slots in trial order and
   * streams that point's row out (one writer at a time). */
  trial_result *results =
      xmalloc((size_t)n_points * (size_t)trials * sizeof(*results));
  int *remaining = xmalloc((size_t)n_points * sizeof(int));
  for (int point = 0; point < n_points; ++point) {
    remaining[point] = trials;
  }

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) collapse(2)
#endif
  for (int point = 0; point < n_points; ++point) {
    for (int trial = 0; trial < trials; ++trial) {
      const work_item item = items[point];
      results[(size_t)point * trials + trial] =
          run_one_trial(codes[item.code_idx], item.channel_axis, item.rate,
                        info_bits, derive_seed(seed, point * trials + trial));

      int left;
#ifdef _OPENMP
#pragma omp atomic capture
#endif
      {
        remaining[point] -= 1;
        left = remaining[point];
      }
      if (left == 0) {
        trial_result agg = {0, 0};
        for (int t = 0; t < trials; ++t) {
          const trial_result *s = &results[(size_t)point * trials + t];
          agg.edits += s->edits;
          agg.ref_bits += s->ref_bits;
        }
        char row[256];
        format_row(row, sizeof(row), CODES[item.code_idx].name,
                   item.channel_axis, item.rate, trials, agg);
#ifdef _OPENMP
#pragma omp critical
#endif
        {
          fputs(row, stdout);
          fflush(stdout);
        }
      }
    }
  }

  free(results);
  free(remaining);
  free(items);
  for (int axis_idx = 0; axis_idx < N_AXES; ++axis_idx) {
    free(g_grids[axis_idx].rates);
  }
  for (int code_idx = 0; code_idx < N_CODES; ++code_idx) {
    dt_ccode_destroy(codes[code_idx]);
  }
  return 0;
}
