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

#ifndef DRIFTY_PIPE_PIPE_H
#define DRIFTY_PIPE_PIPE_H

#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_pipe - a buffered converter with two ends, called through function pointers.
 * You PUSH a stream into its sink end and PULL it back out of its source end, but
 * the two ends are NOT directly chained. Input is always buffered, output is
 * always buffered, and the pipe's operation runs only in tick(): a tick consumes
 * the buffered input and writes the buffered output, converting between the hard
 * (dt_bit) and soft (dt_soft_bit) faces on the way. Nothing pushed reaches the
 * source end until a tick moves it across.
 *
 *   source   - the read end: the dt_pipe_source (<drifty/pipe/source.h>) you pull
 *              the produced output from. Returns the same pointer each call.
 *   sink     - the write end: the dt_pipe_sink (<drifty/pipe/sink.h>) you push
 *              input into. Returns the same pointer each call.
 *   begin    - prepare to run (reset the buffers). Call once, before the first
 *              tick(). Returns 0, or a negative value on error.
 *   tick     - run the operation: consume the buffered input, produce buffered
 *              output. Returns the number of elements written to the output this
 *              tick, 0 when there is no input to process, or a negative on error.
 *   finalize - flush: process any input still buffered after the last tick(). Call
 *              once, last. Returns the number of elements written, or a negative.
 *   data     - implementation-private state; do not touch it.
 *
 * A pipe usually offers one real face on each end and makes the other a no-op (so
 * the unused face is safe to call and yields nothing), which fixes the pipe's
 * direction. Drive it - begin, then interleave push / tick / pull, then finalize:
 *
 *   dt_pipe *p = dt_pipe_hardening_create();  // see <drifty/pipe/pipes.h>
 *   p->begin(p);
 *   dt_pipe_sink   *in  = p->sink(p);
 *   dt_pipe_source *out = p->source(p);
 *   in->soft_push(in, records, n);   // buffer soft input (or in->push for hard)
 *   p->tick(p);                      // harden the buffered input into the output
 *   dt_bit buf[64];
 *   int got = out->pull(out, buf, 64);
 *   p->finalize(p);
 *   dt_pipe_hardening_destroy(p);
 *
 * A pipe is a single TYPE; the hardening and softening pipes are INSTANCES of it,
 * built by the factories in <drifty/pipe/pipes.h>.
 */
typedef struct dt_pipe_t dt_pipe;
struct dt_pipe_t {
  // The read end: pull the produced output.
  dt_pipe_source *(*source)(dt_pipe *pipe);
  // The write end: push input in.
  dt_pipe_sink *(*sink)(dt_pipe *pipe);
  // Prepare to run (reset). Call once, first. Returns 0 or a negative error.
  int (*begin)(dt_pipe *pipe);
  // Consume buffered input, produce buffered output. Returns elements written (0
  // if no input), or a negative error.
  int (*tick)(dt_pipe *pipe);
  // Flush any input still buffered. Call once, last. Returns elements written, or
  // a negative error.
  int (*finalize)(dt_pipe *pipe);
  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_PIPE_H */
