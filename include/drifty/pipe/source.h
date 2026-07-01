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

#ifndef DRIFTY_PIPE_SOURCE_H
#define DRIFTY_PIPE_SOURCE_H

#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_pipe_source - the producing end of a bit pipe, called through function
 * pointers. A source hands out a stream on demand (a PULL model): something
 * downstream asks for up to N elements and the source supplies what it has. It
 * has two faces - a HARD one yielding dt_bit symbols and a SOFT one yielding
 * dt_soft_bit records - so one source type serves both hard and soft pipes:
 *
 *   pull      - take up to `dst_len` dt_bit symbols into `dst`.
 *   soft_pull - take up to `dst_len` dt_soft_bit records into `dst`.
 *
 * Each returns the number produced this call, 0 for a clean end of stream (the
 * source is exhausted), or a negative value on error; a short, non-zero count is
 * normal - just call again. An implementation fills the face(s) it supports and
 * leaves the other NULL (a hard-only source has soft_pull == NULL, and vice
 * versa); a pipe calls the face matching its domain, so wire a source to a pipe
 * that reads the face it provides.
 *
 * It is the counterpart of dt_pipe_sink (<drifty/pipe/sink.h>), and a dt_pipe
 * (<drifty/pipe/pipe.h>) drives one into the other. Wrap anything that yields
 * elements behind it - an in-memory buffer (<drifty/pipe/buffers.h>), a file or
 * radio, or a dt_stream_(soft_)decoder whose output you feed onward.
 *
 * The hard face carries transmit-domain symbols (DT_TRUE / DT_FALSE / DT_ERASURE
 * / DT_INVALID / DT_ABSENT), the alphabet the streaming codecs speak; the soft
 * face carries the consistency records the streaming soft decoders emit. `data`
 * is the implementation's private state - do not touch it. Build a source with a
 * factory and free it with the matching _destroy().
 */
typedef struct dt_pipe_source_t dt_pipe_source;
struct dt_pipe_source_t {
  // Pull up to dst_len dt_bit symbols into dst. Returns the number produced (0 at
  // a clean end of stream), or a negative value on error. NULL if unsupported.
  int (*pull)(dt_pipe_source *source, dt_bit *dst, size_t dst_len);
  // Pull up to dst_len dt_soft_bit records into dst, as pull() but soft-valued.
  // NULL if unsupported.
  int (*soft_pull)(dt_pipe_source *source, dt_soft_bit *dst, size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_SOURCE_H */
