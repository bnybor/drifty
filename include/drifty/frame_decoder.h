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

#ifndef DRIFTY_FRAME_DECODER_H
#define DRIFTY_FRAME_DECODER_H

#include <drifty/bit.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where the decoder sits in the frame state machine, as reported by get_state()
 * after each decode() call:
 *
 *   OUTSIDE -> (BEGIN -> INSIDE -> END)* -> OUTSIDE
 *
 * OUTSIDE and INSIDE are sustained states; BEGIN and END are momentary, reported
 * for the call that opens or closes a frame.
 */
typedef enum dt_frame_decoder_state_t {
  DT_FRAME_DECODER_OUTSIDE = 0, // between frames: decoded bits are verbatim
  DT_FRAME_DECODER_BEGIN = 1,   // a frame just opened
  DT_FRAME_DECODER_INSIDE = 2,  // within a frame: decoded bits are corrected
  DT_FRAME_DECODER_END = 3      // a frame just closed
} dt_frame_decoder_state;

/*
 * dt_frame_decoder - the decode side of a frame codec, called through function
 * pointers. It is the counterpart of dt_frame_encoder (<drifty/frame_encoder.h>):
 * it turns a received stream back into recovered bits, discovering the frame
 * boundaries the encoder wrote. Inside a frame the received bits are decoded
 * (errors corrected); outside a frame they pass through verbatim - the inverse of
 * the encoder's coded/verbatim split.
 *
 * Drive one instance through three phases:
 *
 *   begin    - once, first: initialise and consume any preamble at the head of
 *              the received stream.
 *   decode   - any number of times: feed received bits, out come recovered bits.
 *   finalize - once, last: drain the bits still in flight at end of stream.
 *
 * decode() advances until EITHER the next frame-state transition OR `dst` fills,
 * whichever comes first, and returns the number of bits written (negative on a
 * bad argument). Because a full buffer can stop a call before a transition, the
 * caller must inspect get_state() after EVERY decode() to learn the current
 * frame state rather than assuming a boundary was reached. As with the streaming
 * decoder, output that does not fit in `dst` is buffered: a decode() that returns
 * exactly `dst_len` has more buffered, so call again with no new input
 * (src_len 0) to drain before feeding more.
 *
 *   `src` is the received stream (DT_TRUE / DT_FALSE / DT_ERASURE / DT_INVALID,
 *         and DT_ABSENT for a position an upstream decoder judged deleted).
 *   `dst` is one output-domain symbol per recovered position (see
 *         doc/data_flow_semantics.md); bits decoded outside a frame appear in it
 *         verbatim.
 *
 * `data` is the implementation's private state - do not touch it. Build a decoder
 * with a factory and free it with the matching _destroy().
 */
typedef struct dt_frame_decoder_t dt_frame_decoder;
struct dt_frame_decoder_t {
  // Initialise the decoder and consume any preamble at the head of `src`. Call
  // once, before decode(). Returns preamble bits consumed (0 if none).
  int (*begin)(dt_frame_decoder *dec, const dt_bit *src, size_t src_len);

  // The decoder's current frame state. Check after each decode() call.
  dt_frame_decoder_state (*get_state)(dt_frame_decoder *dec);

  // Decode received bits, writing recovered bits to dst, until the next frame
  // state transition or dst fills. Check get_state() afterwards.
  int (*decode)(dt_frame_decoder *dec, dt_bit *dst, size_t dst_len,
                const dt_bit *src, size_t src_len);

  // Drain bits still in flight. Call once, at end of stream.
  int (*finalize)(dt_frame_decoder *dec, dt_bit *dst, size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_FRAME_DECODER_H */
