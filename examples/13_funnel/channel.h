/*
 * 13 - channel.h: builds the one continuous, time-varying channel the funnel receives.
 * Example scaffolding, not part of drifty.
 */

#ifndef EX13_CHANNEL_H
#define EX13_CHANNEL_H

#include "stack.h"

#include <drifty/bit.h>
#include <drifty/cc/ccode.h>

#include <stdint.h>

/* Coded bits per routing step - the block the funnel routes on, and the boundary the
 * channel aligns each signal to (so its sync preamble lands wholly inside one block). */
#define BLOCK 2048

/* The bit offsets of each region in the built stream, for labeling the trace. */
struct channel_regions {
  int a_lo, a_hi;         /* clean signal (payload A) */
  int gap_lo, gap_hi;     /* dead air */
  int b_lo, b_hi;         /* noisy + drift signal (payload B) */
  int noise_lo, noise_hi; /* dead air */
  int junk_lo, junk_hi;   /* corrupt junk */
};

/* A buffer size that safely holds the built stream and each region's decoded output. */
int channel_stream_cap(const dt_cc_code *code);

/* Build the channel: a clean coded signal (payload A), dead air, a 2%-flip + 5%-drift
 * coded signal (payload B) with a short erasure burst, more dead air, then noise
 * studded with DT_INVALID. Each signal is padded to start on a BLOCK boundary. Writes
 * the stream to `out` (cap bits), fills `r`, returns the total length. */
int channel_build(const dt_cc_code *code, unsigned char payA[][RS_MSG],
                  unsigned char payB[][RS_MSG], uint64_t *rng, dt_bit *out, int cap,
                  struct channel_regions *r);

#endif /* EX13_CHANNEL_H */
