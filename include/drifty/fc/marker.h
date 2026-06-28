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

#ifndef DRIFTY_FC_MARKER_H
#define DRIFTY_FC_MARKER_H

#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The marker frame codec - variable-length frames delimited by 18-bit escape
 * sequences, presented through the frame interfaces (<drifty/frame_encoder.h>,
 * <drifty/frame_decoder.h>, <drifty/frame_soft_decoder.h>).
 *
 * An escape sequence is fifteen 1s followed by a 3-bit code:
 *
 *   111111111111111 000  pure escape  - escaped data, no frame-state change
 *   111111111111111 001  begin a frame from outside  (OUTSIDE -> BEGIN -> INSIDE)
 *   111111111111111 011  begin a frame from inside    (END -> BEGIN, back to back)
 *   111111111111111 010  end a frame                  (INSIDE -> END -> OUTSIDE)
 *
 * Because every code starts with 0, "fifteen 1s then a 0" always begins a
 * sequence. To keep the stream transparent the encoder escapes any payload run
 * that would form one - inside a frame and outside it alike. An escaped run is
 * emitted as a pure escape (111111111111111 000) plus a 2-bit suffix that names
 * which sequence the payload contained:
 *
 *   suffix 00  the pure-escape sequence (its 000 code rides through as data)
 *   suffix 01  the begin-from-outside sequence
 *   suffix 10  the end sequence
 *   suffix 11  the begin-from-inside sequence
 *
 * so a real marker (codes 001/011/010, no suffix) is never confused with the same
 * bits appearing in the payload. The decoder reverses this, recovering the payload
 * and reporting the frame boundaries through get_state(), walking the state machine
 *
 *   OUTSIDE -> (BEGIN -> INSIDE -> END)*
 *
 * one transition per decode() call (a begin-from-inside surfaces as END then
 * BEGIN). A hard decoder and a fully dt_soft_bit soft decoder recover the same
 * structure. Drive each instance through the phases documented on its interface.
 *
 * Build one with a factory below and free it with the matching _destroy().
 */

/* Build a marker frame encoder. Returns NULL on out of memory. */
dt_frame_encoder *dt_fc_marker_frame_encoder_create(void);
/* Free an encoder from dt_fc_marker_frame_encoder_create(). NULL is fine. */
void dt_fc_marker_frame_encoder_destroy(dt_frame_encoder *enc);

/* Build a marker frame decoder. Returns NULL on out of memory. */
dt_frame_decoder *dt_fc_marker_frame_decoder_create(void);
/* Free a decoder from dt_fc_marker_frame_decoder_create(). NULL is fine. */
void dt_fc_marker_frame_decoder_destroy(dt_frame_decoder *dec);

/* Build a marker frame soft decoder (soft bits in and out). NULL on out of
 * memory. */
dt_frame_soft_decoder *dt_fc_marker_frame_soft_decoder_create(void);
/* Free a soft decoder from dt_fc_marker_frame_soft_decoder_create(). NULL is
 * fine. */
void dt_fc_marker_frame_soft_decoder_destroy(dt_frame_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_FC_MARKER_H */
