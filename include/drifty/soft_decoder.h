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

typedef struct dt_soft_decoder_out_t dt_soft_decoder_out;
struct dt_soft_decoder_out_t {
  /*
   * All c_ values are consistency measures (not probabilities) ranging 0...1
   * Consistency is of a hypothesis.
   */

  // The bit position holds false
  double c_false;  // DT_FALSE
  // The bit position holds true
  double c_true;  // DT_TRUE
  // The bit position has an unknowable value
  double c_erasure;  // DT_ERASURE
  // The bit position has a value, but it is neither true nor false
  double c_invalid;  // DT_INVALID
  // The bit position was deleted
  double c_absent;  // DT_ABSENT

  // Hypothesis that the decoder is valid for this stream
  double c_locked;
};

/*
 * A decoder for DT_ bit positions.
 *
 * `src` may contain only:
 * - DT_TRUE
 * - DT_FALSE
 * - DT_ERASURE
 * - DT_INVALID
 */
typedef struct dt_soft_decoder_t dt_soft_decoder;
struct dt_soft_decoder_t {
  // Initialize the decoder, and write any preamble
  int (*begin)(dt_soft_decoder *dec, dt_t *dst, size_t dst_len);
  // Decode bits
  int (*decode)(dt_soft_decoder *dec, dt_soft_decoder_out *dst, size_t dst_len,
                const dt_t *src, size_t src_len);
  // Finish decoding any in-progress bits and write any trailer.
  int (*finalize)(dt_soft_decoder *dec, dt_soft_decoder_out *dst,
                  size_t dst_len);

  // implementation-specific state
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_SOFT_DECODER_H */
