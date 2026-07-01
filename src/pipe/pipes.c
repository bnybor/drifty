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
 * Hardening and softening pipes: buffered converters (see pipes.h and pipe.h).
 * Each embeds a dt_pipe handle plus its own source and sink faces, and holds two
 * growable FIFOs:
 *
 *   input buffer  - dt_soft_bit, fed by the sink. push() lifts a hard symbol to
 *                   its one-hot soft form as it buffers it; soft_push() buffers
 *                   the record directly. So the input buffer is a lossless,
 *                   order-preserving record of everything pushed, in either face.
 *   output buffer - the OUTPUT domain, drained by the source. hard (dt_bit) for a
 *                   hardening pipe, soft (dt_soft_bit) for a softening one.
 *
 * The conversion runs ONLY in tick(): it drains the whole input buffer, converts
 * each element to the output domain, and appends to the output buffer. Nothing
 * pushed reaches the source end until a tick moves it across; the two ends are
 * not chained. finalize() is one last tick (flush any input pushed since).
 *
 * The dt_pipe is the first member, so the factory returns &self->base and destroy
 * frees the pipe (and its buffers) through it.
 */

#include <drifty/pipe/pipes.h>

#include <drifty/bit.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>

#include "fifo.h"

/* -- hard <-> soft projections --------------------------------------------- */

/* Argmax projection of a soft record onto a hard symbol (see soft_bit.h): the
 * hypothesis with the greatest consistency wins. Ties resolve toward the more
 * recoverable symbol - a boolean over a non-bit, DT_TRUE over DT_FALSE. */
static dt_bit harden(const dt_soft_bit *s) {
  dt_bit sym = DT_FALSE;
  float best = s->c_false;
  if (s->c_true >= best) { best = s->c_true; sym = DT_TRUE; }
  if (s->c_erasure > best) { best = s->c_erasure; sym = DT_ERASURE; }
  if (s->c_invalid > best) { best = s->c_invalid; sym = DT_INVALID; }
  if (s->c_absent > best) { best = s->c_absent; sym = DT_ABSENT; }
  return sym;
}

/* Lift a hard symbol to consistency form: the matching hypothesis at 1, rest 0. */
static dt_soft_bit soften(dt_bit x) {
  dt_soft_bit s = {0};
  if (x == DT_TRUE) {
    s.c_true = 1.0f;
  } else if (x == DT_FALSE) {
    s.c_false = 1.0f;
  } else if (x == DT_ERASURE) {
    s.c_erasure = 1.0f;
  } else if (x == DT_INVALID) {
    s.c_invalid = 1.0f;
  } else { /* DT_ABSENT / DT_NONE */
    s.c_absent = 1.0f;
  }
  return s;
}

/* -- hardening pipe: soft input buffer -> hard output buffer ---------------- */

typedef struct {
  dt_pipe base;          // handle (first member; the returned interface)
  dt_pipe_source source; // read end (pull hard)
  dt_pipe_sink sink;     // write end (push hard / soft_push soft)
  dt_soft_fifo in;       // buffered input (soft)
  dt_bit_fifo out;       // buffered output (hard)
} harden_pipe;

static int harden_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  harden_pipe *c = (harden_pipe *)sink->data;
  size_t n = (size_t)dt_fifo_clamp_int(src_len);
  if (dt_soft_fifo_reserve(&c->in, n) < 0) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    c->in.buf[c->in.tail + i] = soften(src[i]); // lift into the soft input buffer
  }
  c->in.tail += n;
  return (int)n;
}

static int harden_soft_push(dt_pipe_sink *sink, const dt_soft_bit *src, size_t src_len) {
  harden_pipe *c = (harden_pipe *)sink->data;
  size_t n = (size_t)dt_fifo_clamp_int(src_len);
  if (dt_soft_fifo_reserve(&c->in, n) < 0) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    c->in.buf[c->in.tail + i] = src[i];
  }
  c->in.tail += n;
  return (int)n;
}

static int harden_finish(dt_pipe_sink *sink) {
  (void)sink; // end of input is handled by the pipe's finalize(), not here
  return 0;
}

static int harden_pull(dt_pipe_source *source, dt_bit *dst, size_t dst_len) {
  harden_pipe *c = (harden_pipe *)source->data;
  return dt_bit_fifo_drain(&c->out, dst, dst_len);
}

static int harden_soft_pull(dt_pipe_source *source, dt_soft_bit *dst, size_t dst_len) {
  (void)source;
  (void)dst;
  (void)dst_len;
  return 0; // no-op: a hardening pipe has no soft output
}

/* The operation: drain the whole soft input buffer, hardening into the output. */
static int harden_run(harden_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  if (n == 0) {
    return 0;
  }
  if (dt_bit_fifo_reserve(&c->out, n) < 0) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    c->out.buf[c->out.tail + i] = harden(&c->in.buf[c->in.head + i]);
  }
  c->out.tail += n;
  c->in.head = c->in.tail = 0; // consumed
  return dt_fifo_clamp_int(n);
}

