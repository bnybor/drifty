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

#ifndef DRIFTY_PIPE_MULTI_H
#define DRIFTY_PIPE_MULTI_H

#include <drifty/pipe/pipe.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Multi-way pipe constructs (fan-out / fan-in over dt_pipe). */

/*
 * Splitter (a tee): a dt_pipe that copies its input to its own output AND to each
 * of `count` extra sinks. You push into its sink and tick it like any pipe; each
 * of its tick / finalize copies the buffered input to the output buffer
 * (which the pipe's source drains) and pushes a copy to every sink in `sinks`.
 * Whichever face carries data (hard dt_bit / soft dt_soft_bit) is copied to the
 * matching face of each destination; a sink that does not accept that face (a NULL
 * push / soft_push, as on a single-domain buffer endpoint) is skipped for it.
 *
 * The `sinks` array is COPIED (it need not outlive the call); the sinks themselves
 * are NOT owned and are never finished - the caller owns their lifecycle, and they
 * must outlive the splitter. `count` may be 0 (the splitter is then a plain
 * passthrough to its own output). Returns NULL on a bad argument (NULL array with
 * count > 0, or any NULL sink) or out of memory.
 */
dt_pipe *dt_pipe_splitter_create(dt_pipe_sink **sinks, size_t count);
/* Free a splitter from dt_pipe_splitter_create(). NULL is fine. Frees the
 * splitter's copy of the sink array but not the sinks themselves. */
void dt_pipe_splitter_destroy(dt_pipe *pipe);

/*
 * Diverter (a demultiplexer): a dt_pipe that copies its input to exactly ONE
 * output, chosen by dt_pipe_diverter_select(). With selection 0 (the default) the
 * input goes to the pipe's own output buffer, which its source drains; with
 * selection k in [1, len] it is diverted to sinks[k-1] instead and the pipe's own
 * source yields nothing. A selection past len discards the input.
 *
 * As with the splitter, `sinks` is COPIED (need not outlive the call) and the
 * sinks are NOT owned or finished; each of tick / finalize routes the
 * buffered input to the current selection, matching hard/soft faces (a
 * destination that lacks the carried face is skipped). Returns NULL on a bad
 * argument (NULL array with len > 0, or any NULL sink) or out of memory.
 */
dt_pipe *dt_pipe_diverter_create(dt_pipe_sink **sinks, size_t len);
/* Choose where the input goes: 0 = the pipe's own output; k = sinks[k-1]. May be
 * called between ticks to reroute the stream. */
void dt_pipe_diverter_select(dt_pipe *pipe, size_t idx);
/* Free a diverter from dt_pipe_diverter_create(). NULL is fine. Frees the
 * diverter's copy of the sink array but not the sinks themselves. */
void dt_pipe_diverter_destroy(dt_pipe *pipe);

/*
 * Selector (a multiplexer): a dt_pipe that pumps exactly ONE of its inputs to the
 * output buffer, chosen by dt_pipe_selector_select(). With selection 0 (the
 * default) the input is the pipe's own input buffer (what was pushed to its sink);
 * with selection k in [1, len] it is pumped from sources[k-1] instead. Every
 * UNSELECTED input (the pipe's own buffer and all the other sources) is pumped to
 * NULL - drained and discarded - on each tick / finalize, so their data is
 * not held for later. A selection past len forwards nothing (all inputs drained).
 *
 * As with the diverter, `sources` is COPIED (need not outlive the call) and the
 * sources are NOT owned; they must outlive the selector. Returns NULL on a bad
 * argument (NULL array with len > 0, or any NULL source) or out of memory.
 */
dt_pipe *dt_pipe_selector_create(dt_pipe_source **sources, size_t len);
/* Choose which input feeds the output: 0 = the pipe's own input buffer; k =
 * sources[k-1]. May be called between ticks to switch inputs. */
void dt_pipe_selector_select(dt_pipe *pipe, size_t idx);
/* Free a selector from dt_pipe_selector_create(). NULL is fine. Frees the
 * selector's copy of the source array but not the sources themselves. */
void dt_pipe_selector_destroy(dt_pipe *pipe);

/*
 * Valve (a gate): a dt_pipe that, while OPEN, passes its input through to its
 * output buffer, and while CLOSED drains its input to NULL (dropping it). Each
 * tick / finalize pumps the buffered input to the output (open) or to
 * nowhere (closed). Created OPEN; toggle between ticks with dt_pipe_valve_open /
 * dt_pipe_valve_close. Returns NULL out of memory.
 */
dt_pipe *dt_pipe_valve_create(void);
/* Open the valve: pass input through to the output. */
void dt_pipe_valve_open(dt_pipe *pipe);
/* Close the valve: drop input instead of passing it through. */
void dt_pipe_valve_close(dt_pipe *pipe);
/* Free a valve from dt_pipe_valve_create(). NULL is fine. */
void dt_pipe_valve_destroy(dt_pipe *pipe);

/*
 * Combiner (a merger): a dt_pipe that pumps its own input buffer and then each of
 * the additional sources, in order, into its output buffer - concatenating them.
 * Each tick / finalize drains the pipe's own input (what was pushed to its
 * sink) followed by sources[0], sources[1], ... into the output, matching hard /
 * soft faces (a source that lacks a face the output takes contributes nothing on
 * it).
 *
 * `sources` is COPIED (need not outlive the call) and the sources are NOT owned;
 * they must outlive the combiner. `len` may be 0 (the combiner is then a plain
 * passthrough of its own input). Returns NULL on a bad argument (NULL array with
 * len > 0, or any NULL source) or out of memory.
 */
dt_pipe *dt_pipe_combiner_create(dt_pipe_source **sources, size_t len);
/* Free a combiner from dt_pipe_combiner_create(). NULL is fine. Frees the
 * combiner's copy of the source array but not the sources themselves. */
void dt_pipe_combiner_destroy(dt_pipe *pipe);

/*
 * Drain (a cap): a dt_pipe that discards its input. Each tick / finalize drains
 * whatever was pushed to its sink and drops it; its source never yields anything.
 * Returns NULL out of memory.
 */
dt_pipe *dt_pipe_drain_create(void);
/* Free a drain from dt_pipe_drain_create(). NULL is fine. */
void dt_pipe_drain_destroy(dt_pipe *pipe);

/*
 * Push-to: a dt_pipe that pumps its input exclusively to the sink `dst` given at
 * create - like a diverter permanently routing to one external sink. Each tick /
 * finalize pumps whatever was pushed to its own sink into `dst`; its own source
 * yields nothing. `dst` is NOT owned and must outlive the pipe. Returns NULL on a
 * NULL `dst` or out of memory.
 */
dt_pipe *dt_pipe_push_to_create(dt_pipe_sink *dst);
/* Free a push-to from dt_pipe_push_to_create(). NULL is fine. Does not free `dst`. */
void dt_pipe_push_to_destroy(dt_pipe *pipe);

/*
 * Pull-from: a dt_pipe that fills its output exclusively from the source `src`
 * given at create - like a selector permanently reading one external source. Each
 * tick / finalize pumps `src` into the output buffer (which its own source drains)
 * and drops anything pushed to its own sink. `src` is NOT owned and must outlive
 * the pipe. Returns NULL on a NULL `src` or out of memory.
 */
dt_pipe *dt_pipe_pull_from_create(dt_pipe_source *src);
/* Free a pull-from from dt_pipe_pull_from_create(). NULL is fine. Does not free `src`. */
void dt_pipe_pull_from_destroy(dt_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_MULTI_H */
