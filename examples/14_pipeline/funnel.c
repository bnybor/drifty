/*
 * 14 - funnel.c: the detection-routing decode graph of layout.dot, packaged behind the
 * funnel.h API. Two detectors, two controlling executors, two diverters, two inner
 * decoders, and a combiner feed one frame pipe; the short pipelines and routing pipes
 * sit in a pipe container the caller ticks. A dt_container frees the whole pile at once.
 */

#include "funnel.h"

#include "util.h"

#include <drifty/container.h>
#include <drifty/fc/marker.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/pipe/frames.h>
#include <drifty/pipe/multi.h>
#include <drifty/pipe/pipes.h>
#include <drifty/pipe/streams.h>
#include <drifty/soft_bit.h>
#include <drifty/stream_soft_decoder.h>

#define CONF 0.85
#define PRIME_CLEAN 768 /* bits to warm detect_clean past its transient */
#define PRIME_NOISY 1600 /* detect_noisy's window is far wider */

struct dt_funnel {
  dt_container *bag;      /* owns every codec and pipe below */
  dt_pipe_container *graph;
  dt_pipe *input_split;   /* splitter1 - the caller's entry */
  dt_pipe *frame;         /* the final frame pipe */
  /* control state, shared with the executor ticks: */
  dt_pipe *div1, *div2;   /* the two diverters they steer */
  dt_pipe *bpipe, *mpipe; /* the two decoders, to re-acquire per run */
  dt_soft_bit *scr;
  int scap;
  int prevB, prevM; /* was bcjr / maxir the route last block? */
  int escalated;    /* did executor1 hand this block to detect_noisy? */
  struct dt_funnel_trace tr;
};

/* Register (obj, destroyer) with the cleanup bag and return obj. */
#define KEEP(f, expr, dtor) dt_container_add((f)->bag, (expr), (void (*)(void *))(dtor))

static void mean_conf(const dt_soft_bit *r, int n, double *ce, double *ca) {
  double se = 0, sa = 0;
  for (int i = 0; i < n; ++i) {
    se += r[i].c_erasure;
    sa += r[i].c_absent;
  }
  *ce = n ? se / n : 1.0;
  *ca = n ? sa / n : 1.0;
}

static int drain_conf(dt_funnel *f, dt_pipe_source *src) {
  int got = 0, g;
  while ((g = src->soft_pull(src, f->scr + got, (size_t)(f->scap - got))) > 0) {
    got += g;
  }
  return got;
}

/* executor1: read detect_clean, steer diverter1 (sinks [bcjr, drain, splitter2] ->
 * select 1 / 2 / 3), re-acquiring bcjr on a fresh run. Leaves the route undecided when
 * it escalates - executor2 finishes it. */
static int exec1_tick(dt_pipe_source *src, dt_pipe_sink *dst, void *data) {
  (void)dst;
  dt_funnel *f = data;
  int got = drain_conf(f, src);
  mean_conf(f->scr, got, &f->tr.cce, &f->tr.cca);
  f->tr.ran_noisy = 0;
  f->tr.nce = f->tr.nca = -1.0;
  size_t sel;
  if (f->tr.cca - f->tr.cce > CONF) {
    f->tr.route = '.';
    f->escalated = 0;
    sel = 2; /* -> drain (no code) */
  } else if (f->tr.cce - f->tr.cca > CONF) {
    f->tr.route = 'B';
    f->escalated = 0;
    if (!f->prevB) {
      f->bpipe->begin(f->bpipe);
    }
    sel = 1; /* -> bcjr */
  } else {
    f->tr.route = ' '; /* undecided */
    f->escalated = 1;
    sel = 3; /* -> splitter2 (detect_noisy) */
  }
  f->prevB = (sel == 1);
  dt_pipe_diverter_select(f->div1, sel);
  return 0;
}

/* executor2: only acts when executor1 escalated. Read detect_noisy, steer diverter2
 * (sinks [maxir, drain] -> select 1 / 2), re-acquiring maxir on a fresh run. */
static int exec2_tick(dt_pipe_source *src, dt_pipe_sink *dst, void *data) {
  (void)dst;
  dt_funnel *f = data;
  int got = drain_conf(f, src);
  if (!f->escalated) {
    f->prevM = 0;                        /* any maxir run is broken by this block */
    dt_pipe_diverter_select(f->div2, 2); /* park on drain */
    return 0;
  }
  f->tr.ran_noisy = 1;
  mean_conf(f->scr, got, &f->tr.nce, &f->tr.nca);
  size_t sel;
  if (f->tr.nca - f->tr.nce > CONF) {
    f->tr.route = 'x';
    f->prevM = 0;
    sel = 2; /* -> drain (no signal) */
  } else {
    f->tr.route = 'M';
    if (!f->prevM) {
      f->mpipe->begin(f->mpipe);
    }
    f->prevM = 1;
    sel = 1; /* -> maxir */
  }
  dt_pipe_diverter_select(f->div2, sel);
  return 0;
}

