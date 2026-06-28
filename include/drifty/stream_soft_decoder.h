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

#ifndef DRIFTY_STREAM_SOFT_DECODER_H
#define DRIFTY_STREAM_SOFT_DECODER_H

#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_stream_soft_decoder - like dt_stream_decoder, but each recovered position is reported as
 * a dt_soft_bit record of consistencies rather than a single hard bit.
 * It is driven and behaves identically otherwise: the same begin / decode /
 * finalize phases, the same warm-up delay, and the same buffering - a decode
 * call that returns exactly `dst_len` records has more buffered, so call again
 * with no new input (src_len 0) to drain before feeding more.
 *
 * `begin` consumes any preamble at the head of `src` (the received hard-bit
 * stream, as decode() takes); the hybrid codec uses none, so begin(dec, NULL, 0)
 * is fine. `src` is the received transmit-domain stream (DT_TRUE / DT_FALSE /
 * DT_ERASURE / DT_INVALID); `data` is private state - do not touch it. Build one
 * with dt_cc_hybrid_soft_decoder_create() and free it with the matching _destroy().
 */
typedef struct dt_stream_soft_decoder_t dt_stream_soft_decoder;
struct dt_stream_soft_decoder_t {
  // Initialise the decoder and consume any preamble at the head of `src`. Call
  // once, before decode(). Returns preamble bits consumed (0 if none).
  int (*begin)(dt_stream_soft_decoder *dec, const dt_bit *src, size_t src_len);
  // Decode src_len received bits, writing up to dst_len soft records to dst.
  int (*decode)(dt_stream_soft_decoder *dec, dt_soft_bit *dst, size_t dst_len,
                const dt_bit *src, size_t src_len);
  // Drain records still in flight. Call once, at end of stream.
  int (*finalize)(dt_stream_soft_decoder *dec, dt_soft_bit *dst,
                  size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_STREAM_SOFT_DECODER_H */
