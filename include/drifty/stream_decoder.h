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

#ifndef DRIFTY_STREAM_DECODER_H
#define DRIFTY_STREAM_DECODER_H

#include <drifty/bit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * dt_stream_decoder - the decode side of the streaming codec, called through function
 * pointers. It recovers input bits from received coded bits, correcting bits
 * that were flipped, inserted, dropped, or erased.
 *
 * Drive one instance through three phases:
 *
 *   begin    - once, first: initialise and write any preamble.
 *   decode   - any number of times: feed received bits as they arrive; out
 *              come recovered bits.
 *   finalize - once, last: drain the bits still in flight at end of stream.
 *
 * Every call writes into `dst` (capacity `dst_len`) and returns the number of
 * bits written, or a negative value on a bad argument such as too little room.
 *
 * Two behaviours to plan for:
 *   - Delay. A bit is committed only after a fixed look-ahead (the decision
 *     depth), so output trails input and the first ~decision_depth decoded bits
 *     are unreliable warm-up - discard them, or send a known preamble you skip.
 *   - Buffering. The decoder keeps recovered bits that don't fit in `dst`
 *     rather than dropping them. When a decode call returns exactly `dst_len`
 *     (it filled the buffer), call decode again with no new input (src_len 0)
 *     to drain the rest before feeding more.
 *
 *   `src` is the received transmit-domain stream (DT_TRUE / DT_FALSE /
 *         DT_ERASURE / DT_INVALID) on an unknown, drifting grid.
 *   `dst` is one output-domain symbol per recovered information position:
 *         DT_TRUE / DT_FALSE recovered, DT_ERASURE for a tracked position whose
 *         value was lost (don't-know), DT_INVALID for a recovered non-value, or
 *         DT_ABSENT for a position judged deleted or not synchronized.
 *
 * `data` is the implementation's private state - do not touch it. Build a
 * decoder with a factory such as dt_cc_hybrid_decoder_create() and free it with
 * the matching _destroy().
 */
/* clang-format on */
typedef struct dt_stream_decoder_t dt_stream_decoder;
struct dt_stream_decoder_t {
  // Initialise the decoder and write any preamble. Call once, before decode().
  int (*begin)(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len);
  // Decode src_len received bits, writing recovered bits to dst.
  int (*decode)(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len, const dt_bit *src,
                size_t src_len);
  // Drain bits still in flight. Call once, at end of stream.
  int (*finalize)(dt_stream_decoder *dec, dt_bit *dst, size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_STREAM_DECODER_H */