/* Warm a detector past its transient: push noise, tick, discard its output. */
static void warm(dt_pipe *p, const dt_bit *noise, int len, dt_soft_bit *scr, int cap) {
  dt_pipe_sink *s = p->sink(p);
  for (int u = 0; u < len;) {
    int w = s->push(s, noise + u, (size_t)(len - u));
    if (w <= 0) {
      break;
    }
    u += w;
  }
  p->tick(p);
  dt_pipe_source *src = p->source(p);
  while (src->soft_pull(src, scr, (size_t)cap) > 0) {
  }
}

dt_funnel *dt_funnel_create(const dt_cc_code *code,
                            const dt_cc_detect_clean_stream_params *clean,
                            const dt_cc_detect_noisy_stream_params *noisy,
                            const dt_cc_bcjr_stream_params *bp,
                            const dt_cc_maxir_stream_params *mp) {
  dt_funnel *f = calloc(1, sizeof *f);
  if (!f) {
    return NULL;
  }
  f->bag = dt_container_create();
  f->scap = FUNNEL_BLOCK * 4;
  f->scr = malloc((size_t)f->scap * sizeof *f->scr);
  if (!f->bag || !f->scr) {
    dt_container_destroy(f->bag);
    free(f->scr);
    free(f);
    return NULL;
  }
  f->tr.route = ' ';
  f->tr.nce = f->tr.nca = -1.0;

  /* Codecs. */
  dt_stream_soft_decoder *dclean =
      KEEP(f, dt_cc_detect_clean_soft_decoder_create(clean), dt_cc_detect_clean_soft_decoder_destroy);
  dt_stream_soft_decoder *dnoisy =
      KEEP(f, dt_cc_detect_noisy_soft_decoder_create(noisy), dt_cc_detect_noisy_soft_decoder_destroy);
  dt_stream_soft_decoder *bcjr =
      KEEP(f, dt_cc_bcjr_soft_decoder_create(code, bp), dt_cc_bcjr_soft_decoder_destroy);
  dt_stream_soft_decoder *maxir =
      KEEP(f, dt_cc_maxir_soft_decoder_create(code, mp), dt_cc_maxir_soft_decoder_destroy);
  dt_frame_soft_decoder *fdec =
      KEEP(f, dt_fc_marker_frame_soft_decoder_create(), dt_fc_marker_frame_soft_decoder_destroy);

  /* Codec pipes. */
  dt_pipe *pclean = KEEP(f, dt_pipe_soft_decoder_create(dclean), dt_pipe_soft_decoder_destroy);
  dt_pipe *pnoisy = KEEP(f, dt_pipe_soft_decoder_create(dnoisy), dt_pipe_soft_decoder_destroy);
  dt_pipe *bpipe = KEEP(f, dt_pipe_soft_decoder_create(bcjr), dt_pipe_soft_decoder_destroy);
  dt_pipe *mpipe = KEEP(f, dt_pipe_soft_decoder_create(maxir), dt_pipe_soft_decoder_destroy);
  f->frame = KEEP(f, dt_pipe_frame_soft_decoder_create(fdec), dt_pipe_frame_soft_decoder_destroy);
  f->bpipe = bpipe;
  f->mpipe = mpipe;

  /* Merge bcjr + maxir, then feed the frame pipe (which the caller drives, so it is
   * NOT in the container). */
  dt_pipe_source *branches[2] = {bpipe->source(bpipe), mpipe->source(mpipe)};
  dt_pipe *combiner = KEEP(f, dt_pipe_combiner_create(branches, 2), dt_pipe_combiner_destroy);
  dt_pipe *toframe = KEEP(f, dt_pipe_pull_from_create(combiner->source(combiner)), dt_pipe_pull_from_destroy);
  dt_pipe *pushf = KEEP(f, dt_pipe_push_to_create(f->frame->sink(f->frame)), dt_pipe_push_to_destroy);

  dt_pipe *drain1 = KEEP(f, dt_pipe_drain_create(), dt_pipe_drain_destroy);
  dt_pipe *drain2 = KEEP(f, dt_pipe_drain_create(), dt_pipe_drain_destroy);
  dt_pipe *drainN = KEEP(f, dt_pipe_drain_create(), dt_pipe_drain_destroy);
  dt_pipe *drain3 = KEEP(f, dt_pipe_drain_create(), dt_pipe_drain_destroy);

  dt_pipe *exec1 = KEEP(f, dt_pipe_executor_create(NULL, exec1_tick, NULL, f), dt_pipe_executor_destroy);
  dt_pipe *exec2 = KEEP(f, dt_pipe_executor_create(NULL, exec2_tick, NULL, f), dt_pipe_executor_destroy);

  /* Noisy branch: diverter2 [maxir, drain3]; splitter2 tees to detect_noisy + div2. */
  dt_pipe_sink *d2[2] = {mpipe->sink(mpipe), drain3->sink(drain3)};
  dt_pipe *div2 = KEEP(f, dt_pipe_diverter_create(d2, 2), dt_pipe_diverter_destroy);
  f->div2 = div2;
  dt_pipe *plN_stages[3] = {pnoisy, exec2, drainN};
  dt_pipe *plN = KEEP(f, dt_pipeline_create(plN_stages, 3), dt_pipeline_destroy);
  dt_pipe_sink *s2[2] = {plN->sink(plN), div2->sink(div2)};
  dt_pipe *split2 = KEEP(f, dt_pipe_splitter_create(s2, 2), dt_pipe_splitter_destroy);

  /* Clean branch: diverter1 [bcjr, drain2, splitter2]; splitter1 tees to detect_clean
   * + div1. */
  dt_pipe_sink *d1[3] = {bpipe->sink(bpipe), drain2->sink(drain2), split2->sink(split2)};
  dt_pipe *div1 = KEEP(f, dt_pipe_diverter_create(d1, 3), dt_pipe_diverter_destroy);
  f->div1 = div1;
  dt_pipe *plC_stages[3] = {pclean, exec1, drain1};
  dt_pipe *plC = KEEP(f, dt_pipeline_create(plC_stages, 3), dt_pipeline_destroy);
  dt_pipe_sink *s1[2] = {plC->sink(plC), div1->sink(div1)};
  dt_pipe *split1 = KEEP(f, dt_pipe_splitter_create(s1, 2), dt_pipe_splitter_destroy);
  f->input_split = split1;

  dt_pipe *plO_stages[2] = {toframe, pushf};
  dt_pipe *plO = KEEP(f, dt_pipeline_create(plO_stages, 2), dt_pipeline_destroy);

  /* The pipe container, in tick order - one tick drives a block from channel to frame. */
  f->graph = dt_pipe_container_create();
  KEEP(f, f->graph, dt_pipe_container_destroy);
  f->graph->add(f->graph, split1, NULL);
  f->graph->add(f->graph, plC, NULL);
  f->graph->add(f->graph, div1, NULL);
  f->graph->add(f->graph, drain2, NULL);
  f->graph->add(f->graph, split2, NULL);
  f->graph->add(f->graph, plN, NULL);
  f->graph->add(f->graph, div2, NULL);
  f->graph->add(f->graph, drain3, NULL);
  f->graph->add(f->graph, bpipe, NULL);
  f->graph->add(f->graph, mpipe, NULL);
  f->graph->add(f->graph, combiner, NULL);
  f->graph->add(f->graph, plO, NULL);

  f->graph->begin(f->graph);
  f->frame->begin(f->frame);
  {
    uint64_t prng = 0xC0FFEE14u;
    dt_bit *noise = malloc(PRIME_NOISY);
    if (noise) {
      ex_rand_bits(noise, PRIME_CLEAN, &prng);
      warm(pclean, noise, PRIME_CLEAN, f->scr, f->scap);
      ex_rand_bits(noise, PRIME_NOISY, &prng);
      warm(pnoisy, noise, PRIME_NOISY, f->scr, f->scap);
      free(noise);
    }
  }
  return f;
}

dt_pipe_sink *dt_funnel_input(dt_funnel *f) {
  return f->input_split->sink(f->input_split);
}
void dt_funnel_tick(dt_funnel *f) { f->graph->tick(f->graph); }
void dt_funnel_finalize(dt_funnel *f) { f->graph->finalize(f->graph); }
dt_pipe *dt_funnel_frames(dt_funnel *f) { return f->frame; }
struct dt_funnel_trace dt_funnel_last(const dt_funnel *f) { return f->tr; }

void dt_funnel_destroy(dt_funnel *f) {
  if (!f) {
    return;
  }
  dt_container_destroy(f->bag); /* frees the graph, every pipe, and every codec */
  free(f->scr);
  free(f);
}
