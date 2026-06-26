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

#ifndef DRIFTY_MAXIR_DECODE_H
#define DRIFTY_MAXIR_DECODE_H

/* The decoder is built from a dt_ccode and shares the result codes defined
 * alongside the encoder. dt_maxir_stream_decoder_create takes the
 * dt_maxir_stream_params channel model, which lives in <drifty/cc/maxir.h>. */
#include <drifty/cc/maxir.h>
#include "encode.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * DIVERGED FROM BCJR (2026-06-26): maxir is an independent fork of bcjr - do not
 * port bcjr changes here. See include/drifty/cc/maxir.h.
 *
 * The receiver's half of the maxir codec (the sender's half is in encode.h).
 *
 * A MAXIR (MAP / forward-backward) decoder: recover the original bits from a
 * received stream, correcting flipped and erased bits. Unlike the Viterbi
 * decoder, which returns the single most likely path, MAXIR weighs every path to
 * give each input bit's a-posteriori probability - so a soft decision falls out
 * of the same recursions. It does not track inserted or dropped bits.
 *
 *   dt_maxir_stream_decoder *d = dt_maxir_stream_decoder_create(code,
 *       &(dt_maxir_stream_params){ .decision_depth = 40, .p_flip = 0.01 });
 *   int n = dt_maxir_stream_decode(d, in, n_in, out, NULL, out_cap);
 *   while (dt_maxir_stream_decode_flush(d, out, NULL, out_cap) > 0) { }
 *   dt_maxir_stream_decoder_destroy(d);
 *
 * `code` must be the same one the sender used, and must stay alive until the
 * decoder is freed. dt_maxir_stream_decoder is an opaque handle. Bits crossing
 * this boundary are dt_bit symbols (DT_FALSE / DT_TRUE / DT_ERASURE).
 *
 * STUB: the forward-backward recursions are not yet implemented. The handle
 * is created and the channel model validated, but decode/flush currently emit
 * no bits.
 */
/* clang-format on */
typedef struct dt_maxir_stream_decoder dt_maxir_stream_decoder;

/*
 * Per-bit soft output the MAXIR decoder produces alongside (or instead of) the
 * hard decision. Each c_ field is a consistency in [0, 1] - how well that
 * hypothesis fits the received stream - not a probability split, so they need
 * not sum to 1.
 */
typedef struct {
  /* Consistency that the decoded info bit was true / false (its a-posteriori
   * weight). */
  float c_true;
  float c_false;
  /* Consistency that the value is unrecoverable (an erasure to an outer code). */
  float c_lost;
  /* Consistency that the slot's coded group was the encoder's DT_INVALID poison
   * marker - a bound, non-boolean value. */
  float c_invalid;
  /* Consistency that the slot is not backed by a tracked codeword stream
   * (1 - c_lock); surfaces as DT_ABSENT. */
  float c_absent;
  /* Consistency that the decoder is tracking a valid stream of this code. */
  float c_lock;
} dt_maxir_decode_details;

/*
 * Make a decoder for `code` (which must stay alive until the decoder is freed)
 * using `params`. Returns NULL on invalid settings or out of memory; free it
 * with dt_maxir_stream_decoder_destroy().
 */
dt_maxir_stream_decoder *dt_maxir_stream_decoder_create(
    const dt_ccode *code, const dt_maxir_stream_params *params);

/* Free a decoder. Passing NULL is fine. */
void dt_maxir_stream_decoder_destroy(dt_maxir_stream_decoder *d);

/*
 * Feed `n_in` received bits (each DT_FALSE, DT_TRUE, or DT_ERASURE) and collect
 * up to `max_out` decoded bits into `out`. Returns how many decoded bits were
 * written (0 or more), or a negative DT_ERR_* code. If `out` fills up (return
 * value == max_out), call again to collect more before feeding more input.
 *
 * `out` and `details` may both be NULL. If supplied, they must be arrays of
 * length max_out: `out` receives the hard decision (DT_TRUE / DT_FALSE /
 * DT_ERASURE) and `details` the per-bit soft output for the same positions.
 */
int dt_maxir_stream_decode(dt_maxir_stream_decoder *d, const uint8_t *in, int n_in,
                          uint8_t *out, dt_maxir_decode_details *details,
                          int max_out);

/*
 * Call at the end of the stream to get the last decoded bits still in flight.
 * Returns how many bits were written (0..max_out); call it repeatedly until it
 * returns 0, after which every bit has been decoded.
 */
int dt_maxir_stream_decode_flush(dt_maxir_stream_decoder *d, uint8_t *out,
                                dt_maxir_decode_details *details, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_MAXIR_DECODE_H */
