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

#ifndef DRIFTY_CCODE_H
#define DRIFTY_CCODE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A dt_ccode is the convolutional code - the redundancy scheme the encoder and
 * decoder share. Pick a ready-made one with dt_ccode_create_standard() (most
 * users want this) or define your own with dt_ccode_create(), then build a
 * codec over it with the dt_hybrid_*_create() factories in <drifty/cc/hybrid.h>.
 * The code must outlive everything built from it.
 */

/* ------------------------------------------------------------------------- */
/* Code (the error-correction scheme)                                        */
/* ------------------------------------------------------------------------- */

/* A code is the redundancy scheme; sender and receiver must use the same one.
 * Opaque handle - make it below, free it with dt_ccode_destroy(). */
typedef struct dt_ccode dt_ccode;

/* Ready-made codes. More output per input bit corrects more errors but uses
 * more bandwidth; when unsure, pick DT_CODE_K7_RATE_1_2. Each rate/K family is
 * a default plus a few alternates, all picked to be mutually distinguishable: a
 * decoder for one will not lock onto another's stream. How many alternates a
 * family has depends
 * on how many distinguishable codes its generator space actually holds - the
 * rate-1/2 families support three apiece, the rate-1/3 and rate-1/5 families
 * five. The alternates trade a little free distance for that separation. The
 * trailing comment is each code's free distance: higher corrects more. */
typedef enum {
  DT_CODE_K3_RATE_1_2, /* 2x output size; d_free 5 (good default for K=3) */
  DT_CODE_K3_RATE_1_2_ALT1, /* 2x output size; d_free 4 */
  DT_CODE_K3_RATE_1_2_ALT2, /* 2x output size; d_free 4 */
  DT_CODE_K7_RATE_1_2, /* 2x output size; d_free 10 (good default)        */
  DT_CODE_K7_RATE_1_2_ALT1, /* 2x output size; d_free 8 */
  DT_CODE_K7_RATE_1_2_ALT2, /* 2x output size; d_free 8 */
  DT_CODE_K7_RATE_1_3, /* 3x output size; d_free 15                       */
  DT_CODE_K7_RATE_1_3_ALT1, /* 3x output size; d_free 14 */
  DT_CODE_K7_RATE_1_3_ALT2, /* 3x output size; d_free 13 */
  DT_CODE_K7_RATE_1_3_ALT3, /* 3x output size; d_free 12 */
  DT_CODE_K7_RATE_1_3_ALT4, /* 3x output size; d_free 12 */
  DT_CODE_K5_RATE_1_5, /* 5x output size; d_free 20 (strongest)           */
  DT_CODE_K5_RATE_1_5_ALT1, /* 5x output size; d_free 18 */
  DT_CODE_K5_RATE_1_5_ALT2, /* 5x output size; d_free 18 */
  DT_CODE_K5_RATE_1_5_ALT3, /* 5x output size; d_free 17 */
  DT_CODE_K5_RATE_1_5_ALT4 /* 5x output size; d_free 17                       */
} dt_standard_code;

/* Make one of the ready-made codes above. Returns NULL on a bad argument or out
 * of memory; free the result with dt_ccode_destroy(). */
dt_ccode *dt_ccode_create_standard(dt_standard_code which);

/*
 * Make a custom code - most users want dt_ccode_create_standard() instead. `K`
 * is the code's memory (2..9); `generators` is an array of `num_generators` tap
 * masks of K bits each, and `num_generators` sets how many output bits each
 * input bit becomes. Returns NULL on a bad argument or out of memory.
 */
dt_ccode *dt_ccode_create(int K, const unsigned int *generators,
                        int num_generators);

/* Free a code. Passing NULL is fine. */
void dt_ccode_destroy(dt_ccode *code);

/* Output bits produced per input bit, so the encoded size of n_bits is
 * n_bits * dt_ccode_n(code). Returns -1 if code is NULL. */
int dt_ccode_n(const dt_ccode *code);

/* The code's memory length K. A good decision_depth is about 6 * K. Returns -1
 * if code is NULL. */
int dt_ccode_k(const dt_ccode *code);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CCODE_H */
