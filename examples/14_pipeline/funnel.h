/*
 * 14 - funnel.h: the detection-routed decode graph, packaged as one object. The whole
 * funnel of example 13 lives inside a pipe graph (see layout.dot); the caller only
 * pushes channel bits, ticks, and reads recovered frames from the final frame pipe -
 * the graph detects, switches between bcjr and maxir, and reframes internally.
 */

#ifndef EX14_FUNNEL_H
#define EX14_FUNNEL_H

#include <drifty/cc/bcjr.h>
#include <drifty/cc/ccode.h>
#include <drifty/cc/detect_clean.h>
#include <drifty/cc/detect_noisy.h>
#include <drifty/cc/maxir.h>
#include <drifty/pipe/pipe.h>

/* Coded bits per routing step (wider than detect_noisy's ~1200-bit window). */
#define FUNNEL_BLOCK 2048

typedef struct dt_funnel dt_funnel;

/* Build the graph over `code` with the four channel models. Returns NULL on failure.
 * `code` and the params are copied where the codecs need them; `code` must outlive
 * the funnel. */
dt_funnel *dt_funnel_create(const dt_cc_code *code,
                            const dt_cc_detect_clean_stream_params *clean,
                            const dt_cc_detect_noisy_stream_params *noisy,
                            const dt_cc_bcjr_stream_params *bp,
                            const dt_cc_maxir_stream_params *mp);

/* Where the caller pushes a block of channel bits. */
dt_pipe_sink *dt_funnel_input(dt_funnel *f);
/* Drive one block all the way through: detect -> route -> decode -> combine -> frame. */
void dt_funnel_tick(dt_funnel *f);
/* Flush the inner decoders' in-flight tails into the frame pipe at end of stream. */
void dt_funnel_finalize(dt_funnel *f);
/* The final frame SOFT-decoder pipe - the only pipe the caller sees. */
dt_pipe *dt_funnel_frames(dt_funnel *f);

/* The routing decision the graph made on the most recent tick, for tracing. */
struct dt_funnel_trace {
  double cce, cca; /* detect_clean (c_erasure, c_absent) */
  double nce, nca; /* detect_noisy, or negative when it was skipped */
  int ran_noisy;   /* did this block reach detect_noisy? */
  char route;      /* 'B' bcjr, 'M' maxir, '.' drop(clean), 'x' drop(noisy) */
};
struct dt_funnel_trace dt_funnel_last(const dt_funnel *f);

void dt_funnel_destroy(dt_funnel *f);

#endif /* EX14_FUNNEL_H */