static int harden_begin(dt_pipe *pipe) {
  (void)pipe; // no begin phase: buffered input is preserved for tick, not discarded
  return 0;
}
static int harden_tick(dt_pipe *pipe) { return harden_run((harden_pipe *)pipe->data); }
static int harden_finalize(dt_pipe *pipe) { return harden_run((harden_pipe *)pipe->data); }

static dt_pipe_source *harden_get_source(dt_pipe *pipe) {
  return &((harden_pipe *)pipe->data)->source;
}
static dt_pipe_sink *harden_get_sink(dt_pipe *pipe) {
  return &((harden_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_hardening_create(void) {
  harden_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = harden_get_source;
  c->base.sink = harden_get_sink;
  c->base.begin = harden_begin;
  c->base.tick = harden_tick;
  c->base.finalize = harden_finalize;
  c->base.data = c;
  c->source.pull = harden_pull;
  c->source.soft_pull = harden_soft_pull; // no-op, not NULL
  c->source.data = c;
  c->sink.push = harden_push;
  c->sink.soft_push = harden_soft_push;
  c->sink.finish = harden_finish;
  c->sink.data = c;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_hardening_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  harden_pipe *c = (harden_pipe *)pipe; // base is the first member
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}

/* -- softening pipe: soft input buffer -> soft output buffer ---------------- */

typedef struct {
  dt_pipe base;          // handle (first member; the returned interface)
  dt_pipe_source source; // read end (soft_pull soft)
  dt_pipe_sink sink;     // write end (push hard / soft_push soft)
  dt_soft_fifo in;       // buffered input (soft)
  dt_soft_fifo out;      // buffered output (soft)
} soften_pipe;

static int soften_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  soften_pipe *c = (soften_pipe *)sink->data;
  size_t n = (size_t)dt_fifo_clamp_int(src_len);
  if (dt_soft_fifo_reserve(&c->in, n) < 0) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    c->in.buf[c->in.tail + i] = soften(src[i]); // lift hard into the soft input
  }
  c->in.tail += n;
  return (int)n;
}

static int soften_soft_push(dt_pipe_sink *sink, const dt_soft_bit *src, size_t src_len) {
  soften_pipe *c = (soften_pipe *)sink->data;
  size_t n = (size_t)dt_fifo_clamp_int(src_len);
  if (dt_soft_fifo_reserve(&c->in, n) < 0) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    c->in.buf[c->in.tail + i] = src[i];
  }
  c->in.tail += n;
  return (int)n;
}

static int soften_finish(dt_pipe_sink *sink) {
  (void)sink;
  return 0;
}

static int soften_soft_pull(dt_pipe_source *source, dt_soft_bit *dst, size_t dst_len) {
  soften_pipe *c = (soften_pipe *)source->data;
  return dt_soft_fifo_drain(&c->out, dst, dst_len);
}

static int soften_pull(dt_pipe_source *source, dt_bit *dst, size_t dst_len) {
  (void)source;
  (void)dst;
  (void)dst_len;
  return 0; // no-op: a softening pipe has no hard output
}

/* The operation: drain the whole soft input buffer into the soft output. */
static int soften_run(soften_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  if (n == 0) {
    return 0;
  }
  if (dt_soft_fifo_reserve(&c->out, n) < 0) {
    return -1;
  }
  for (size_t i = 0; i < n; ++i) {
    c->out.buf[c->out.tail + i] = c->in.buf[c->in.head + i];
  }
  c->out.tail += n;
  c->in.head = c->in.tail = 0; // consumed
  return dt_fifo_clamp_int(n);
}

static int soften_begin(dt_pipe *pipe) {
  (void)pipe; // no begin phase: buffered input is preserved for tick, not discarded
  return 0;
}
static int soften_tick(dt_pipe *pipe) { return soften_run((soften_pipe *)pipe->data); }
static int soften_finalize(dt_pipe *pipe) { return soften_run((soften_pipe *)pipe->data); }

static dt_pipe_source *soften_get_source(dt_pipe *pipe) {
  return &((soften_pipe *)pipe->data)->source;
}
static dt_pipe_sink *soften_get_sink(dt_pipe *pipe) {
  return &((soften_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_softening_create(void) {
  soften_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = soften_get_source;
  c->base.sink = soften_get_sink;
  c->base.begin = soften_begin;
  c->base.tick = soften_tick;
  c->base.finalize = soften_finalize;
  c->base.data = c;
  c->source.pull = soften_pull; // no-op, not NULL
  c->source.soft_pull = soften_soft_pull;
  c->source.data = c;
  c->sink.push = soften_push;
  c->sink.soft_push = soften_soft_push;
  c->sink.finish = soften_finish;
  c->sink.data = c;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_softening_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  soften_pipe *c = (soften_pipe *)pipe; // base is the first member
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}
