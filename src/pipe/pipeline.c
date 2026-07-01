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
 * dt_pipeline: a linear compound dt_pipe (see pipeline.h). It holds four FIFOs -
 * a hard and a soft queue on each of its input and output ends - reached through
 * two pairs of faces over the SAME buffers: the pipeline's public source/sink
 * (the caller pulls output / pushes input) and an internal source/sink used to
 * feed the first stage and collect from the last. Each begin/tick/finalize moves
 * bits between the stages (pull one stage's output, push it to the next) around
 * driving each stage's matching call.
 *
 * The dt_pipe handle is the first member, so the factory returns &self->base and
 * destroy frees the pipeline through it. Stages are referenced, not owned.
 */

#include <drifty/pipe/pipeline.h>

#include <drifty/bit.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>

#include "fifo.h"

typedef struct {
  dt_pipe base;              // the compound handle (first member)
  dt_pipe_source pub_source; // caller's read end (drains the output FIFOs)
  dt_pipe_sink pub_sink;     // caller's write end (fills the input FIFOs)
  dt_pipe_source in_src;     // internal: drains the input FIFOs into stage 0
  dt_pipe_sink out_snk;      // internal: collects the last stage into the output
  dt_pipe **stages;          // owned array copy; the stages are not owned
  size_t n;
  dt_bit_fifo in_hard, out_hard;
  dt_soft_fifo in_soft, out_soft;
} pipeline;

/* -- faces over the four FIFOs --------------------------------------------- */

static int pl_pub_push(dt_pipe_sink *k, const dt_bit *s, size_t n) {
  return dt_bit_fifo_append(&((pipeline *)k->data)->in_hard, s, n);
}
static int pl_pub_soft_push(dt_pipe_sink *k, const dt_soft_bit *s, size_t n) {
  return dt_soft_fifo_append(&((pipeline *)k->data)->in_soft, s, n);
}
static int pl_pub_finish(dt_pipe_sink *k) {
  (void)k;
  return 0;
}
static int pl_pub_pull(dt_pipe_source *s, dt_bit *d, size_t n) {
  return dt_bit_fifo_drain(&((pipeline *)s->data)->out_hard, d, n);
}
static int pl_pub_soft_pull(dt_pipe_source *s, dt_soft_bit *d, size_t n) {
  return dt_soft_fifo_drain(&((pipeline *)s->data)->out_soft, d, n);
}

static int pl_in_pull(dt_pipe_source *s, dt_bit *d, size_t n) {
  return dt_bit_fifo_drain(&((pipeline *)s->data)->in_hard, d, n);
}
static int pl_in_soft_pull(dt_pipe_source *s, dt_soft_bit *d, size_t n) {
  return dt_soft_fifo_drain(&((pipeline *)s->data)->in_soft, d, n);
}
static int pl_out_push(dt_pipe_sink *k, const dt_bit *s, size_t n) {
  return dt_bit_fifo_append(&((pipeline *)k->data)->out_hard, s, n);
}
static int pl_out_soft_push(dt_pipe_sink *k, const dt_soft_bit *s, size_t n) {
  return dt_soft_fifo_append(&((pipeline *)k->data)->out_soft, s, n);
}
static int pl_out_finish(dt_pipe_sink *k) {
  (void)k;
  return 0;
}

/* -- move a source's whole output into a sink (both faces) ------------------ */

#define PL_CHUNK 256

/* Drain everything available from `S` into `K`. A source produces on one face
 * (the other is a no-op yielding 0), so this forwards whichever face carries data
 * to the matching face of the sink. Returns elements moved, or a negative error. */
static int pl_move(dt_pipe_source *S, dt_pipe_sink *K) {
  int total = 0;
  for (;;) {
    dt_bit b[PL_CHUNK];
    int g = S->pull(S, b, PL_CHUNK);
    if (g < 0) {
      return g;
    }
    if (g == 0) {
      break;
    }
    int off = 0;
    while (off < g) {
      int w = K->push(K, b + off, (size_t)(g - off));
      if (w < 0) {
        return w;
      }
      if (w == 0) {
        return -1; /* sink refuses data - would drop it */
      }
      off += w;
    }
    total += g;
  }
  for (;;) {
    dt_soft_bit sft[PL_CHUNK];
    int g = S->soft_pull(S, sft, PL_CHUNK);
    if (g < 0) {
      return g;
    }
    if (g == 0) {
      break;
    }
    int off = 0;
    while (off < g) {
      int w = K->soft_push(K, sft + off, (size_t)(g - off));
      if (w < 0) {
        return w;
      }
      if (w == 0) {
        return -1;
      }
      off += w;
    }
    total += g;
  }
  return total;
}

/* -- pipe interface -------------------------------------------------------- */

static dt_pipe_source *pl_get_source(dt_pipe *pipe) {
  return &((pipeline *)pipe->data)->pub_source;
}
static dt_pipe_sink *pl_get_sink(dt_pipe *pipe) {
  return &((pipeline *)pipe->data)->pub_sink;
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
    return pl_move(&pl->in_src, &pl->out_snk); /* no stages: a plain buffer */
  }
  int r = pl_move(&pl->in_src, pl->stages[0]->sink(pl->stages[0]));
  if (r < 0) {
    return r;
  }
  r = pl_stage_run(pl->stages[0], phase);
  if (r < 0) {
    return r;
  }
  for (size_t i = 1; i < pl->n; ++i) {
    dt_pipe *prev = pl->stages[i - 1], *cur = pl->stages[i];
    r = pl_move(prev->source(prev), cur->sink(cur));
    if (r < 0) {
      return r;
    }
    r = pl_stage_run(cur, phase);
    if (r < 0) {
      return r;
    }
  }
  dt_pipe *last = pl->stages[pl->n - 1];
  return pl_move(last->source(last), &pl->out_snk);
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

  pl->pub_source.pull = pl_pub_pull;
  pl->pub_source.soft_pull = pl_pub_soft_pull;
  pl->pub_source.data = pl;
  pl->pub_sink.push = pl_pub_push;
  pl->pub_sink.soft_push = pl_pub_soft_push;
  pl->pub_sink.finish = pl_pub_finish;
  pl->pub_sink.data = pl;

  pl->in_src.pull = pl_in_pull;
  pl->in_src.soft_pull = pl_in_soft_pull;
  pl->in_src.data = pl;
  pl->out_snk.push = pl_out_push;
  pl->out_snk.soft_push = pl_out_soft_push;
  pl->out_snk.finish = pl_out_finish;
  pl->out_snk.data = pl;

  pl->in_hard.buf = NULL;
  pl->in_hard.cap = pl->in_hard.head = pl->in_hard.tail = 0;
  pl->out_hard.buf = NULL;
  pl->out_hard.cap = pl->out_hard.head = pl->out_hard.tail = 0;
  pl->in_soft.buf = NULL;
  pl->in_soft.cap = pl->in_soft.head = pl->in_soft.tail = 0;
  pl->out_soft.buf = NULL;
  pl->out_soft.cap = pl->out_soft.head = pl->out_soft.tail = 0;
  return &pl->base;
}

void dt_pipeline_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  pipeline *pl = (pipeline *)pipe; // base is the first member
  dt_free(pl->stages);
  dt_free(pl->in_hard.buf);
  dt_free(pl->in_soft.buf);
  dt_free(pl->out_hard.buf);
  dt_free(pl->out_soft.buf);
  dt_free(pl);
}
