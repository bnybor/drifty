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

#ifndef DRIFTY_PIPE_BUFFERS_H
#define DRIFTY_PIPE_BUFFERS_H

#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>

#include <limits.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Buffer-backed pipe endpoints - concrete dt_pipe_source / dt_pipe_sink over a
 * caller-provided array. They allocate nothing: declare one, init it over your
 * buffer, and use the returned interface pointer as any source or sink - read from
 * one directly, or hand a pipe's source/sink end to it.
 *
 *   dt_pipe_buffer_source       reads dt_bit symbols from a buffer  (pull face)
 *   dt_pipe_buffer_soft_source  reads dt_soft_bit records from a buffer (soft_pull)
 *   dt_pipe_buffer_sink         writes dt_bit symbols into a buffer  (push face)
 *   dt_pipe_buffer_soft_sink    writes dt_soft_bit records into a buffer (soft_push)
 *
 *   dt_bit out[M];
 *   dt_pipe_buffer_sink dst;
 *   dt_pipe_sink *k = dt_pipe_buffer_sink_init(&dst, out, M);
 *   k->push(k, symbols, n);      // or feed it from a pipe's sink end
 *   // dst.len now holds how many symbols were written
 *
 * A buffer backs one domain, so each fills just its face of the unified
 * source/sink vtable - the hard variants implement pull/push and leave
 * soft_pull/soft_push NULL, the soft variants the reverse. Use the face it
 * provides (pull a hard source, soft_pull a soft one - likewise for a sink).
 *
 * Each _init wires the embedded `base` vtable to point back at the struct and
 * returns &self->base for convenience. The structs are plain data, so read the
 * cursor fields directly: a source's `pos` is how much has been pulled, a sink's
 * `len` how much has been pushed. A source's read face returns 0 once exhausted;
 * a sink consumes up to its remaining capacity, so a short push count (including
 * 0) means the buffer is full. The buffers are not owned - they must outlive the
 * endpoint.
 */

/* clang-format off */

/* -- source: read dt_bit symbols from a buffer (hard/pull face) ------------ */

typedef struct {
  dt_pipe_source base;  // the source interface; pass &<obj>.base to a pipe
  const dt_bit *buf;    // backing storage (not owned)
  size_t len;           // symbols in buf
  size_t pos;           // read cursor (symbols pulled so far)
} dt_pipe_buffer_source;

static inline int dt_pipe_buffer_source_pull(dt_pipe_source *source, dt_bit *dst,
                                             size_t dst_len) {
  dt_pipe_buffer_source *self = (dt_pipe_buffer_source *)source->data;
  size_t avail = self->len - self->pos;
  size_t n = dst_len < avail ? dst_len : avail;
  if (n > (size_t)INT_MAX) n = (size_t)INT_MAX;
  for (size_t i = 0; i < n; ++i) {
    dst[i] = self->buf[self->pos + i];
  }
  self->pos += n;
  return (int)n; // 0 once exhausted
}

// Init `self` to read `len` symbols from `buf`. Returns the source interface.
static inline dt_pipe_source *dt_pipe_buffer_source_init(
    dt_pipe_buffer_source *self, const dt_bit *buf, size_t len) {
  self->base.pull = dt_pipe_buffer_source_pull;
  self->base.soft_pull = NULL; // hard-only: no soft face
  self->base.data = self;
  self->buf = buf;
  self->len = len;
  self->pos = 0;
  return &self->base;
}

/* -- soft source: read dt_soft_bit records from a buffer (soft_pull face) --- */

typedef struct {
  dt_pipe_source base;    // the source interface; pass &<obj>.base to a pipe
  const dt_soft_bit *buf; // backing storage (not owned)
  size_t len;             // records in buf
  size_t pos;             // read cursor (records pulled so far)
} dt_pipe_buffer_soft_source;

static inline int dt_pipe_buffer_soft_source_soft_pull(dt_pipe_source *source,
                                                       dt_soft_bit *dst, size_t dst_len) {
  dt_pipe_buffer_soft_source *self = (dt_pipe_buffer_soft_source *)source->data;
  size_t avail = self->len - self->pos;
  size_t n = dst_len < avail ? dst_len : avail;
  if (n > (size_t)INT_MAX) n = (size_t)INT_MAX;
  for (size_t i = 0; i < n; ++i) {
    dst[i] = self->buf[self->pos + i];
  }
  self->pos += n;
  return (int)n; // 0 once exhausted
}

