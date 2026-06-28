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

#ifndef DRIFTY_SOFT_BIT_H
#define DRIFTY_SOFT_BIT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_soft_bit - the soft-decision counterpart of a hard dt_bit (<drifty/bit.h>):
 * the soft output for one recovered information position. A graded *consistency*
 * in [0, 1] for each output-domain hypothesis - how well it fits the received
 * stream - not a probability split, so the fields need not sum to 1. The hard
 * symbol is the argmax projection over the alphabet (recoverability-first; see
 * stream_decoder.h).
 *
 * An implementation need not model every hypothesis: the hybrid codec leaves
 * c_invalid and c_absent at 0, while the max-log-MAP codecs (bcjr, maxir)
 * populate the full alphabet.
 */
typedef struct dt_soft_bit_t dt_soft_bit;
struct dt_soft_bit_t {
  // Consistency that the bit position holds false (DT_FALSE)
  float c_false;
  // Consistency that the bit position holds true (DT_TRUE)
  float c_true;
  // Consistency that the value is unrecoverable (DT_ERASURE)
  float c_erasure;
  // Consistency of a bound, non-boolean value (DT_INVALID)
  float c_invalid;
  // Consistency that the position was deleted (DT_ABSENT)
  float c_absent;

  // Consistency that the decoder is correctly tracking this stream - low during
  // warm-up or after losing sync. Independent of the value fields above.
  float c_locked;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_SOFT_BIT_H */
