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

#ifndef DRIFTY_VINDEL_DECODE_H
#define DRIFTY_VINDEL_DECODE_H

/* The vindel decoder is the ported drift_viterbi algorithm. It is built from a
 * dt_ccode and shares the result codes defined alongside the encoder;
 * dt_vindel_stream_decoder_create takes the dt_vindel_stream_params channel
 * model, which lives in <drifty/vindel.h>. */
#include <drifty/vindel.h>
#include "encode.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * The receiver's half of the vindel codec (the sender's half is in encode.h).
 *
 * Recover the original bits from a received stream: feed received bits to a
 * decoder and read the bits back, with flipped, inserted and dropped bits
 * corrected.
 *
 *   dt_vindel_stream_decoder *d = dt_vindel_stream_decoder_create(code,
 *       &(dt_vindel_stream_params){
 *           .decision_depth = 40, .max_drift = 4,
 *           .p_sub = 0.01, .p_ins = 0.01, .p_del = 0.01,
 *       });
 *   int n = dt_vindel_stream_decode(d, in, n_in, out, NULL, out_cap);
 *   while (dt_vindel_stream_decode_flush(d, out, out_cap) > 0) { }
 *
 *   dt_vindel_stream_decoder_destroy(d);
 *
 * `code` must be the same one the sender used, and must stay alive until the
 * decoder is freed. dt_vindel_stream_decoder is an opaque handle.
 *
 * Bits crossing this boundary are dt_t symbols (DT_FALSE / DT_TRUE, and
 * DT_ERASURE on input to mark a lost bit); the engine converts to and from its
 * internal representation at the edges.
 */
/* clang-format on */
typedef struct dt_vindel_stream_decoder dt_vindel_stream_decoder;

/* dt_vindel_stream_params (the decoder channel-model settings) is defined in
 * <drifty/vindel.h>, included above. */

/*
 * Make a decoder for `code` (which must stay alive until the decoder is freed)
 * using `params`. Returns NULL on invalid settings or out of memory; free it
 * with dt_vindel_stream_decoder_destroy().
 */
dt_vindel_stream_decoder *dt_vindel_stream_decoder_create(
    const dt_ccode *code, const dt_vindel_stream_params *params);

/* Free a decoder. Passing NULL is fine. */
void dt_vindel_stream_decoder_destroy(dt_vindel_stream_decoder *d);

/*
 * Feed `n_in` received bits (each DT_FALSE, DT_TRUE, or DT_ERASURE) and collect
 * up to `max_out` decoded bits into `out`. Returns how many decoded bits were
 * written (0 or more), or a negative DT_ERR_* code.
 *
 * You get about one decoded bit per dt_ccode_n(code) received bits. If `out`
 * fills up (return value == max_out), call again to collect more before feeding
 * more input.
 *
 * `lock_probability`, which may be NULL, stores the probability at each bit
 * position that the decoder is locked onto a valid coded bit stream. Same size
 * as `out`.
 */
int dt_vindel_stream_decode(dt_vindel_stream_decoder *d, const uint8_t *in,
                            int n_in, uint8_t *out, float *lock_probability,
                            int max_out);

/*
 * Call at the end of the stream to get the last decoded bits still in flight.
 * Returns how many bits were written (0..max_out); call it repeatedly until it
 * returns 0, after which every bit has been decoded.
 */
int dt_vindel_stream_decode_flush(dt_vindel_stream_decoder *d, uint8_t *out,
                                  int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_VINDEL_DECODE_H */
