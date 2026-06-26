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

#ifndef DRIFTY_VITERBI_DECODE_H
#define DRIFTY_VITERBI_DECODE_H

/* The decoder is built from a dt_ccode and shares the result codes defined
 * alongside the encoder. */
#include "encode.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The receiver's half of the viterbi codec (the sender's half is in encode.h).
 *
 * A plain Viterbi hard-decision decoder: recover the original bits from a
 * received stream, correcting flipped bits. It does not track inserted or
 * dropped bits, and takes no channel-model parameters - the decoder is built
 * from the code alone.
 *
 *   dt_viterbi_stream_decoder *d = dt_viterbi_stream_decoder_create(code);
 *   int n = dt_viterbi_stream_decode(d, in, n_in, out, out_cap);
 *   while (dt_viterbi_stream_decode_flush(d, out, out_cap) > 0) { }
 *   dt_viterbi_stream_decoder_destroy(d);
 *
 * `code` must be the same one the sender used, and must stay alive until the
 * decoder is freed. dt_viterbi_stream_decoder is an opaque handle. Bits crossing
 * this boundary are dt_bit symbols (DT_FALSE / DT_TRUE / DT_ERASURE).
 */
typedef struct dt_viterbi_stream_decoder dt_viterbi_stream_decoder;

/*
 * Make a decoder for `code` (which must stay alive until the decoder is freed).
 * Returns NULL on a bad argument or out of memory; free it with
 * dt_viterbi_stream_decoder_destroy().
 */
dt_viterbi_stream_decoder *dt_viterbi_stream_decoder_create(const dt_ccode *code);

/* Free a decoder. Passing NULL is fine. */
void dt_viterbi_stream_decoder_destroy(dt_viterbi_stream_decoder *d);

/*
 * Feed `n_in` received bits (each DT_FALSE, DT_TRUE, or DT_ERASURE) and collect
 * up to `max_out` decoded bits into `out`. Returns how many decoded bits were
 * written (0 or more), or a negative DT_ERR_* code. If `out` fills up (return
 * value == max_out), call again to collect more before feeding more input.
 */
int dt_viterbi_stream_decode(dt_viterbi_stream_decoder *d, const uint8_t *in,
                             int n_in, uint8_t *out, int max_out);

/*
 * Call at the end of the stream to get the last decoded bits still in flight.
 * Returns how many bits were written (0..max_out); call it repeatedly until it
 * returns 0, after which every bit has been decoded.
 */
int dt_viterbi_stream_decode_flush(dt_viterbi_stream_decoder *d, uint8_t *out,
                                   int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_VITERBI_DECODE_H */
