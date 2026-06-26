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

#ifndef DRIFTY_HYBRID_MULTI_ENCODE_H
#define DRIFTY_HYBRID_MULTI_ENCODE_H

#include "encode.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The sender's mirror of dt_multi_decoder: holds a family of codes that share a
 * rate and constraint length and one encoder state shared across them, and on
 * each call encodes with a code you pick by index. Opaque handle - make it with
 * dt_multi_encode_create(), free it with dt_multi_encode_destroy().
 */
typedef struct dt_multi_encoder dt_multi_encoder;

/* clang-format off */
/*
 * Settings for dt_multi_encode_create().
 *
 *   codes     : array of `codes_len` codes to encode with, selected per call by
 *               index. Required. They must share a rate (same dt_ccode_n) AND a
 *               constraint length (same dt_ccode_k): one shared encoder state
 *               drives whichever code is chosen, which is well defined precisely
 *               because a convolutional encoder's state transition depends only
 *               on the constraint length and the input bits, not the generator
 *               polynomials - so a same-K family steps one state in lockstep and
 *               typically differs only in its generators. A set that violates
 *               this is rejected (dt_multi_encode_create returns NULL). Each code
 *               must outlive the encoder; the array is copied, the codes are not.
 *   codes_len : how many codes `codes` points to.
 */
/* clang-format on */
typedef struct {
  const dt_ccode *const *codes;
  size_t codes_len;
} dt_multi_encode_params;

/*
 * Allocate a multi-encoder from `params` (must be non-NULL). The codes must share
 * a rate (dt_ccode_n) and a constraint length (dt_ccode_k), and each must outlive
 * the encoder (it copies the array and borrows the codes; it frees neither in
 * dt_multi_encode_destroy()). Returns NULL on a NULL params/codes, a NULL or
 * mismatched-rate/length code entry, or out of memory.
 */
dt_multi_encoder *dt_multi_encode_create(const dt_multi_encode_params *params);

/* Free a multi-encoder. The codes it borrowed are left untouched. NULL is fine. */
void dt_multi_encode_destroy(dt_multi_encoder *e);

/*
 * Encode `n_bits` input bits (each DT_FALSE or DT_TRUE) with codes[idx], writing
 * n_bits * dt_ccode_n bits to `out` (capacity `max_out`). The one encoder state is
 * shared across all the codes and advances here, so encoding is one continuous
 * stream across calls - and because the state is code-independent for a same-K
 * family, you may switch `idx` between calls to change codes mid-stream. Call
 * dt_multi_encode_flush() once the whole message is encoded.
 *
 * Returns the number of bits written, or DT_ERR_ARG (bad handle, idx out of
 * range, bad in/out arguments, or the output would not fit in max_out).
 */
int dt_multi_encode(dt_multi_encoder *e, int idx, const uint8_t *bits, int n_bits,
                    uint8_t *out, int max_out);

/*
 * Finish an encoded stream with codes[idx]: writes (dt_ccode_k - 1) * dt_ccode_n
 * trailing bits to `out` (capacity `max_out`) and returns the shared state to 0.
 * Mirror of dt_ccode_encode_flush. Returns the number of bits written, or
 * DT_ERR_ARG.
 */
int dt_multi_encode_flush(dt_multi_encoder *e, int idx, uint8_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_HYBRID_MULTI_ENCODE_H */
