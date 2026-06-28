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

#ifndef DRIFTY_FRAME_ENCODER_H
#define DRIFTY_FRAME_ENCODER_H

#include <drifty/bit.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_frame_encoder - the encode side of a frame codec, called through function
 * pointers. Like dt_stream_encoder (<drifty/stream_encoder.h>) it turns input
 * bits into coded bits as a continuous stream, but it groups protected runs into
 * delimited *frames* and lets other bits pass through uncoded - so a header or
 * sync pattern can ride alongside coded payload in the same output.
 *
 * Drive one instance through these phases:
 *
 *   begin       - once, first: initialise and write any preamble.
 *   begin_frame - open a frame: subsequent encode() output is coded and
 *                 framed.
 *   encode      - any number of times: append bits for `src`. Inside a frame the
 *                 bits are coded; outside a frame they are copied to `dst`
 *                 verbatim (uncoded passthrough).
 *   end_frame   - close the frame opened by begin_frame, writing its trailer.
 *   finalize    - once, last: flush any in-progress bits and write the trailer
 *                 at end of stream.
 *
 * so the lifecycle is:
 *
 *   begin -> [ encode (verbatim) | begin_frame -> encode* -> end_frame ]* -> finalize
 *
 * begin_frame and end_frame pair up and do not nest: begin_frame must be matched
 * by an end_frame before the next begin_frame, and end_frame is valid only with a
 * frame open. encode() is legal in either state - coded while a frame is open,
 * verbatim otherwise.
 *
 * Every call writes into `dst` (capacity `dst_len`) and returns the number of
 * bits written, or a negative value on a bad argument - most often too little
 * room.
 *
 *   `src` is one transmit-domain symbol per information position: DT_TRUE /
 *         DT_FALSE to protect, DT_ERASURE for a don't-care value deferred to the
 *         channel, or DT_INVALID for a deliberate non-value sent as such.
 *   `dst` is the coded transmit-domain stream (see doc/data_flow_semantics.md);
 *         bits encoded outside a frame appear in it unchanged.
 *
 * `data` is the implementation's private state - do not touch it. Build an
 * encoder with a factory and free it with the matching _destroy().
 */
typedef struct dt_frame_encoder_t dt_frame_encoder;
struct dt_frame_encoder_t {
  // Initialise the encoder and write any preamble. Call once, before encode().
  int (*begin)(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len);
  // Open a frame; subsequent encode() output is coded and framed.
  int (*begin_frame)(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len);
  // Encode src_len input bits, appending to dst - coded inside a frame, copied
  // verbatim outside one.
  int (*encode)(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len,
                const dt_bit *src, size_t src_len);
  // Close the open frame, writing its trailer.
  int (*end_frame)(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len);
  // Flush in-progress bits and write the trailer. Call once, at end of stream.
  int (*finalize)(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_FRAME_ENCODER_H */
