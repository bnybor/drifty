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

#ifndef DRIFTY_BASIC_ENCODE_H
#define DRIFTY_BASIC_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include <drifty/bit.h>
#include <drifty/cc/ccode.h> /* dt_ccode (the code handle these functions take) */

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * basic encoder - the plain convolutional encode side, standalone: it depends
 * only on the code (dt_ccode), not on any other codec. It adds redundancy to a
 * stream of bits; it corrects nothing on its own. Pair it with whatever decoder
 * shares the same code.
 *
 *   dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
 *
 *   // encode (in one or more chunks)
 *   int state = 0;
 *   int len  = dt_basic_encode(code, bits, n_bits, &state, out);
 *   len     += dt_basic_encode_flush(code, &state, out + len);
 *
 *   dt_ccode_destroy(code);
 *
 * dt_ccode is an opaque handle - create and free it with the matching functions.
 * Functions return DT_OK (0) or a count on success, or a negative DT_ERR_* code.
 */
/* clang-format on */

/* Result codes for functions that don't return a count. */
enum {
  DT_OK = 0,
  DT_ERR_ARG = -1,  /* a bad argument was passed */
  DT_ERR_ALLOC = -2 /* ran out of memory         */
};

/* ------------------------------------------------------------------------- */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Encode `n_bits` input bits (each DT_FALSE or DT_TRUE) into `out`, which needs
 * room for n_bits * dt_ccode_n(code) bits.
 *
 * Encoding is one continuous stream: keep an `int state`, set it to 0 before
 * the first call, and pass the same variable to every call - so you can encode
 * in as many chunks as you like. When the whole message is encoded, call
 * dt_basic_encode_flush() once to finish it.
 *
 * Returns the number of bits written, or DT_ERR_ARG.
 */
int dt_basic_encode(const dt_ccode *code, const uint8_t *bits, int n_bits,
                    int *state, uint8_t *out);

/*
 * Finish an encoded stream: writes (K-1) * dt_ccode_n(code) trailing bits so the
 * decoder can recover the last input bits cleanly. Pass the same `state` you
 * gave dt_basic_encode(). Returns the number of bits written, or DT_ERR_ARG.
 */
int dt_basic_encode_flush(const dt_ccode *code, int *state, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_BASIC_ENCODE_H */
