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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Copy elements from `src` to `dst` until the source yields no more: it pulls
 * both the hard and soft face and pushes what it gets, forwarding whichever face
 * carries data to the matching face of the sink. A face that either end leaves
 * NULL (as a single-domain buffer endpoint does) is skipped - nothing is pumped
 * on a domain that has no path across. A NULL `dst` drains the source and DROPS
 * the bits. Returns the number of elements moved (or dropped), or a negative value
 * on error (a source or sink error, or a sink that will not accept the data). This
 * is the primitive dt_pipeline uses to move bits between stages.
 */
int dt_pipe_pump(dt_pipe_source *src, dt_pipe_sink *dst);

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

/*
 * Executor pipe: a pipe whose phases are user-supplied functions. Each of the
 * pipe's begin / tick / finalize runs the matching function, passing it a `src`
 * that DRAWS from the executor's input buffer (what was pushed to the pipe's
 * sink) and a `dst` that APPENDS to its output buffer (what the pipe's source
 * pulls), plus the `data` pointer given here. A function returns the number of
 * elements it wrote (0 if none) or a negative value on error - the same as the
 * pipe phase it implements. A NULL function pointer is treated as a no-op.
 *
 * This is the general building block the other pipes specialize: e.g. a tick that
 * pulls hard from `src` and pushes soft to `dst` is a softening pipe.
 *
 * Returns NULL out of memory. `data` is not owned.
 */
dt_pipe *dt_pipe_executor_create(
    int (*begin)(dt_pipe_source *src, dt_pipe_sink *dst, void *data),
    int (*tick)(dt_pipe_source *src, dt_pipe_sink *dst, void *data),
    int (*finalize)(dt_pipe_source *src, dt_pipe_sink *dst, void *data),
    void *data);
/* Free an executor pipe from dt_pipe_executor_create(). NULL is fine. */
void dt_pipe_executor_destroy(dt_pipe *pipe);

/*
 * dt_pipeline - a linear compound dt_pipe (<drifty/pipe/pipe.h>): a chain of
 * component pipes (stages) that is itself a dt_pipe. You push into its sink, tick
 * it, and pull from its source like any pipe; internally it moves the stream
 * through the stages and drives their ticks.
 *
 * It has its own input and output buffers. Each of the compound's begin / tick /
 * finalize is the same phase driver - only the stage method it runs differs - so
 * begin and finalize move bits between the stages just as tick does. For a
 * pipeline of stages A, B, C, a tick() runs:
 *
 *   pull from the input buffer, push to A;  A.tick();
 *   pull from A, push to B;                 B.tick();
 *   pull from B, push to C;                 C.tick();
 *   pull from C, push to the output buffer.
 *
 * begin() runs the same movement around each stage's begin (a stage's begin may
 * consume its buffered input - e.g. a preamble - and emit output that flows on to
 * the next stage), and finalize() around each stage's finalize (a trailing flush
 * cascades stage by stage to the output buffer). Whatever domain (hard / soft) a
 * stage emits is forwarded to the matching face of the next stage, so adjacent
 * stages must have compatible domains (insert a hardening or softening pipe where
 * they differ).
 *
 * Because a pipeline is a dt_pipe, a pipeline can be a stage of another pipeline.
 *
 * The `stages` array is COPIED (it need not outlive the call), but the stage
 * pipes themselves are NOT owned: the caller destroys each stage (with its own
 * _destroy) after the pipeline. They must outlive the pipeline.
 *
 * Build from `count` stages (in flow order, index 0 first). `count` may be 0 (the
 * pipeline is then a plain buffer: tick moves input to output). Returns NULL on a
 * bad argument (NULL stages array with count > 0, or any NULL stage) or out of
 * memory.
 */
dt_pipe *dt_pipeline_create(dt_pipe **stages, size_t count);
/* Free a pipeline from dt_pipeline_create(). NULL is fine. Frees the pipeline's
 * copy of the stage array but not the stages themselves. */
void dt_pipeline_destroy(dt_pipe *pipe);

/* Append `stage` to `pipeline` (from dt_pipeline_create) as its new last stage.
 * Like the create-time stages, `stage` is not owned - the caller destroys it. */
void dt_pipeline_add(dt_pipe *pipeline, dt_pipe *stage);
/* Remove `stage` from `pipeline`, preserving the order of the rest; a no-op if
 * `stage` is not a member. Does not destroy the removed stage. */
void dt_pipeline_remove(dt_pipe *pipeline, dt_pipe *stage);

/*
 * dt_pipe_container - a vtable interface that holds a set of pipes and drives
 * their lifecycle together. Its begin / tick / finalize invoke the matching call
 * on every contained pipe, in the order they were added; it does NOT move bits
 * between them (unlike a pipeline) - each pipe is fed and drained independently.
 * Build one with dt_pipe_container_create() and call through the vtable:
 *
 *   dt_pipe_container *c = dt_pipe_container_create();
 *   c->add(c, dt_pipe_hardening_create(), dt_pipe_hardening_destroy);
 *   c->add(c, borrowed_pipe, NULL);   // NULL destroyer: not owned by the container
 *   c->begin(c);
 *   c->tick(c);
 *   c->finalize(c);
 *   dt_pipe_container_destroy(c);      // destroys the owned pipes, last added first
 *
 *   add      - add `pipe`, recording `destroyer` to call on it when the container
 *              is destroyed (NULL leaves the pipe's destruction to the caller).
 *   remove   - remove `pipe` from the container WITHOUT destroying it (the caller
 *              reclaims it); a no-op if it is not held.
 *   begin / tick / finalize - invoke that call on every contained pipe.
 *
 * dt_pipe_container_destroy() destroys the still-contained pipes (via the
 * destroyers given to add) in the REVERSE of the order they were added, then
 * frees the container itself.
 */
typedef struct dt_pipe_container_t dt_pipe_container;
struct dt_pipe_container_t {
  void (*add)(dt_pipe_container *container, dt_pipe *pipe, void (*destroyer)(dt_pipe *));
  void (*remove)(dt_pipe_container *container, dt_pipe *pipe);
  void (*begin)(dt_pipe_container *container);
  void (*tick)(dt_pipe_container *container);
  void (*finalize)(dt_pipe_container *container);
  // implementation-private state; do not access
  void *data;
};

dt_pipe_container *dt_pipe_container_create(void);
/* Destroy a container from dt_pipe_container_create(). NULL is fine. Destroys the
 * still-contained pipes (reverse add order) before freeing the container. */
void dt_pipe_container_destroy(dt_pipe_container *container);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_PIPES_H */
