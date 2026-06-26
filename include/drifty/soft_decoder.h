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

#ifndef DRIFTY_SOFT_DECODER_H
#define DRIFTY_SOFT_DECODER_H

#include <drifty/bit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Soft output for one decoded bit position. Each c_ field is a *consistency* in
 * [0, 1] - how well that hypothesis fits the received stream - not a
 * probability, so the fields need not sum to 1; compare them to decide. The
 * usual hard decision is DT_ERASURE if c_erasure dominates, else whichever of
 * c_true / c_false is larger.
 *
 * An implementation need not model every hypothesis: the hybrid codec leaves
 * c_invalid and c_absent at 0.
 */
typedef struct dt_soft_decoder_out_t dt_soft_decoder_out;
struct dt_soft_decoder_out_t {
  // Consistency that the bit position holds false (DT_FALSE)
  double c_false;
  // Consistency that the bit position holds true (DT_TRUE)
  double c_true;
  // Consistency that the value is unrecoverable (DT_ERASURE)
  double c_erasure;
  // Consistency of a bound, non-boolean value (DT_INVALID)
  double c_invalid;
  // Consistency that the position was deleted (DT_ABSENT)
  double c_absent;

  // Consistency that the decoder is correctly tracking this stream - low during
  // warm-up or after losing sync. Independent of the value fields above.
  double c_locked;
};

/*
 * dt_soft_decoder - like dt_decoder, but each recovered position is reported as
 * a dt_soft_decoder_out record of consistencies rather than a single hard bit.
 * It is driven and behaves identically otherwise: the same begin / decode /
 * finalize phases, the same warm-up delay, and the same buffering - a decode
 * call that returns exactly `dst_len` records has more buffered, so call again
 * with no new input (src_len 0) to drain before feeding more.
 *
 * `begin` writes any preamble into `dst`; the hybrid codec emits none, so
 * begin(dec, NULL, 0) is fine. `src` holds received DT_TRUE / DT_FALSE /
 * DT_ERASURE (and may carry DT_INVALID); `data` is private state - do not touch
 * it. Build one with dt_hybrid_soft_decoder_create() and free it with the
 * matching _destroy().
 */
typedef struct dt_soft_decoder_t dt_soft_decoder;
struct dt_soft_decoder_t {
  // Initialise the decoder and write any preamble. Call once, before decode().
  int (*begin)(dt_soft_decoder *dec, dt_t *dst, size_t dst_len);
  // Decode src_len received bits, writing up to dst_len soft records to dst.
  int (*decode)(dt_soft_decoder *dec, dt_soft_decoder_out *dst, size_t dst_len,
                const dt_t *src, size_t src_len);
  // Drain records still in flight. Call once, at end of stream.
  int (*finalize)(dt_soft_decoder *dec, dt_soft_decoder_out *dst,
                  size_t dst_len);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_SOFT_DECODER_H */
