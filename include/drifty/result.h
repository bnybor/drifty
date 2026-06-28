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

#ifndef DRIFTY_RESULT_H
#define DRIFTY_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes shared across drifty, for functions that don't return a count.
 * Encoders and decoders return DT_OK (0) or a count on success, or a negative
 * DT_ERR_* code; an incremental operation with more work left returns DT_AGAIN,
 * asking the caller to call again. */
typedef enum dt_result_t {
  DT_OK = 0,
  DT_ERR_ARG = -1,    /* a bad argument was passed              */
  DT_ERR_ALLOC = -2,  /* ran out of memory                     */
  DT_AGAIN = -3,      /* incremental op not finished; call again */
  DT_ERR_DECODE = -4  /* the input could not be decoded          */
} dt_result;

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_RESULT_H */
