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

#ifndef DRIFTY_ENCODER_H
#define DRIFTY_ENCODER_H

#include <drifty/bit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_encoder - the encode side of the streaming codec, called through function
 * pointers. It turns a stream of input bits into a longer stream of coded bits.
 *
 * Drive one instance through three phases:
 *
 *   begin    - once, first: initialise and write any preamble.
 *   encode   - any number of times: append the coded bits for `src`.
 *   finalize - once, last: flush the tail (terminating bits) at end of stream.
 *
 * Every call writes into `dst` (capacity `dst_len`) and returns the number of
 * bits written, or a negative value on a bad argument - most often too little
 * room. Size `dst` from the input length and dt_cc_code_n(): n input bits become
 * up to (n + K) * dt_cc_code_n(code) coded bits once the flush is counted.
 *
 *   `src` is one transmit-domain symbol per information position: DT_TRUE /
 *         DT_FALSE to protect, DT_ERASURE for a don't-care value deferred to the
 *         channel, or DT_INVALID for a deliberate non-value sent as such.
 *   `dst` is the coded transmit-domain stream: the code's booleans, except a
 *         DT_INVALID input's coded positions are emitted DT_INVALID (structural
 *         poison) and a DT_ERASURE input's are emitted DT_ERASURE (deferred).
 *
 * `data` is the implementation's private state - do not touch it. Build an
 * encoder with a factory such as dt_cc_basic_encoder_create() and free it with
 * the matching _destroy().
 */
typedef struct dt_encoder_t dt_encoder;
struct dt_encoder_t {
  // Initialise the encoder and write any preamble. Call once, before encode().
  int (*begin)(dt_encoder *enc, dt_bit *dst, size_t dst_len);
  // Encode src_len input bits, appending the coded bits to dst.
  int (*encode)(dt_encoder *enc, dt_bit *dst, size_t dst_len, const dt_bit *src,
                size_t src_len);
  // Flush in-progress bits and write the trailer. Call once, at end of stream.
  int (*finalize)(dt_encoder *enc, dt_bit *dst, size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_ENCODER_H */
