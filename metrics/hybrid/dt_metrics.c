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
 * dt_metrics - Monte-Carlo measurement of decoding-mistake rate as a function
 * of the channel's flip / insert / delete / erase rates, for each standard code.
 *
 * For one data point we: generate a random message, encode it (with a flush so
 * the message bits sit in the stream interior), pass the coded bits through a
 * channel that independently flips, inserts, deletes, and erases (marks lost)
 * bits, stream-decode, and count how many decoded bits disagree with the
 * original message.
 *
 * The reported metric is the normalized EDIT (Levenshtein) distance between the
 * decoded bits and the original message, divided by the number of message bits.
 * Edit distance counts the substitutions, insertions, and deletions needed to
 * turn one into the other, so a single uncorrected sync slip costs one edit
 * rather than misaligning (and thus mis-scoring) the whole remaining stream -
 * the right metric when the channel itself inserts and deletes bits. A first
 * `warmup` bits are dropped from both sequences to skip the decoder's
 * blind-acquisition transient, and the trailing flush bits are trimmed off.
 *
 * Each axis (flip, insert, delete) is swept independently with the other two
 * rates held at zero, so each curve isolates one channel impairment.
 *
 * Alongside the edit rate we report a confidence metric in [0, 1]:
 *
 *   mean_lock   - the decoder's own running estimate (the soft decoder's
 *                 c_locked) that it is tracking a valid coded stream,
 *                 averaged across the kept bits. It shows how confidently the
 *                 decoder stays synced as each impairment ramps up.
 *
 * Output is CSV on stdout (see header row); feed it to metrics/hybrid/plot_metrics.py.
 *
 * Usage: dt_metrics [trials] [info_bits] [seed]
 */

#include <drifty/cc/encoders.h>
#include <drifty/cc/hybrid.h>

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
    fprintf(stderr, "dt_metrics: out of memory\n");
    exit(1);
  }
  return ptr;
}

/* Clamp a probability into [lo, hi]. The decoder needs every rate strictly inside
 * (0, 1), so the matched model floors them above 0 and caps the active one below
 * 1 at the top of a sweep. */
