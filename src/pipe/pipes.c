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

#include "bufcore.h"
#include "fifo.h"

/* -- pump: copy a source's whole output into a sink ------------------------ */

#define PIPE_PUMP_CHUNK 256

int dt_pipe_pump(dt_pipe_source *src, dt_pipe_sink *dst) {
  int total = 0;
  /* A NULL dst drains the source and drops the bits. Otherwise move the hard face
   * only if both ends have one, and the soft face only if both do: a face absent
   * on either end (a NULL function pointer, as on a single-domain buffer endpoint)
   * means that domain has no matching path across, so nothing is pumped on it. */
  if (src->pull && (!dst || dst->push)) {
    for (;;) {
      dt_bit b[PIPE_PUMP_CHUNK];
      int g = src->pull(src, b, PIPE_PUMP_CHUNK);
      if (g < 0) {
        return g;
      }
      if (g == 0) {
        break;
      }
      int off = 0;
      while (dst && off < g) {
        int w = dst->push(dst, b + off, (size_t)(g - off));
        if (w < 0) {
          return w;
        }
        if (w == 0) {
          return -1; /* sink refuses data - would drop it */
        }
        off += w;
      }
      total += g; /* dst == NULL: pulled and dropped */
    }
  }
  if (src->soft_pull && (!dst || dst->soft_push)) {
    for (;;) {
      dt_soft_bit s[PIPE_PUMP_CHUNK];
      int g = src->soft_pull(src, s, PIPE_PUMP_CHUNK);
      if (g < 0) {
        return g;
      }
      if (g == 0) {
        break;
      }
      int off = 0;
      while (dst && off < g) {
        int w = dst->soft_push(dst, s + off, (size_t)(g - off));
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
  }
  return total;
}

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

/* -- executor pipe: user-supplied begin / tick / finalize ------------------ */

typedef int (*exec_fn)(dt_pipe_source *src, dt_pipe_sink *dst, void *data);

typedef struct {
  dt_pipe base;      // handle (first member; the returned interface)
  dt_pipe_ends ends; // input/output buffers and their faces
  exec_fn begin;
  exec_fn tick;
  exec_fn finalize;
  void *data;
} exec_pipe;

/* Run one phase: the user function draws input from ends.in_src and appends
 * output to ends.out_snk. A NULL function is a no-op. */
static int exec_run(exec_pipe *c, exec_fn fn) {
  return fn ? fn(&c->ends.in_src, &c->ends.out_snk, c->data) : 0;
}

static int exec_begin(dt_pipe *pipe) {
  exec_pipe *c = (exec_pipe *)pipe->data;
  return exec_run(c, c->begin);
}
static int exec_tick(dt_pipe *pipe) {
  exec_pipe *c = (exec_pipe *)pipe->data;
  return exec_run(c, c->tick);
}
static int exec_finalize(dt_pipe *pipe) {
  exec_pipe *c = (exec_pipe *)pipe->data;
  return exec_run(c, c->finalize);
}

static dt_pipe_source *exec_get_source(dt_pipe *pipe) {
  return &((exec_pipe *)pipe->data)->ends.pub_source;
}
static dt_pipe_sink *exec_get_sink(dt_pipe *pipe) {
  return &((exec_pipe *)pipe->data)->ends.pub_sink;
}

dt_pipe *dt_pipe_executor_create(exec_fn begin, exec_fn tick, exec_fn finalize, void *data) {
  exec_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = exec_get_source;
  c->base.sink = exec_get_sink;
  c->base.begin = exec_begin;
  c->base.tick = exec_tick;
  c->base.finalize = exec_finalize;
  c->base.data = c;
  dt_pipe_ends_init(&c->ends);
  c->begin = begin;
  c->tick = tick;
  c->finalize = finalize;
  c->data = data;
  return &c->base;
}

void dt_pipe_executor_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  exec_pipe *c = (exec_pipe *)pipe; // base is the first member
  dt_pipe_ends_free(&c->ends);
  dt_free(c);
}

/* -- container: hold pipes and drive their lifecycle ----------------------- */

typedef struct {
  dt_pipe *pipe;
  void (*destroyer)(dt_pipe *); // NULL: the container does not own this pipe
} container_entry;

typedef struct {
  dt_pipe_container base; // the vtable (first member; the returned interface)
  container_entry *entries;
  size_t n, cap;
} container_impl;

static void container_add(dt_pipe_container *c, dt_pipe *pipe, void (*destroyer)(dt_pipe *)) {
  container_impl *self = (container_impl *)c->data;
  if (self->n == self->cap) {
    size_t ncap = self->cap ? self->cap * 2 : 4;
    container_entry *ne = dt_realloc(self->entries, ncap * sizeof(*ne));
    if (!ne) {
      return; /* out of memory: cannot add (the vtable returns void) */
    }
    self->entries = ne;
    self->cap = ncap;
  }
  self->entries[self->n].pipe = pipe;
  self->entries[self->n].destroyer = destroyer;
  self->n++;
}

static void container_remove(dt_pipe_container *c, dt_pipe *pipe) {
  container_impl *self = (container_impl *)c->data;
  for (size_t i = 0; i < self->n; ++i) {
    if (self->entries[i].pipe == pipe) {
      for (size_t j = i + 1; j < self->n; ++j) { /* shift down, keeping add order */
        self->entries[j - 1] = self->entries[j];
      }
      self->n--;
      return;
    }
  }
}

static void container_begin(dt_pipe_container *c) {
  container_impl *self = (container_impl *)c->data;
  for (size_t i = 0; i < self->n; ++i) {
    dt_pipe *p = self->entries[i].pipe;
    p->begin(p);
  }
}
static void container_tick(dt_pipe_container *c) {
  container_impl *self = (container_impl *)c->data;
  for (size_t i = 0; i < self->n; ++i) {
    dt_pipe *p = self->entries[i].pipe;
    p->tick(p);
  }
}
static void container_finalize(dt_pipe_container *c) {
  container_impl *self = (container_impl *)c->data;
  for (size_t i = 0; i < self->n; ++i) {
    dt_pipe *p = self->entries[i].pipe;
    p->finalize(p);
  }
}

dt_pipe_container *dt_pipe_container_create(void) {
  container_impl *self = dt_malloc(sizeof(*self));
  if (!self) {
    return NULL;
  }
  self->base.add = container_add;
  self->base.remove = container_remove;
  self->base.begin = container_begin;
  self->base.tick = container_tick;
  self->base.finalize = container_finalize;
  self->base.data = self;
  self->entries = NULL;
  self->n = self->cap = 0;
  return &self->base;
}

void dt_pipe_container_destroy(dt_pipe_container *c) {
  if (!c) {
    return;
  }
  container_impl *self = (container_impl *)c->data;
  for (size_t i = self->n; i > 0; --i) { /* reverse of the order added */
    container_entry *e = &self->entries[i - 1];
    if (e->destroyer) {
      e->destroyer(e->pipe);
    }
  }
  dt_free(self->entries);
  dt_free(self);
}