// Init `self` to read `len` records from `buf`. Returns the source interface.
static inline dt_pipe_source *dt_pipe_buffer_soft_source_init(
    dt_pipe_buffer_soft_source *self, const dt_soft_bit *buf, size_t len) {
  self->base.pull = NULL; // soft-only: no hard face
  self->base.soft_pull = dt_pipe_buffer_soft_source_soft_pull;
  self->base.data = self;
  self->buf = buf;
  self->len = len;
  self->pos = 0;
  return &self->base;
}

/* -- sink: write dt_bit symbols into a buffer (hard/push face) -------------- */

typedef struct {
  dt_pipe_sink base; // the sink interface; pass &<obj>.base to a pipe
  dt_bit *buf;       // backing storage (not owned)
  size_t cap;        // capacity in symbols
  size_t len;        // symbols pushed so far
} dt_pipe_buffer_sink;

static inline int dt_pipe_buffer_sink_push(dt_pipe_sink *sink, const dt_bit *src,
                                           size_t src_len) {
  dt_pipe_buffer_sink *self = (dt_pipe_buffer_sink *)sink->data;
  size_t room = self->cap - self->len;
  size_t n = src_len < room ? src_len : room;
  if (n > (size_t)INT_MAX) n = (size_t)INT_MAX;
  for (size_t i = 0; i < n; ++i) {
    self->buf[self->len + i] = src[i];
  }
  self->len += n;
  return (int)n; // short (or 0) once the buffer is full
}

static inline int dt_pipe_buffer_sink_finish(dt_pipe_sink *sink) {
  (void)sink; // writes straight through; nothing buffered to flush
  return 0;
}

// Init `self` to write up to `cap` symbols into `buf`. Returns the sink interface.
static inline dt_pipe_sink *dt_pipe_buffer_sink_init(dt_pipe_buffer_sink *self,
                                                     dt_bit *buf, size_t cap) {
  self->base.push = dt_pipe_buffer_sink_push;
  self->base.soft_push = NULL; // hard-only: no soft face
  self->base.finish = dt_pipe_buffer_sink_finish;
  self->base.data = self;
  self->buf = buf;
  self->cap = cap;
  self->len = 0;
  return &self->base;
}

/* -- soft sink: write dt_soft_bit records into a buffer (soft_push face) ---- */

typedef struct {
  dt_pipe_sink base; // the sink interface; pass &<obj>.base to a pipe
  dt_soft_bit *buf;  // backing storage (not owned)
  size_t cap;        // capacity in records
  size_t len;        // records pushed so far
} dt_pipe_buffer_soft_sink;

static inline int dt_pipe_buffer_soft_sink_soft_push(dt_pipe_sink *sink,
                                                     const dt_soft_bit *src, size_t src_len) {
  dt_pipe_buffer_soft_sink *self = (dt_pipe_buffer_soft_sink *)sink->data;
  size_t room = self->cap - self->len;
  size_t n = src_len < room ? src_len : room;
  if (n > (size_t)INT_MAX) n = (size_t)INT_MAX;
  for (size_t i = 0; i < n; ++i) {
    self->buf[self->len + i] = src[i];
  }
  self->len += n;
  return (int)n; // short (or 0) once the buffer is full
}

static inline int dt_pipe_buffer_soft_sink_finish(dt_pipe_sink *sink) {
  (void)sink; // writes straight through; nothing buffered to flush
  return 0;
}

// Init `self` to write up to `cap` records into `buf`. Returns the sink interface.
static inline dt_pipe_sink *dt_pipe_buffer_soft_sink_init(
    dt_pipe_buffer_soft_sink *self, dt_soft_bit *buf, size_t cap) {
  self->base.push = NULL; // soft-only: no hard face
  self->base.soft_push = dt_pipe_buffer_soft_sink_soft_push;
  self->base.finish = dt_pipe_buffer_soft_sink_finish;
  self->base.data = self;
  self->buf = buf;
  self->cap = cap;
  self->len = 0;
  return &self->base;
}

/* clang-format on */

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_BUFFERS_H */
