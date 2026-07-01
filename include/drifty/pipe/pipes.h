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

#ifndef DRIFTY_PIPE_PIPES_H
#define DRIFTY_PIPE_PIPES_H

#include <drifty/pipe/pipe.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Concrete pipe factories. Each returns a dt_pipe (<drifty/pipe/pipe.h>) - a
 * buffered converter you push into, tick, and pull out of. They take no
 * arguments: a pipe owns its own source and sink ends and its input/output
 * buffers, which grow as needed. Free with the matching _destroy().
 *
 * Both accept hard AND soft input on their sink (push and soft_push both buffer
 * input); the conversion runs in tick(), which drains the input buffer into the
 * output buffer. They differ in the output face their source exposes.
 */

/*
 * Hardening pipe. Sink: push (hard) and soft_push (soft) both buffer input.
 * tick() hardens the buffered input to dt_bit by the argmax projection over the
 * alphabet (see <drifty/soft_bit.h>) and appends it to the output. Source: pull
 * returns the hard output; soft_pull is a no-op that yields nothing (this pipe
 * has no soft output). Returns NULL out of memory.
 */
dt_pipe *dt_pipe_hardening_create(void);
/* Free a hardening pipe from dt_pipe_hardening_create(). NULL is fine. */
void dt_pipe_hardening_destroy(dt_pipe *pipe);

/*
 * Softening pipe: the mirror. Sink: push (hard) and soft_push (soft) both buffer
 * input; hard input is lifted to consistency form as it is buffered. tick() moves
 * the buffered soft input to the output. Source: soft_pull returns the soft
 * output; pull is a no-op that yields nothing. Returns NULL out of memory.
 */
dt_pipe *dt_pipe_softening_create(void);
/* Free a softening pipe from dt_pipe_softening_create(). NULL is fine. */
void dt_pipe_softening_destroy(dt_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_PIPES_H */
