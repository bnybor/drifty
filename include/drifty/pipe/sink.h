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

#ifndef DRIFTY_PIPE_SINK_H
#define DRIFTY_PIPE_SINK_H

#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_pipe_sink - the consuming end of a bit pipe, called through function
 * pointers. A sink accepts a stream (a PUSH model): something upstream hands it
 * elements and the sink takes them. It has two faces - a HARD one taking dt_bit
 * symbols and a SOFT one taking dt_soft_bit records - so one sink type serves
 * both hard and soft pipes:
 *
 *   push      - accept up to `src_len` dt_bit symbols from `src`.
 *   soft_push - accept up to `src_len` dt_soft_bit records from `src`.
 *   finish    - drain any buffered elements and close the stream. Call once,
 *               after the last push() / soft_push().
 *
 * push / soft_push return the number CONSUMED this call; a short count means the
 * sink is momentarily full, so retry the remainder; negative on error. An
 * implementation fills the face(s) it supports and leaves the other NULL (a
 * hard-only sink has soft_push == NULL, and vice versa); a pipe calls the face
 * matching its domain. finish returns 0 on success, or a negative value on error.
 *
 * It is the counterpart of dt_pipe_source (<drifty/pipe/source.h>), and a dt_pipe
 * (<drifty/pipe/pipe.h>) drives one into the other. Wrap anything that swallows
 * elements behind it - an in-memory buffer (<drifty/pipe/buffers.h>), a file or
 * radio, or a dt_stream_(soft_)decoder you feed onward.
 *
 * The hard face carries transmit-domain symbols (DT_TRUE / DT_FALSE / DT_ERASURE
 * / DT_INVALID / DT_ABSENT); the soft face carries consistency records. `data` is
 * the implementation's private state - do not touch it. Build a sink with a
 * factory and free it with the matching _destroy().
 */
typedef struct dt_pipe_sink_t dt_pipe_sink;
struct dt_pipe_sink_t {
  // Push up to src_len dt_bit symbols from src. Returns the number consumed (a
  // short count means momentarily full - retry the rest), or a negative value on
  // error. NULL if unsupported.
  int (*push)(dt_pipe_sink *sink, const dt_bit *src, size_t src_len);
  // Push up to src_len dt_soft_bit records from src, as push() but soft-valued.
  // NULL if unsupported.
  int (*soft_push)(dt_pipe_sink *sink, const dt_soft_bit *src, size_t src_len);
  // Drain buffered elements and close. Call once, after the last push. Returns 0
  // on success, or a negative value on error.
  int (*finish)(dt_pipe_sink *sink);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_SINK_H */
