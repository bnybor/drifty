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

#ifndef DRIFTY_PIPE_FRAMES_H
#define DRIFTY_PIPE_FRAMES_H

#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/pipe/pipe.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pipe adapters over the frame codecs (fc/) - the frame-codec counterpart of the
 * streaming-codec adapters in <drifty/pipe/streams.h>. Each wraps a frame encoder or
 * decoder as a dt_pipe (<drifty/pipe/pipe.h>): you push a stream into its sink, and
 * each tick() runs the codec over the buffered input and appends its output to the
 * buffer you pull from. begin()/finalize() drive the codec's begin (preamble) and
 * finalize (flush / trailer).
 *
 * A frame codec has boundary operations the plain begin/tick/finalize lifecycle
 * cannot carry, so they are exposed as extra, non-vtable calls on the wrapping pipe:
 * the encoder's begin_frame / end_frame (which emit the delimiters around a frame's
 * payload), and, for the decoder, get_state (the OUTSIDE / BEGIN / INSIDE / END
 * position in the frame state machine - see <drifty/frame_decoder.h>) and advance.
 * A decoder tick() copies bits only up to the next frame-state change and then STALLS
 * there - it copies nothing further until you advance() past the boundary. So a
 * decoder is driven boundary by boundary: tick() to copy the next run, read get_state
 * and pull the output, then advance() to move on, repeated. Each tick's output is one
 * run (a frame's payload, or the verbatim bits between frames), and get_state after it
 * names the boundary that ended it. finalize() flushes straight through any stall.
 *
 * The wrapped codec is NOT owned - create it, and free it (with its own _destroy)
 * after the pipe. It must outlive the pipe.
 */

/*
 * Frame encoder pipe (hard -> hard). Sink: push buffers the bits to encode (soft_push
 * is a no-op). tick() encodes the buffered bits - coded while a frame is open, copied
 * verbatim otherwise - appending to the output; begin() emits the preamble, finalize()
 * the trailer. Source: pull returns the framed coded bits (soft_pull is a no-op).
 *
 * Open and close frames with the two calls below, which emit the begin/end delimiters
 * into the output buffer between ticks:
 *
 *   dt_frame_encoder *fe = dt_fc_marker_frame_encoder_create();
 *   dt_pipe *p = dt_pipe_frame_encoder_create(fe);
 *   p->begin(p);
 *   dt_pipe_sink   *in  = p->sink(p);
 *   dt_pipe_frame_encoder_begin_frame(p);   // open a frame
 *   in->push(in, payload, n);
 *   p->tick(p);                             // encode the payload (coded, in-frame)
 *   dt_pipe_frame_encoder_end_frame(p);     // close it
 *   p->finalize(p);                         // flush the trailer
 *   dt_pipe_frame_encoder_destroy(p);
 *   dt_fc_marker_frame_encoder_destroy(fe); // the pipe does not own the codec
 *
 * Returns NULL on a NULL encoder or out of memory.
 */
dt_pipe *dt_pipe_frame_encoder_create(dt_frame_encoder *encoder);
/* Free a frame encoder pipe from dt_pipe_frame_encoder_create(). NULL is fine. Does
 * not free the wrapped encoder. */
void dt_pipe_frame_encoder_destroy(dt_pipe *pipe);

/* Open a frame: emit the begin-frame delimiter into the pipe's output buffer (drained
 * by its source). Subsequent tick() output is coded and framed until the matching
 * end_frame. Returns the number of bits emitted, or negative on error (a NULL pipe,
 * out of memory, or a frame already open). */
int dt_pipe_frame_encoder_begin_frame(dt_pipe *pipe);
/* Close the frame opened by dt_pipe_frame_encoder_begin_frame(): emit the end-frame
 * delimiter into the output buffer. Returns the number of bits emitted, or negative
 * on error (a NULL pipe, out of memory, or no frame open). */
int dt_pipe_frame_encoder_end_frame(dt_pipe *pipe);

/*
 * Frame decoder pipe (hard -> hard). Sink: push buffers received bits (soft_push is a
 * no-op). tick() runs the frame decoder over them, appending the recovered stream to
 * the output; finalize() drains the bits still in flight. Source: pull returns the
 * recovered bits (soft_pull is a no-op). Returns NULL on a NULL decoder or out of
 * memory.
 */
dt_pipe *dt_pipe_frame_decoder_create(dt_frame_decoder *decoder);
/* Free a frame decoder pipe from dt_pipe_frame_decoder_create(). NULL is fine. Does
 * not free the wrapped decoder. */
void dt_pipe_frame_decoder_destroy(dt_pipe *pipe);

/* The frame-boundary state the wrapped decoder is currently in (OUTSIDE / BEGIN /
 * INSIDE / END; see <drifty/frame_decoder.h>). Check it after each tick or advance to
 * follow the frame boundaries. Returns DT_FRAME_DECODER_OUTSIDE for a NULL pipe. */
dt_frame_decoder_state dt_pipe_frame_decoder_get_state(dt_pipe *pipe);

/* Step past the boundary a tick() stalled at, releasing the pipe so the next tick()
 * copies the following run. advance() emits no bits itself (the run's bits came out of
 * the tick that stalled) and always returns 0. Drive a stream frame by frame as
 * tick -> read get_state / pull -> advance, repeated. A no-op on a NULL pipe. */
int dt_pipe_frame_decoder_advance(dt_pipe *pipe);

/*
 * Frame soft-decoder pipe (soft -> soft). Sink: soft_push buffers received soft
 * records (push is a no-op). tick() runs the soft frame decoder over them, appending
 * the recovered soft records to the output; finalize() drains those still in flight.
 * Source: soft_pull returns the recovered records (pull is a no-op). Returns NULL on a
 * NULL decoder or out of memory.
 */
dt_pipe *dt_pipe_frame_soft_decoder_create(dt_frame_soft_decoder *decoder);
/* Free a frame soft-decoder pipe from dt_pipe_frame_soft_decoder_create(). NULL is
 * fine. Does not free the wrapped decoder. */
void dt_pipe_frame_soft_decoder_destroy(dt_pipe *pipe);

/* The frame-boundary state the wrapped soft decoder is currently in. Check it after
 * each tick or advance. Returns DT_FRAME_DECODER_OUTSIDE for a NULL pipe. */
dt_frame_decoder_state dt_pipe_frame_soft_decoder_get_state(dt_pipe *pipe);

/* Step past the boundary a tick() stalled at - the soft-record counterpart of
 * dt_pipe_frame_decoder_advance(). Emits nothing and returns 0; a no-op on a NULL
 * pipe. */
int dt_pipe_frame_soft_decoder_advance(dt_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_FRAMES_H */
