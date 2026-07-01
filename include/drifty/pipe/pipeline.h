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

#ifndef DRIFTY_PIPE_PIPELINE_H
#define DRIFTY_PIPE_PIPELINE_H

#include <drifty/pipe/pipe.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_pipeline - a linear compound dt_pipe (<drifty/pipe/pipe.h>): a chain of
 * component pipes (stages) that is itself a dt_pipe. You push into its sink, tick
 * it, and pull from its source like any pipe; internally it moves the stream
 * through the stages and drives their ticks.
 *
 * It has its own input and output buffers. Each of the compound's begin / tick /
 * finalize moves bits BETWEEN the stages before and after driving each stage's
 * corresponding call. For a pipeline of stages A, B, C, a tick() runs:
 *
 *   pull from the input buffer, push to A;  A.tick();
 *   pull from A, push to B;                 B.tick();
 *   pull from B, push to C;                 C.tick();
 *   pull from C, push to the output buffer.
 *
 * begin() resets the buffers, begins every stage, then propagates any bits the
 * stages' begins produced (e.g. a preamble) one hop downstream; finalize() drives
 * each stage's finalize in order, moving the bits each one flushes into the next,
 * so a trailing flush cascades all the way to the output buffer. Whatever domain
 * (hard / soft) a stage emits is forwarded to the matching face of the next
 * stage, so adjacent stages must have compatible domains (insert a hardening or
 * softening pipe, <drifty/pipe/pipes.h>, where they differ).
 *
 * Because a pipeline is a dt_pipe, a pipeline can be a stage of another pipeline.
 *
 * The `stages` array is COPIED (it need not outlive the call), but the stage
 * pipes themselves are NOT owned: the caller destroys each stage (with its own
 * _destroy) after the pipeline. They must outlive the pipeline.
 */

/*
 * Build a pipeline from `count` stages (in flow order, index 0 first). `count`
 * may be 0 (the pipeline is then a plain buffer: tick moves input to output).
 * Returns NULL on a bad argument (NULL stages array with count > 0, or any NULL
 * stage) or out of memory.
 */
dt_pipe *dt_pipeline_create(dt_pipe **stages, size_t count);
/* Free a pipeline from dt_pipeline_create(). NULL is fine. Frees the pipeline's
 * copy of the stage array but not the stages themselves. */
void dt_pipeline_destroy(dt_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_PIPELINE_H */
