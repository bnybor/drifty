/*
 * 13 - channel.c: the time-varying channel (see channel.h). Mirrors a receiver's view
 * of a channel whose condition changes over time.
 */

#include "channel.h"

#include "util.h"

#define WARM 256 /* drift-free bit-sync preamble: the inner decoder acquires lock here
                  * before any timing drift accumulates. */
#define FCAP 4096
#define GAP 4096
#define JUNK 4096

int channel_stream_cap(const dt_cc_code *code) {
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  return 6 * ((FCAP + WARM + K) * n + 512);
}

/* Frame payload A/B, prepend the sync preamble, and cc-encode into `coded`. */
static int encode_signal(const dt_cc_code *code, unsigned char pay[][RS_MSG],
                         dt_bit *framed, dt_bit *info, dt_bit *coded, int mcap) {
  int fl = stack_build_framed(pay, FRAMES, framed, FCAP);
  for (int i = 0; i < WARM; ++i) {
    info[i] = DT_FALSE;
  }
  memcpy(info + WARM, framed, (size_t)fl);
  return ex_encode(code, info, WARM + fl, coded, mcap);
}

int channel_build(const dt_cc_code *code, unsigned char payA[][RS_MSG],
                  unsigned char payB[][RS_MSG], uint64_t *rng, dt_bit *out, int cap,
                  struct channel_regions *r) {
  const int n = dt_cc_code_n(code), K = dt_cc_code_k(code);
  const int mcap = (FCAP + WARM + K) * n + 512;
  dt_bit *framed = malloc((size_t)FCAP);
  dt_bit *info = malloc((size_t)(FCAP + WARM));
  dt_bit *coded = malloc((size_t)mcap);
  dt_bit *tmp = malloc((size_t)mcap);
  (void)cap;

  int len = 0;
  /* Region 1 - a clean, bit-aligned coded signal. */
  r->a_lo = len;
  int cl = encode_signal(code, payA, framed, info, coded, mcap);
  memcpy(out, coded, (size_t)cl);
  len += cl;
  r->a_hi = len;

  /* Region 2 - dead air, padded so the next signal starts on a block boundary. */
  r->gap_lo = len;
  len = (len + BLOCK - 1) / BLOCK * BLOCK + GAP;
  ex_rand_bits(out + r->gap_lo, len - r->gap_lo, rng);
  r->gap_hi = len;

  /* Region 3 - the same signal through a combined FLIP + DRIFT channel. Flips hit the
   * whole signal; the drift (inserted / dropped bits) starts only after the preamble. */
  r->b_lo = len;
  cl = encode_signal(code, payB, framed, info, coded, mcap);
  int lead = WARM * n; /* the drift-free sync preamble, in coded bits */
  memcpy(out + len, coded, (size_t)lead);
  int t = ex_insert(coded + lead, cl - lead, 0.005, rng, tmp);  /* +0.5% inserts */
  int held = ex_delete(tmp, t, 0.05, rng, out + len + lead);    /* -5% deletes */
  held += lead;
  ex_flip(out + len, held, 0.02, rng);                          /* 2% flips */
  for (int i = 0; i < 64 && lead + 1200 + i < held; ++i) {
    out[len + lead + 1200 + i] = DT_ERASURE; /* a burst for rs251 to mop up */
  }
  len += held;
  r->b_hi = len;

  /* Region 4 - dead air. */
  r->noise_lo = len;
  len = (len + BLOCK - 1) / BLOCK * BLOCK + GAP;
  ex_rand_bits(out + r->noise_lo, len - r->noise_lo, rng);
  r->noise_hi = len;

  /* Region 5 - corrupt junk: noise studded with lone DT_INVALID symbols. */
  r->junk_lo = len;
  ex_rand_bits(out + len, JUNK, rng);
  for (int i = 0; i < JUNK; i += 60) {
    out[len + i] = DT_INVALID;
  }
  len += JUNK;
  r->junk_hi = len;

  free(framed);
  free(info);
  free(coded);
  free(tmp);
  return len;
}
