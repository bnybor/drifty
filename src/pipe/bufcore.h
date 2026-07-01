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
 * A two-ended buffered core shared by the compound pipes (pipeline, executor). It
 * holds an input buffer and an output buffer (a hard and a soft FIFO each),
 * reached through two pairs of faces over the SAME buffers:
 *
 *   pub_sink   - the pipe's write end: the caller pushes input here.
 *   pub_source - the pipe's read end: the caller pulls output here.
 *   in_src     - the work's source: reads what was pushed to pub_sink.
 *   out_snk    - the work's sink: writes what pub_source will read.
 *
 * The owning pipe embeds this, hands in_src/out_snk to its per-phase work, and
 * exposes pub_source/pub_sink through its dt_pipe getters. Every face carries
 * `data` = the dt_pipe_ends itself, so it reaches the FIFOs. Header-only; not a
 * public header.
 */

#ifndef DRIFTY_SRC_PIPE_BUFCORE_H
#define DRIFTY_SRC_PIPE_BUFCORE_H

#include <drifty/bit.h>
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>

#include <stddef.h>

#include "fifo.h"

typedef struct {
  dt_pipe_source pub_source; // read end: drains the output buffer
  dt_pipe_sink pub_sink;     // write end: fills the input buffer
  dt_pipe_source in_src;     // work's source: drains the input buffer
  dt_pipe_sink out_snk;      // work's sink: fills the output buffer
  dt_bit_fifo in_hard, out_hard;
  dt_soft_fifo in_soft, out_soft;
} dt_pipe_ends;

static inline int dt_pipe_ends_pub_push(dt_pipe_sink *k, const dt_bit *s, size_t n) {
  return dt_bit_fifo_append(&((dt_pipe_ends *)k->data)->in_hard, s, n);
}
static inline int dt_pipe_ends_pub_soft_push(dt_pipe_sink *k, const dt_soft_bit *s, size_t n) {
  return dt_soft_fifo_append(&((dt_pipe_ends *)k->data)->in_soft, s, n);
}
static inline int dt_pipe_ends_pub_pull(dt_pipe_source *s, dt_bit *d, size_t n) {
  return dt_bit_fifo_drain(&((dt_pipe_ends *)s->data)->out_hard, d, n);
}
static inline int dt_pipe_ends_pub_soft_pull(dt_pipe_source *s, dt_soft_bit *d, size_t n) {
  return dt_soft_fifo_drain(&((dt_pipe_ends *)s->data)->out_soft, d, n);
}
static inline int dt_pipe_ends_in_pull(dt_pipe_source *s, dt_bit *d, size_t n) {
  return dt_bit_fifo_drain(&((dt_pipe_ends *)s->data)->in_hard, d, n);
}
static inline int dt_pipe_ends_in_soft_pull(dt_pipe_source *s, dt_soft_bit *d, size_t n) {
  return dt_soft_fifo_drain(&((dt_pipe_ends *)s->data)->in_soft, d, n);
}
static inline int dt_pipe_ends_out_push(dt_pipe_sink *k, const dt_bit *s, size_t n) {
  return dt_bit_fifo_append(&((dt_pipe_ends *)k->data)->out_hard, s, n);
}
static inline int dt_pipe_ends_out_soft_push(dt_pipe_sink *k, const dt_soft_bit *s, size_t n) {
  return dt_soft_fifo_append(&((dt_pipe_ends *)k->data)->out_soft, s, n);
}
static inline int dt_pipe_ends_noop_finish(dt_pipe_sink *k) {
  (void)k;
  return 0;
}

/* Wire the four faces (data pointing back at `e`) and start with empty buffers. */
static inline void dt_pipe_ends_init(dt_pipe_ends *e) {
  e->pub_source.pull = dt_pipe_ends_pub_pull;
  e->pub_source.soft_pull = dt_pipe_ends_pub_soft_pull;
  e->pub_source.data = e;
  e->pub_sink.push = dt_pipe_ends_pub_push;
  e->pub_sink.soft_push = dt_pipe_ends_pub_soft_push;
  e->pub_sink.finish = dt_pipe_ends_noop_finish;
  e->pub_sink.data = e;
  e->in_src.pull = dt_pipe_ends_in_pull;
  e->in_src.soft_pull = dt_pipe_ends_in_soft_pull;
  e->in_src.data = e;
  e->out_snk.push = dt_pipe_ends_out_push;
  e->out_snk.soft_push = dt_pipe_ends_out_soft_push;
  e->out_snk.finish = dt_pipe_ends_noop_finish;
  e->out_snk.data = e;
  e->in_hard.buf = NULL;
  e->in_hard.cap = e->in_hard.head = e->in_hard.tail = 0;
  e->out_hard.buf = NULL;
  e->out_hard.cap = e->out_hard.head = e->out_hard.tail = 0;
  e->in_soft.buf = NULL;
  e->in_soft.cap = e->in_soft.head = e->in_soft.tail = 0;
  e->out_soft.buf = NULL;
  e->out_soft.cap = e->out_soft.head = e->out_soft.tail = 0;
}

static inline void dt_pipe_ends_free(dt_pipe_ends *e) {
  dt_free(e->in_hard.buf);
  dt_free(e->in_soft.buf);
  dt_free(e->out_hard.buf);
  dt_free(e->out_soft.buf);
}

#endif /* DRIFTY_SRC_PIPE_BUFCORE_H */