static double clamp_double(double value, double lo, double hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

/* Apply the channel to `coded` (coded_len bits): with probability p_ins emit a
 * random bit before each coded bit, with probability p_del drop the coded bit,
 * otherwise emit it flipped with probability p_flip and then, with probability
 * p_erase, marked DT_ERASURE (received but known-lost). Returns the received
 * length and stores a freshly malloc'd buffer in *out_received. */
static int apply_channel(const uint8_t *coded, int coded_len, double p_flip,
                         double p_ins, double p_del, double p_erase,
                         uint64_t *rng, uint8_t **out_received) {
  int capacity = coded_len + coded_len / 4 + 64;
  uint8_t *received = xmalloc((size_t)capacity);
  int received_len = 0;
  for (int i = 0; i < coded_len; ++i) {
    if (received_len + 2 > capacity) {
      capacity *= 2;
      uint8_t *grown = realloc(received, (size_t)capacity);
      if (!grown) {
        free(received);
        fprintf(stderr, "dt_metrics: out of memory\n");
        exit(1);
      }
      received = grown;
    }
    if (p_ins > 0.0 && rng_unit(rng) < p_ins) {
      received[received_len++] = bit_sym((unsigned int)rng_next(rng));
    }
    if (p_del > 0.0 && rng_unit(rng) < p_del) {
      continue;
    }
    uint8_t bit = coded[i];
    if (p_flip > 0.0 && rng_unit(rng) < p_flip) {
      bit ^= DT_VALUE; /* toggle the value bit: DT_TRUE <-> DT_FALSE */
    }
    if (p_erase > 0.0 && rng_unit(rng) < p_erase) {
      bit = DT_ERASURE;
    }
    received[received_len++] = bit;
  }
  *out_received = received;
  return received_len;
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

  /* Row 0: prefix of seq_b reached by insertions, but only inside the band; the
   * cell just past it (read as the next row's right boundary) is INF. */
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
      int subst_cost = prev[col - 1] + (seq_a[row - 1] != seq_b[col - 1] ? 1 : 0);
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
 * length). The true distance never exceeds max(len_a, len_b), which bounds the
 * worst case. */
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
#define N_AXES (AXIS_ERASE + 1)

/* The metrics measured at each point. Run length is derived from the edit rate,
 * so it shares edit's grid and is not a separate entry here. */
typedef enum { METRIC_EDIT, METRIC_LOCK } metric;
static const char *METRIC_NAME[] = {"edit", "lock"};
#define N_METRICS ((int)(sizeof(METRIC_NAME) / sizeof(METRIC_NAME[0])))

/* What a run measures, selected by the command-line argument. The three decoding
 * variations measure edit and lock with a decoder whose channel model is either
 * pegged (every probability fixed at 1%, the channel met cold - the decoder
 * cannot anticipate it) or matched (the active impairment's probability set to
 * the swept rate - the decoder anticipates the channel). overmatched reuses the
 * matched model but runs it against a CLEAN channel, so its swept rate is what the
 * decoder *expects* rather than what the channel does: it measures the penalty of
 * an over-pessimistic decoder when nothing is actually wrong. The decoding
 * variations use rate grids tuned to their curves; overmatched's run almost to 1,
 * since the decoder copes on clean data until its expected rate gets extreme. */
typedef enum { VAR_PEGGED, VAR_MATCHED, VAR_OVERMATCHED } variation;

/* Channel rate grids - one per (variation, metric, impairment) - are read at
 * startup from a text file (load_grids; default metrics/hybrid/rate_grids.txt) rather
 * than hard-coded, so a sweep can be retuned without recompiling. Each grid is
 * sampled only over the range where its metric carries information on that axis,
 * with points clustered where the curve bends; the shipped file documents the
 * shapes. */
typedef struct {
  double *rates;
  int count;
} rate_grid;

#define N_VARIATIONS (VAR_OVERMATCHED + 1)
static rate_grid g_grids[N_VARIATIONS][N_METRICS][N_AXES];

/* Index of `name` in `names` (length n), or -1 if absent. */
static int name_index(const char *name, const char *const *names, int n) {
  for (int i = 0; i < n; ++i) {
    if (strcmp(name, names[i]) == 0) {
      return i;
    }
  }
  return -1;
}

/* Map a variation name (canonical, or the untuned/tuned aliases) to its enum, or
 * -1 if unrecognised. Shared by load_grids and the command-line parsing. */
static int parse_variation(const char *s) {
  if (strcmp(s, "pegged") == 0 || strcmp(s, "untuned") == 0) return VAR_PEGGED;
  if (strcmp(s, "matched") == 0 || strcmp(s, "tuned") == 0) return VAR_MATCHED;
  if (strcmp(s, "overmatched") == 0) return VAR_OVERMATCHED;
  return -1;
}

/* Load the rate grids from `path` into g_grids. Each non-blank, non-comment line
 * is "<variation> <metric> <axis>  <rate> <rate> ..."; '#' begins a comment.
 * Returns 0 on success, -1 after printing a diagnostic. */
static int load_grids(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "dt_metrics: cannot open rate-grid file '%s'\n", path);
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
    const char *mn = strtok(NULL, " \t\r\n");
    const char *an = strtok(NULL, " \t\r\n");
    int v = parse_variation(first);
    int m = mn ? name_index(mn, METRIC_NAME, N_METRICS) : -1;
    int a = an ? name_index(an, AXIS_NAME, N_AXES) : -1;
    if (v < 0 || m < 0 || a < 0) {
      fprintf(stderr, "dt_metrics: %s:%d: bad variation/metric/axis\n", path,
              lineno);
      ok = 0;
      break;
    }
    int cap = 16, n = 0;
    double *rates = xmalloc((size_t)cap * sizeof(double));
    for (char *t = strtok(NULL, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
      char *end;
      double value = strtod(t, &end);
      if (*end != '\0') {
        fprintf(stderr, "dt_metrics: %s:%d: bad rate '%s'\n", path, lineno, t);
        ok = 0;
        break;
      }
      if (n == cap) {
        cap *= 2;
        double *grown = realloc(rates, (size_t)cap * sizeof(double));
        if (!grown) {
          fprintf(stderr, "dt_metrics: out of memory\n");
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
      fprintf(stderr, "dt_metrics: %s:%d: grid has no rates\n", path, lineno);
      free(rates);
      ok = 0;
      break;
    }
    free(g_grids[v][m][a].rates); /* last definition wins */
    g_grids[v][m][a].rates = rates;
    g_grids[v][m][a].count = n;
  }
  fclose(f);
  return ok ? 0 : -1;
}

static const double *metric_axis_rates(variation var, metric which_metric,
                                       axis channel_axis, int *count) {
  const rate_grid *g = &g_grids[var][which_metric][channel_axis];
  *count = g->count;
  return g->rates;
}

/* The decoder model and measurement windows for one point - fixed by the code,
 * axis, rate, and variation but not the trial, so both the trial worker and the
 * row formatter derive them the same way from make_model(). */
typedef struct {
  dt_cc_hybrid_stream_params params;
  int code_n;
  int constraint_len;
  int decision_depth;
  int max_drift;
  int warmup;
} point_model;

/* Per-trial partial sums; summed across a point's trials to form its CSV row. */
typedef struct {
  long long edits, ref_bits, lock_bits;
  double lock_sum;
} trial_result;

static point_model make_model(const dt_cc_code *code, axis channel_axis,
                              double rate, variation var) {
  point_model m;
  m.code_n = dt_cc_code_n(code);
  m.constraint_len = dt_cc_code_k(code);
  m.decision_depth = 8 * m.constraint_len;

  if (var != VAR_MATCHED && var != VAR_OVERMATCHED) {
    /* Channel-agnostic model: every impairment pegged at a flat 1%, regardless
     * of which axis is swept or how high its rate runs, with drift tracking
     * always on. The decoder is never told what the channel is doing, so at low
     * rates it sees roughly what it expects and as a rate climbs it meets
     * something increasingly unexpected - the stress this variation measures. */
    const double pegged = 0.01;
    m.max_drift = 8;
    m.params = (dt_cc_hybrid_stream_params){
        .decision_depth = m.decision_depth,
        .max_drift = m.max_drift,
        .p_flip = pegged,
        .p_ins_true = pegged * 0.5,
        .p_ins_false = pegged * 0.5,
        .p_del = pegged,
        .p_ovr_erase = pegged,
    };
  } else {
    /* Matched model: the decoder anticipates the channel, setting the active
     * impairment's probability to the swept rate and tracking drift only when
     * that impairment is an indel. The inactive probabilities sit at a small
     * floor; the decoder needs every rate strictly inside (0, 1) and
     * p_ins + p_del < 1, so the active rate is clamped just shy of those bounds
     * at the high end. matched shows the decoder's best case; overmatched reuses
     * this exact model but run_one_trial feeds it a clean channel instead. */
    const double min_prob = 1e-3;
    const double max_prob = 1.0 - min_prob;
    const double drift_max = 1.0 - 2.0 * min_prob;
    const double channel_ins = channel_axis == AXIS_INSERT ? rate : 0.0;
    const double channel_del = channel_axis == AXIS_DELETE ? rate : 0.0;
    m.max_drift =
        (channel_axis == AXIS_INSERT || channel_axis == AXIS_DELETE) ? 8 : 0;
    /* The channel inserts uniformly-random 0/1 bits, so split the total
     * insertion rate evenly across the true/false components. */
    const double ins_total = (m.max_drift > 0)
                                 ? clamp_double(channel_ins, min_prob, drift_max)
                                 : 0.0;
    m.params = (dt_cc_hybrid_stream_params){
        .decision_depth = m.decision_depth,
        .max_drift = m.max_drift,
        .p_flip = (channel_axis == AXIS_FLIP)
                     ? clamp_double(rate, min_prob, max_prob)
                     : 0.005,
        .p_ins_true = ins_total * 0.5,
        .p_ins_false = ins_total * 0.5,
        .p_del = (m.max_drift > 0)
                     ? clamp_double(channel_del, min_prob, drift_max)
                     : 0.0,
        .p_ovr_erase = (channel_axis == AXIS_ERASE)
                       ? clamp_double(rate, min_prob, max_prob)
                       : 0.0,
    };
  }
  m.warmup = m.decision_depth;
  return m;
}

/* Run one Monte-Carlo trial for a (code, axis, metric, rate) point with its own
 * PRNG stream seeded from `seed`, returning that trial's partial sums. Edit
 * distance and lock probability both come from a single decode; the metric
 * selects which post-processing the trial does and each skips the other's. Each
 * trial is fully independent, so trials parallelize across cores (see main). */
static trial_result run_one_trial(const dt_cc_code *code, axis channel_axis,
                                  metric which_metric, double rate,
                                  int info_bits, uint64_t seed,
                                  variation var) {
  uint64_t rng_state = seed;
  uint64_t *rng = &rng_state;
  const point_model m = make_model(code, channel_axis, rate, var);

  /* Channel rates: only the active axis is nonzero - except overmatched, which
   * runs a clean channel (the swept rate drives the decoder's model, not the
   * channel), so every channel rate stays zero. */
  const int clean = var == VAR_OVERMATCHED;
  const double channel_sub = (!clean && channel_axis == AXIS_FLIP) ? rate : 0.0;
  const double channel_ins = (!clean && channel_axis == AXIS_INSERT) ? rate : 0.0;
  const double channel_del = (!clean && channel_axis == AXIS_DELETE) ? rate : 0.0;
  const double channel_erase =
      (!clean && channel_axis == AXIS_ERASE) ? rate : 0.0;

  uint8_t *message = xmalloc((size_t)info_bits);
  uint8_t *coded = xmalloc((size_t)((info_bits + m.constraint_len) * m.code_n));

  trial_result r = {0, 0, 0, 0.0};
  for (int i = 0; i < info_bits; ++i) {
    message[i] = bit_sym((unsigned int)rng_next(rng));
  }
  /* Encode with the encoder: begin, then encode, then finalize
   * (which writes the flush tail). */
  const int coded_cap = (info_bits + m.constraint_len) * m.code_n;
  dt_encoder *encoder = dt_cc_encoder_create(code);
  if (!encoder) {
    fprintf(stderr, "dt_metrics: encoder create failed\n");
    exit(1);
  }
  int coded_len = encoder->begin(encoder, coded, (size_t)coded_cap);
  coded_len += encoder->encode(encoder, coded + coded_len,
                               (size_t)(coded_cap - coded_len), message,
                               (size_t)info_bits);
  coded_len += encoder->finalize(encoder, coded + coded_len,
                                 (size_t)(coded_cap - coded_len));
  dt_cc_encoder_destroy(encoder);

  uint8_t *received = NULL;
  int received_len = apply_channel(coded, coded_len, channel_sub, channel_ins,
                                   channel_del, channel_erase, rng, &received);

  /* Decode through the public API. Edit distance needs the hard decoder's bits;
   * lock probability needs the soft decoder's per-bit c_locked - allocate and
   * run only what this metric uses. For lock, only the streaming bits carry a
   * value, so n_stream tracks how many `decode` produced (vs the flush tail from
   * `finalize`). */
  const int decoded_cap = info_bits + 256;
  uint8_t *decoded = NULL;
  dt_soft_decoder_out *soft = NULL;
  int n_stream = 0, n_decoded;
  if (which_metric == METRIC_EDIT) {
    decoded = xmalloc((size_t)decoded_cap);
    dt_decoder *dec = dt_cc_hybrid_decoder_create(code, &m.params);
    if (!dec) {
      fprintf(stderr, "dt_metrics: decoder create failed\n");
      exit(1);
    }
    /* Feed in 64-bit chunks (the decoder's output depends on feed granularity),
     * collecting streaming bits, then flush the tail. */
    for (int read_pos = 0; read_pos < received_len && n_stream < decoded_cap;) {
      int chunk = received_len - read_pos < 64 ? received_len - read_pos : 64;
      int w = dec->decode(dec, decoded + n_stream, (size_t)(decoded_cap - n_stream),
                          received + read_pos, (size_t)chunk);
      if (w < 0) {
        n_stream = w;
        break;
      }
      n_stream += w;
      read_pos += chunk;
    }
    n_decoded = n_stream;
    if (n_decoded >= 0) {
      int tail = dec->finalize(dec, decoded + n_decoded,
                               (size_t)(decoded_cap - n_decoded));
      n_decoded = tail < 0 ? tail : n_decoded + tail;
    }
    dt_cc_hybrid_decoder_destroy(dec);
  } else { /* METRIC_LOCK */
    soft = xmalloc((size_t)decoded_cap * sizeof(*soft));
    dt_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(code, &m.params);
    if (!sd) {
      fprintf(stderr, "dt_metrics: decoder create failed\n");
      exit(1);
    }
    for (int read_pos = 0; read_pos < received_len && n_stream < decoded_cap;) {
      int chunk = received_len - read_pos < 64 ? received_len - read_pos : 64;
      int w = sd->decode(sd, soft + n_stream, (size_t)(decoded_cap - n_stream),
                         received + read_pos, (size_t)chunk);
      if (w < 0) {
        n_stream = w;
        break;
      }
      n_stream += w;
      read_pos += chunk;
    }
    n_decoded = n_stream;
    if (n_decoded >= 0) {
      int tail = sd->finalize(sd, soft + n_decoded,
                              (size_t)(decoded_cap - n_decoded));
      n_decoded = tail < 0 ? tail : n_decoded + tail;
    }
    dt_cc_hybrid_soft_decoder_destroy(sd);
  }
  free(received);
  if (n_decoded < 0) {
    fprintf(stderr, "dt_metrics: decode error %d\n", n_decoded);
    exit(1);
  }

  /* Compare against the message after dropping the warm-up transient (from both)
   * and trimming the constraint_len-1 flush bits off the tail. */
  int decoded_end = n_decoded - (m.constraint_len - 1);
  if (decoded_end < m.warmup) {
    decoded_end = m.warmup;
  }
  int decoded_len = decoded_end - m.warmup;
  int ref_len = info_bits - m.warmup;
  if (ref_len > 0 && which_metric == METRIC_EDIT) {
    int *dp_prev = xmalloc((size_t)(decoded_cap + 1) * sizeof(int));
    int *dp_cur = xmalloc((size_t)(decoded_cap + 1) * sizeof(int));
    r.edits = edit_distance(decoded + m.warmup, decoded_len, message + m.warmup,
                            ref_len, dp_prev, dp_cur);
    r.ref_bits = ref_len;
    free(dp_prev);
    free(dp_cur);
  } else if (ref_len > 0) { /* METRIC_LOCK */
    /* Average the decoder's lock probability over the same window. Only
     * streaming bits carry a lock value, so stop at n_stream (the flush tail,
     * already trimmed from the edit window, has none). */
    int lock_end = decoded_end < n_stream ? decoded_end : n_stream;
    for (int i = m.warmup; i < lock_end; ++i) {
      r.lock_sum += soft[i].c_locked;
      ++r.lock_bits;
    }
  }

  free(message);
  free(coded);
  free(decoded);
  free(soft);
  return r;
}

/* Format the CSV row for a point from its summed trial results. A point measures
 * one metric, so only that metric's value columns are filled; the others are
 * left blank for the plotter to skip. The trailing columns are, in order:
 * ref_bits, edit_distance, edit_rate, lock_bits, mean_lock. */
static void format_row(char *out, size_t cap, const char *code_name,
                       metric which_metric, axis channel_axis, double rate,
                       const point_model *m, int trials, trial_result agg) {
  char head[192];
  snprintf(head, sizeof(head), "%s,%s,%s,%.6g,%.6g,%.6g,%.6g,%.6g,%d,%d,%d",
           code_name, METRIC_NAME[which_metric], AXIS_NAME[channel_axis], rate,
           m->params.p_flip, m->params.p_ins_true + m->params.p_ins_false,
           m->params.p_del, m->params.p_ovr_erase,
           m->decision_depth, m->max_drift, trials);
  if (which_metric == METRIC_EDIT) {
    double edit_rate =
        agg.ref_bits > 0 ? (double)agg.edits / (double)agg.ref_bits : 0.0;
    snprintf(out, cap, "%s,%lld,%lld,%.8g,,\n", head, agg.ref_bits, agg.edits,
             edit_rate);
  } else { /* METRIC_LOCK */
    double mean_lock =
        agg.lock_bits > 0 ? agg.lock_sum / (double)agg.lock_bits : 0.0;
    /* Leave ref_bits, edit_distance, edit_rate (3 columns) blank so lock_bits
     * and mean_lock land in their header columns - four commas, not three. */
    snprintf(out, cap, "%s,,,,%lld,%.8g\n", head, agg.lock_bits, mean_lock);
  }
}

int main(int argc, char **argv) {
  /* Many short trials beat one long stream here: the streams self-synchronize,
   * so a single long run would mostly re-measure the same steady state, while
   * independent trials each pay a fresh blind acquisition and sample the
   * heavy-tailed lose-lock events near each code's knee. So we default to a high
   * trial count over a modest message length. */
  int trials = argc > 1 ? atoi(argv[1]) : 50;
  int info_bits = argc > 2 ? atoi(argv[2]) : 1000;
  uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 0xC0FFEEULL;
  if (trials < 1 || info_bits < 1) {
    fprintf(stderr,
            "usage: %s [trials>=1] [info_bits>=1] [seed] "
            "[variation=pegged|matched|overmatched] [rate_grids_file]\n",
            argv[0]);
    return 2;
  }

  /* What to measure. The decoding variations (pegged, matched, overmatched)
   * measure edit and lock; pegged/matched accept the untuned/tuned aliases. */
  variation var = VAR_PEGGED;
  if (argc > 4) {
    int parsed = parse_variation(argv[4]);
    if (parsed < 0) {
      fprintf(stderr,
              "dt_metrics: unknown variation '%s' "
              "(use pegged|matched|overmatched)\n",
              argv[4]);
      return 2;
    }
    var = (variation)parsed;
  }

  /* Channel rate grids are read from a file (default metrics/hybrid/rate_grids.txt,
   * overridden by a 5th argument) so a sweep can be retuned without recompiling. */
  const char *grids_path = argc > 5 ? argv[5] : "metrics/hybrid/rate_grids.txt";
  if (load_grids(grids_path) < 0) {
    return 2;
  }

  /* Metrics for every variation: edit distance and lock probability. */
  const metric run_metrics[] = {METRIC_EDIT, METRIC_LOCK};
  const int n_run_metrics = (int)(sizeof(run_metrics) / sizeof(run_metrics[0]));

  /* The trellis tables in a dt_cc_code are read-only once built, so all threads
   * share the four codes; each decode allocates its own decoder state. */
  dt_cc_code *codes[N_CODES];
  for (int code_idx = 0; code_idx < N_CODES; ++code_idx) {
    codes[code_idx] = dt_cc_code_create_standard(CODES[code_idx].which);
    if (!codes[code_idx]) {
      fprintf(stderr, "dt_metrics: code create failed\n");
      return 1;
    }
  }

  /* Each (code, axis, rate) point is an independent work item. Axes use
   * different rate grids, so we enumerate the combinations explicitly into a
   * list rather than decomposing a flat index. */
  const int n_axes = N_AXES;
  int n_points = 0;
  for (int mi = 0; mi < n_run_metrics; ++mi) {
    for (int axis_idx = 0; axis_idx < n_axes; ++axis_idx) {
      int count;
      metric_axis_rates(var, run_metrics[mi], (axis)axis_idx, &count);
      if (count == 0) {
        fprintf(stderr, "dt_metrics: %s: no grid for %s %s %s\n", grids_path,
                argc > 4 ? argv[4] : "pegged", METRIC_NAME[run_metrics[mi]],
                AXIS_NAME[axis_idx]);
        return 2;
      }
      n_points += N_CODES * count;
    }
  }

  typedef struct {
    int code_idx;
    axis channel_axis;
    metric which_metric;
    double rate;
  } work_item;
  work_item *items = xmalloc((size_t)n_points * sizeof(*items));
  int filled = 0;
  for (int code_idx = 0; code_idx < N_CODES; ++code_idx) {
    for (int mi = 0; mi < n_run_metrics; ++mi) {
      for (int axis_idx = 0; axis_idx < n_axes; ++axis_idx) {
        int count;
        const double *rates =
            metric_axis_rates(var, run_metrics[mi], (axis)axis_idx, &count);
        for (int rate_idx = 0; rate_idx < count; ++rate_idx) {
          items[filled].code_idx = code_idx;
          items[filled].channel_axis = (axis)axis_idx;
          items[filled].which_metric = run_metrics[mi];
          items[filled].rate = rates[rate_idx];
          ++filled;
        }
      }
    }
  }

  const char *var_name = var == VAR_MATCHED       ? "matched"
                         : var == VAR_OVERMATCHED ? "overmatched"
                                                  : "pegged";
#ifdef _OPENMP
  fprintf(stderr, "running %d points x %d trials (%s) on %d threads ...\n",
          n_points, trials, var_name, omp_get_max_threads());
#else
  fprintf(stderr, "running %d points x %d trials (%s, single-threaded) ...\n",
          n_points, trials, var_name);
#endif

  printf(
      "code,metric,axis,rate,dec_p_flip,dec_p_ins,dec_p_del,dec_p_ovr_erase,"
      "decision_depth,max_drift,trials,ref_bits,edit_distance,edit_rate,"
      "lock_bits,mean_lock\n");
  fflush(stdout);

  /* Fan every (point, trial) out as an independent task (collapse(2)), so a
   * single slow point's trials spread across all cores instead of pinning one -
   * the slow drift points would otherwise bound wall-clock. Each trial owns a
   * seed derived from its flat (point, trial) index, so values are reproducible
   * regardless of thread count. Trials write into their own result slots; the
   * task that finishes a point's last trial (tracked by an atomic counter) sums
   * the slots in trial order - keeping mean_lock bit-exact, free of
   * reduction-order wobble - and streams that point's row out (one writer at a
   * time; flush so the file grows as the sweep runs). */
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
          run_one_trial(codes[item.code_idx], item.channel_axis,
                        item.which_metric, item.rate, info_bits,
                        derive_seed(seed, point * trials + trial), var);

      int left;
#ifdef _OPENMP
#pragma omp atomic capture
#endif
      {
        remaining[point] -= 1;
        left = remaining[point];
      }
      if (left == 0) {
        const point_model m =
            make_model(codes[item.code_idx], item.channel_axis, item.rate, var);
        trial_result agg = {0, 0, 0, 0.0};
        for (int t = 0; t < trials; ++t) {
          const trial_result *s = &results[(size_t)point * trials + t];
          agg.edits += s->edits;
          agg.ref_bits += s->ref_bits;
          agg.lock_bits += s->lock_bits;
          agg.lock_sum += s->lock_sum;
        }
        char row[256];
        format_row(row, sizeof(row), CODES[item.code_idx].name,
                   item.which_metric, item.channel_axis, item.rate, &m, trials,
                   agg);
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
  for (int code_idx = 0; code_idx < N_CODES; ++code_idx) {
    dt_cc_code_destroy(codes[code_idx]);
  }
  return 0;
}
