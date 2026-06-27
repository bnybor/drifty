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

#ifndef DRIFTY_CC_ENCODER_ENCODE_H
#define DRIFTY_CC_ENCODER_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include <drifty/result.h> /* DT_OK / DT_ERR_* */
#include <drifty/bit.h>
#include <drifty/cc/ccode.h> /* dt_cc_code (the code handle these functions take) */

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * encoder - the convolutional encode side, standalone: it depends only on
 * the code (dt_cc_code), not on any other codec, and every codec encodes through
 * it. It is the ordinary convolutional encoder, and it also tracks non-boolean
 * inputs - a DT_INVALID input emits DT_INVALID coded bits (structural poison),
 * and a DT_ERASURE input emits DT_ERASURE coded bits (an unbound value deferred
 * to the channel). Pair it with whatever decoder shares the same code (e.g. maxir
 * or bcjr, which read those markers).
 *
 *   dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
 *
 *   // encode (in one or more chunks)
 *   int state = 0;
 *   unsigned int unknown = 0;
 *   int len  = dt_cc_encoder_encode(code, bits, n_bits, &state, &unknown, out);
 *   len     += dt_cc_encoder_flush(code, &state, &unknown, out + len);
 *
 *   dt_cc_code_destroy(code);
 *
 * dt_cc_code is an opaque handle - create and free it with the matching functions.
 * Functions return DT_OK (0) or a count on success, or a negative DT_ERR_* code.
 */
/* clang-format on */

/* ------------------------------------------------------------------------- */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Encode `n_bits` input bits into `out`, which needs room for
 * n_bits * dt_cc_code_n(code) bits. Each input is normally DT_FALSE or DT_TRUE; a
 * non-boolean input has no boolean value to encode, and the coded bits that
 * would carry it are marked per kind: a DT_INVALID input emits DT_INVALID
 * (structural poison the decoder reads as a deliberate tie at that position),
 * while a DT_ERASURE input emits DT_ERASURE (an unbound value deferred to the
 * channel, which may concretize it - encoder output is not decoder input).
 *
 * Encoding is one continuous stream: keep an `int state` and an
 * `unsigned int unknown` (the in-flight poison register), set both to 0 before
 * the first call, and pass the same variables to every call - so you can encode
 * in as many chunks as you like. When the whole message is encoded, call
 * dt_cc_encoder_flush() once to finish it.
 *
 * Returns the number of bits written, or DT_ERR_ARG.
 */
int dt_cc_encoder_encode(const dt_cc_code *code, const uint8_t *bits,
                              int n_bits, int *state, unsigned int *unknown,
                              uint8_t *out);

/*
 * Finish an encoded stream: writes (K-1) * dt_cc_code_n(code) trailing bits so the
 * decoder can recover the last input bits cleanly. Pass the same `state` and
 * `unknown` you gave dt_cc_encoder_encode(). Returns the number of bits
 * written, or DT_ERR_ARG.
 */
int dt_cc_encoder_flush(const dt_cc_code *code, int *state,
                             unsigned int *unknown, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_ENCODER_ENCODE_H */
