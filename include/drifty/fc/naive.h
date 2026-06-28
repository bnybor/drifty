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

#ifndef DRIFTY_FC_NAIVE_H
#define DRIFTY_FC_NAIVE_H

#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The naive frame codec - the simplest frame codec, presented through the frame
 * interfaces (<drifty/frame_encoder.h>, <drifty/frame_decoder.h>,
 * <drifty/frame_soft_decoder.h>). It uses fixed-length frames of `len` symbols
 * with no actual preamble or trailer: the encoder is a pass-through that copies
 * bits unchanged, and both sides agree on `len` rather than relying on any
 * delimiter in the stream.
 *
 * The decoder (hard, and a fully dt_soft_bit soft variant) re-splits the stream
 * into `len`-symbol frames and reports the boundaries through get_state(),
 * walking the frame state machine
 *
 *   OUTSIDE -> (BEGIN -> INSIDE -> END)*
 *
 * Because the preamble and trailer are zero-length, BEGIN and END are pure 0-bit
 * transitions: each decode() makes exactly one state move (consuming the next
 * frame's `len` payload symbols while INSIDE), so the caller must drive decode()
 * + get_state() to observe every transition. Once the first frame opens the
 * decoder never returns to OUTSIDE - frames run back to back. As a delay-free
 * pass-through it buffers nothing: every call consumes exactly the symbols it
 * writes, so the caller advances `src` by the return value.
 *
 * Each factory takes the frame length `len` (in symbols). Build one with a
 * factory below and free it with the matching _destroy().
 */

/* Build a naive frame encoder for frames of `len` symbols. Returns NULL on out
 * of memory. */
dt_frame_encoder *dt_fc_naive_frame_encoder_create(size_t len);
/* Free an encoder from dt_fc_naive_frame_encoder_create(). NULL is fine. */
void dt_fc_naive_frame_encoder_destroy(dt_frame_encoder *enc);

/* Build a naive frame decoder for frames of `len` symbols. Returns NULL on out
 * of memory. */
dt_frame_decoder *dt_fc_naive_frame_decoder_create(size_t len);
/* Free a decoder from dt_fc_naive_frame_decoder_create(). NULL is fine. */
void dt_fc_naive_frame_decoder_destroy(dt_frame_decoder *dec);

/* Build a naive frame soft decoder (soft bits in and out) for frames of `len`
 * symbols. Returns NULL on out of memory. */
dt_frame_soft_decoder *dt_fc_naive_frame_soft_decoder_create(size_t len);
/* Free a soft decoder from dt_fc_naive_frame_soft_decoder_create(). NULL is fine. */
void dt_fc_naive_frame_soft_decoder_destroy(dt_frame_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_FC_NAIVE_H */
