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

#ifndef DRIFTY_DETECT_CLEAN_DECODE_H
#define DRIFTY_DETECT_CLEAN_DECODE_H

/* The decoder shares the cc result codes. It is standalone (no dt_cc_code) but
 * takes the dt_cc_detect_clean_stream_params channel model, which lives in
 * <drifty/cc/detect_clean.h>. */
#include <drifty/cc/detect_clean.h>
#include <drifty/result.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The receiver's half of the detect_clean codec - a blind detector of convolutional-code
 * structure in an arbitrary bit stream (no code, rate, generators, or alignment
 * known). It reports, per stream position, a confidence that the stream carries a
 * convolutional code vs that it does not. See decode.c for the method (GF(2)
 * strided-window rank deficiency).
 *
 *   dt_cc_detect_clean_stream_decoder *d = dt_cc_detect_clean_stream_decoder_create(
       &(dt_cc_detect_clean_stream_params){ .decision_depth = 40 });
 *   dt_cc_detect_clean_decode_details det[CAP];
 *   int n = dt_cc_detect_clean_stream_decode(d, in, n_in, det, CAP);
 *   while (dt_cc_detect_clean_stream_decode_flush(d, det, CAP) > 0) { }
 *   dt_cc_detect_clean_stream_decoder_destroy(d);
 *
 * dt_cc_detect_clean_stream_decoder is an opaque handle.
 */
typedef struct dt_cc_detect_clean_stream_decoder dt_cc_detect_clean_stream_decoder;

/*
 * Per-position detection output. Two consistencies in [0, 1] (how well each
 * hypothesis fits the local stream - not a probability split, so they need not
 * sum to 1):
 */
typedef struct {
  /* Confidence that a convolutional code IS encoded onto the stream here. */
  float c_lost;
  /* Confidence that a convolutional code is NOT encoded onto the stream here. */
  float c_absent;
} dt_cc_detect_clean_decode_details;

/*
 * Make a decoder with the channel model in `params` (copied; need not outlive the
 * call). Returns NULL on invalid settings or out of memory; free it with
 * dt_cc_detect_clean_stream_decoder_destroy().
 */
dt_cc_detect_clean_stream_decoder *dt_cc_detect_clean_stream_decoder_create(
    const dt_cc_detect_clean_stream_params *params);

/* Free a decoder. Passing NULL is fine. */
void dt_cc_detect_clean_stream_decoder_destroy(dt_cc_detect_clean_stream_decoder *d);

/*
 * Feed `n_in` received bits and collect up to `max_out` per-position detection
 * records into `details`. Returns how many records were written (0 or more), or a
 * negative DT_ERR_* code. If `details` fills up (return value == max_out), call
 * again to collect more before feeding more input. `details` may be NULL (the
 * input is still consumed). One record is produced per input bit, but a record is
 * only emitted once its analysis block is complete, so output trails input by up
 * to one block.
 */
int dt_cc_detect_clean_stream_decode(dt_cc_detect_clean_stream_decoder *d, const uint8_t *in,
                            int n_in, dt_cc_detect_clean_decode_details *details,
                            int max_out);

/*
 * Call at the end of the stream to get the last records still in flight. Returns
 * how many were written (0..max_out); call repeatedly until it returns 0.
 */
int dt_cc_detect_clean_stream_decode_flush(dt_cc_detect_clean_stream_decoder *d,
                                 dt_cc_detect_clean_decode_details *details,
                                 int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_DETECT_CLEAN_DECODE_H */
