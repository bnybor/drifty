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

#ifndef DRIFTY_ENCODE_H
#define DRIFTY_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * drifty - forward error correction for a stream of bits sent over a
 * noisy channel. It corrects flipped bits, and - unlike most error-correcting
 * codes - also keeps its place when bits are accidentally inserted or dropped.
 *
 * This header is the sender's half: pick a code, then encode your bits (this
 * adds redundancy). The receiver's half - feeding the received bits to a decoder
 * and reading your bits back, with errors corrected - is in decode.h. The code
 * picked here (dt_code) is shared by both; sender and receiver must use the same
 * one.
 *
 *   dt_code *code = dt_code_create_standard(DT_CODE_K7_RATE_1_2);
 *
 *   // encode (in one or more chunks)
 *   int state = 0;
 *   int len  = dt_code_encode(code, bits, n_bits, &state, out);
 *   len     += dt_code_encode_flush(code, &state, out + len);
 *
 *   dt_code_destroy(code);
 *
 * dt_code is an opaque handle - create and free it with the matching functions.
 * Functions return DT_OK (0) or a count on success, or a negative DT_ERR_* code.
 */
/* clang-format on */

/* Result codes for functions that don't return a count. */
enum {
  DT_OK = 0,
  DT_ERR_ARG = -1,  /* a bad argument was passed */
  DT_ERR_ALLOC = -2 /* ran out of memory         */
};

/*
 * Every bit is DT_FALSE or DT_TRUE. In received data you may also mark a bit
 * DT_ERASURE to say "this one was lost"; the decoder then treats it as unknown
 * instead of guessing 0 or 1 (see decode.h).
 */
#define DT_FALSE ((uint8_t)0u)
#define DT_TRUE ((uint8_t)1u)
#define DT_ERASURE ((uint8_t)0xFFu)

/* This library's version, e.g. "0.1.0". */
const char *drifty_version(void);

/* ------------------------------------------------------------------------- */
/* Code (the error-correction scheme)                                        */
/* ------------------------------------------------------------------------- */

/* A code is the redundancy scheme; sender and receiver must use the same one.
 * Opaque handle - make it below, free it with dt_code_destroy(). */
typedef struct dt_code dt_code;

/* Ready-made codes. More output per input bit corrects more errors but uses
 * more bandwidth; when unsure, pick DT_CODE_K7_RATE_1_2. Each rate/K family is a
 * default plus a few alternates, all picked to be mutually distinguishable: a
 * decoder (or dt_compare) for one will not lock onto another's stream (see
 * dt_decode_details' c_lock). How many alternates a family has depends
 * on how many distinguishable codes its generator space actually holds - the
 * rate-1/2 families support three apiece, the rate-1/3 and rate-1/5 families
 * five. The alternates trade a little free distance for that separation. The
 * trailing comment is each code's free distance: higher corrects more. */
typedef enum {
  DT_CODE_K3_RATE_1_2,      /* 2x output size; d_free 5 (good default for K=3) */
  DT_CODE_K3_RATE_1_2_ALT1, /* 2x output size; d_free 4                        */
  DT_CODE_K3_RATE_1_2_ALT2, /* 2x output size; d_free 4                        */
  DT_CODE_K7_RATE_1_2,      /* 2x output size; d_free 10 (good default)        */
  DT_CODE_K7_RATE_1_2_ALT1, /* 2x output size; d_free 8                        */
  DT_CODE_K7_RATE_1_2_ALT2, /* 2x output size; d_free 8                        */
  DT_CODE_K7_RATE_1_3,      /* 3x output size; d_free 15                       */
  DT_CODE_K7_RATE_1_3_ALT1, /* 3x output size; d_free 14                       */
  DT_CODE_K7_RATE_1_3_ALT2, /* 3x output size; d_free 13                       */
  DT_CODE_K7_RATE_1_3_ALT3, /* 3x output size; d_free 12                       */
  DT_CODE_K7_RATE_1_3_ALT4, /* 3x output size; d_free 12                       */
  DT_CODE_K5_RATE_1_5,      /* 5x output size; d_free 20 (strongest)           */
  DT_CODE_K5_RATE_1_5_ALT1, /* 5x output size; d_free 18                       */
  DT_CODE_K5_RATE_1_5_ALT2, /* 5x output size; d_free 18                       */
  DT_CODE_K5_RATE_1_5_ALT3, /* 5x output size; d_free 17                       */
  DT_CODE_K5_RATE_1_5_ALT4  /* 5x output size; d_free 17                       */
} dt_standard_code;

/* Make one of the ready-made codes above. Returns NULL on a bad argument or out
 * of memory; free the result with dt_code_destroy(). */
dt_code *dt_code_create_standard(dt_standard_code which);

/*
 * Make a custom code - most users want dt_code_create_standard() instead. `K`
 * is the code's memory (2..9); `generators` is an array of `num_generators` tap
 * masks of K bits each, and `num_generators` sets how many output bits each
 * input bit becomes. Returns NULL on a bad argument or out of memory.
 */
dt_code *dt_code_create(int K, const unsigned int *generators,
                        int num_generators);

/* Free a code. Passing NULL is fine. */
void dt_code_destroy(dt_code *code);

/* Output bits produced per input bit, so the encoded size of n_bits is
 * n_bits * dt_code_n(code). Returns -1 if code is NULL. */
int dt_code_n(const dt_code *code);

/* The code's memory length K. A good decision_depth is about 6 * K. Returns -1
 * if code is NULL. */
int dt_code_k(const dt_code *code);

/* ------------------------------------------------------------------------- */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Encode `n_bits` input bits (each DT_FALSE or DT_TRUE) into `out`, which needs
 * room for n_bits * dt_code_n(code) bits.
 *
 * Encoding is one continuous stream: keep an `int state`, set it to 0 before
 * the first call, and pass the same variable to every call - so you can encode
 * in as many chunks as you like. When the whole message is encoded, call
 * dt_code_encode_flush() once to finish it.
 *
 * Returns the number of bits written, or DT_ERR_ARG.
 */
int dt_code_encode(const dt_code *code, const uint8_t *bits, int n_bits,
                   int *state, uint8_t *out);

/*
 * Finish an encoded stream: writes (K-1) * dt_code_n(code) trailing bits so the
 * decoder can recover the last input bits cleanly. Pass the same `state` you
 * gave dt_code_encode(). Returns the number of bits written, or DT_ERR_ARG.
 */
int dt_code_encode_flush(const dt_code *code, int *state, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_ENCODE_H */
