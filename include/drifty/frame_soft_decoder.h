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

#ifndef DRIFTY_FRAME_SOFT_DECODER_H
#define DRIFTY_FRAME_SOFT_DECODER_H

#include <drifty/frame_decoder.h> /* dt_frame_decoder_state - the shared state machine */
#include <drifty/soft_bit.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_frame_soft_decoder - the frame decoder in fully soft bits. Like
 * dt_frame_decoder (<drifty/frame_decoder.h>) it splits a stream into frames and
 * tracks the boundaries, but every position - received, recovered, and preamble -
 * is a dt_soft_bit (a graded consistency over the output alphabet; see
 * <drifty/soft_bit.h>), never a hard bit.
 *
 * It is the soft-domain middle stage of a concatenated pipeline: a
 * dt_stream_soft_decoder (<drifty/stream_soft_decoder.h>) emits soft bits, this
 * decoder reframes that stream (carrying the soft values through), and a
 * dt_block_soft_decoder (<drifty/block_soft_decoder.h>) consumes the soft bits of
 * each frame's payload - so soft information is preserved end to end.
 *
 * Drive one instance through three phases, and read its frame state from
 * get_state() (OUTSIDE -> BEGIN -> INSIDE -> END; see dt_frame_decoder_state):
 *
 *   begin    - once, first: initialise and write any preamble.
 *   decode   - any number of times: feed received soft bits, out come recovered
 *              soft bits.
 *   finalize - once, last: drain the records still in flight at end of stream.
 *
 * decode() advances until EITHER the next frame-state transition OR `dst` fills,
 * whichever comes first, and returns the number of soft records written (negative
 * on a bad argument). Because a full buffer can stop a call before a transition,
 * inspect get_state() after EVERY decode() to learn the current frame state rather
 * than assuming a boundary was reached. Output that does not fit in `dst` is
 * buffered: a decode() that returns exactly `dst_len` has more buffered, so call
 * again with no new input (src_len 0) to drain before feeding more.
 *
 *   `src` and `dst` are both streams of dt_soft_bit - one record per position.
 *         Inside a frame the received soft bits are decoded (errors corrected);
 *         outside a frame each passes through verbatim.
 *
 * `data` is the implementation's private state - do not touch it. Build a decoder
 * with a factory and free it with the matching _destroy().
 */
typedef struct dt_frame_soft_decoder_t dt_frame_soft_decoder;
struct dt_frame_soft_decoder_t {
  // Initialise the decoder and write any preamble. Call once, before decode().
  int (*begin)(dt_frame_soft_decoder *dec, dt_soft_bit *dst, size_t dst_len);

  // The decoder's current frame state. Check after each decode() call.
  dt_frame_decoder_state (*get_state)(dt_frame_soft_decoder *dec);

  // Decode received soft bits, writing recovered soft records to dst, until the
  // next frame state transition or dst fills. Check get_state() afterwards.
  int (*decode)(dt_frame_soft_decoder *dec, dt_soft_bit *dst, size_t dst_len,
                const dt_soft_bit *src, size_t src_len);

  // Drain records still in flight. Call once, at end of stream.
  int (*finalize)(dt_frame_soft_decoder *dec, dt_soft_bit *dst, size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_FRAME_SOFT_DECODER_H */
