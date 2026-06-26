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

#ifndef DRIFTY_CCODE_INTERNAL_H
#define DRIFTY_CCODE_INTERNAL_H

#include <stdint.h>

/*
 * Definition of the opaque dt_ccode, shared by the encoder (encode.c) and the
 * decoder (decode.c). Private to the library build - not installed.
 */
struct dt_ccode {
  int K;                    /* constraint length                   */
  int n;                    /* output bits per input bit           */
  int n_states;             /* 1 << (K-1)                          */
  unsigned int input_tap;   /* 1 << (K-1)                          */
  unsigned int *generators; /* [n]                                 */
  int *next_state;          /* [n_states*2], indexed [s*2 + b]     */
  uint8_t *output;          /* [n_states*2*n], [((s*2)+b)*n + j]   */
};

#endif /* DRIFTY_CCODE_INTERNAL_H */
