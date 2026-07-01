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
 * dt_pipeline: a linear compound dt_pipe (see pipeline.h). It embeds a two-ended
 * buffered core (bufcore.h) - the caller's input and output buffers plus the
 * internal in_src / out_snk the compound reads and writes. Each of begin / tick /
 * finalize is the same phase driver: move the input buffer into the first stage,
 * run the stage, move each stage's output into the next and run it, then collect
 * the last stage into the output buffer - differing only in which stage method it
 * runs. So begin and finalize are specialized ticks that also move bits between
 * the stages' begin / finalize calls.
 *
 * The dt_pipe handle is the first member, so the factory returns &self->base and
 * destroy frees the pipeline through it. Stages are referenced, not owned.
 */

#include <drifty/pipe/pipes.h> /* dt_pipeline_* declarations + dt_pipe_pump */

#include <drifty/pipe/pipe.h>
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/stdlib.h>

#include "bufcore.h"

typedef struct {
  dt_pipe base;      // the compound handle (first member)
  dt_pipe_ends ends; // input/output buffers and their faces
  dt_pipe **stages;  // owned array copy; the stages are not owned
  size_t n;
} pipeline;

/* -- pipe interface -------------------------------------------------------- */

static dt_pipe_source *pl_get_source(dt_pipe *pipe) {
  return &((pipeline *)pipe->data)->ends.pub_source;
}
static dt_pipe_sink *pl_get_sink(dt_pipe *pipe) {
  return &((pipeline *)pipe->data)->ends.pub_sink;
}

/* The one phase driver. begin, tick, and finalize differ ONLY in which method
 * each stage runs; the bit movement is identical - pull the input buffer into the
 * first stage, run the stage, move each stage's output into the next stage and
 * run it, then collect the last stage into the output buffer. So begin and
 * finalize are just specialized ticks: begin moves bits between the stages' begin
 * calls (a stage's begin may consume its buffered input - e.g. a preamble - and
 * emit output), finalize between their finalize calls (flushing a trailer / tail
 * cascades stage by stage to the output). */
typedef enum { PL_BEGIN, PL_TICK, PL_FINALIZE } pl_phase;

static int pl_stage_run(dt_pipe *s, pl_phase phase) {
  return phase == PL_BEGIN ? s->begin(s) : phase == PL_TICK ? s->tick(s) : s->finalize(s);
}

static int pl_run(pipeline *pl, pl_phase phase) {
  if (pl->n == 0) {
    return dt_pipe_pump(&pl->ends.in_src, &pl->ends.out_snk); /* no stages: a buffer */
  }
  int r = dt_pipe_pump(&pl->ends.in_src, pl->stages[0]->sink(pl->stages[0]));
  if (r < 0) {
    return r;
  }
  r = pl_stage_run(pl->stages[0], phase);
  if (r < 0) {
    return r;
  }
  for (size_t i = 1; i < pl->n; ++i) {
    dt_pipe *prev = pl->stages[i - 1], *cur = pl->stages[i];
    r = dt_pipe_pump(prev->source(prev), cur->sink(cur));
    if (r < 0) {
      return r;
    }
    r = pl_stage_run(cur, phase);
    if (r < 0) {
      return r;
    }
  }
  dt_pipe *last = pl->stages[pl->n - 1];
  return dt_pipe_pump(last->source(last), &pl->ends.out_snk);
}

static int pl_begin(dt_pipe *pipe) { return pl_run((pipeline *)pipe->data, PL_BEGIN); }
static int pl_tick(dt_pipe *pipe) { return pl_run((pipeline *)pipe->data, PL_TICK); }
static int pl_finalize(dt_pipe *pipe) { return pl_run((pipeline *)pipe->data, PL_FINALIZE); }

/* -- factory --------------------------------------------------------------- */

dt_pipe *dt_pipeline_create(dt_pipe **stages, size_t count) {
  if (count > 0 && !stages) {
    return NULL;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!stages[i]) {
      return NULL;
    }
  }
  pipeline *pl = dt_malloc(sizeof(*pl));
  if (!pl) {
    return NULL;
  }
  if (count > 0) {
    pl->stages = dt_malloc(count * sizeof(*pl->stages));
    if (!pl->stages) {
      dt_free(pl);
      return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
      pl->stages[i] = stages[i];
    }
  } else {
    pl->stages = NULL;
  }
  pl->n = count;
  pl->base.source = pl_get_source;
  pl->base.sink = pl_get_sink;
  pl->base.begin = pl_begin;
  pl->base.tick = pl_tick;
  pl->base.finalize = pl_finalize;
  pl->base.data = pl;
  dt_pipe_ends_init(&pl->ends);
  return &pl->base;
}

void dt_pipeline_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  pipeline *pl = (pipeline *)pipe; // base is the first member
  dt_pipe_ends_free(&pl->ends);
  dt_free(pl->stages);
  dt_free(pl);
}
