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

#ifndef DRIFTY_PIPE_STREAMS_H
#define DRIFTY_PIPE_STREAMS_H

#include <drifty/pipe/pipe.h>
#include <drifty/stream_decoder.h>
#include <drifty/stream_encoder.h>
#include <drifty/stream_soft_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pipe adapters over the streaming codecs. Each wraps a stream encoder or decoder
 * as a dt_pipe (<drifty/pipe/pipe.h>): you push a stream into its sink, and each
 * tick() runs the codec over the buffered input and appends its output to the
 * buffer you pull from. The pipe's begin()/finalize() drive the codec's begin
 * (preamble) and finalize (flush / trailer), so the whole run is:
 *
 *   dt_stream_encoder *enc = dt_cc_encoder_create(code);
 *   dt_pipe *p = dt_pipe_encoder_create(enc);
 *   p->begin(p);
 *   dt_pipe_sink   *in  = p->sink(p);
 *   dt_pipe_source *out = p->source(p);
 *   in->push(in, info, n);     // buffer info bits
 *   p->tick(p);                // encode the buffered bits into the output
 *   dt_bit coded[512];
 *   int got = out->pull(out, coded, 512);
 *   p->finalize(p);            // flush the trailer
 *   dt_pipe_encoder_destroy(p);
 *   dt_cc_encoder_destroy(enc);   // the pipe does not own the codec
 *
 * The wrapped codec is NOT owned - the caller creates it and frees it (with its
 * own _destroy) after the pipe. It must outlive the pipe. These adapters assume a
 * codec that consumes no preamble at begin (true for the drifty cc codecs), so
 * begin() primes the decoder with no input.
 */

/*
 * Encoder pipe (hard -> hard). Sink: push buffers info bits (soft_push is a
 * no-op). tick() encodes them; begin() emits any preamble, finalize() flushes the
 * trailer. Source: pull returns coded bits (soft_pull is a no-op). Returns NULL
 * on a NULL encoder or out of memory.
 */
dt_pipe *dt_pipe_encoder_create(dt_stream_encoder *encoder);
/* Free an encoder pipe from dt_pipe_encoder_create(). NULL is fine. Does not free
 * the wrapped encoder. */
void dt_pipe_encoder_destroy(dt_pipe *pipe);

/*
 * Decoder pipe (hard -> hard). Sink: push buffers received bits (soft_push is a
 * no-op). tick() decodes them; finalize() drains the bits still in flight.
 * Source: pull returns recovered bits (soft_pull is a no-op). Output trails input
 * by the decoder's decision depth (warm-up). Returns NULL on a NULL decoder or
 * out of memory.
 */
dt_pipe *dt_pipe_decoder_create(dt_stream_decoder *decoder);
/* Free a decoder pipe from dt_pipe_decoder_create(). NULL is fine. Does not free
 * the wrapped decoder. */
void dt_pipe_decoder_destroy(dt_pipe *pipe);

/*
 * Soft-decoder pipe (hard -> soft). Sink: push buffers received bits (soft_push
 * is a no-op). tick() soft-decodes them; finalize() drains the records still in
 * flight. Source: soft_pull returns the per-position dt_soft_bit records (pull is
 * a no-op). Output trails input by the decoder's decision depth. Returns NULL on
 * a NULL decoder or out of memory.
 */
dt_pipe *dt_pipe_soft_decoder_create(dt_stream_soft_decoder *decoder);
/* Free a soft-decoder pipe from dt_pipe_soft_decoder_create(). NULL is fine. Does
 * not free the wrapped decoder. */
void dt_pipe_soft_decoder_destroy(dt_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_PIPE_STREAMS_H */
