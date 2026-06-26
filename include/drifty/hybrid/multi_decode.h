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

#ifndef DRIFTY_HYBRID_MULTI_DECODE_H
#define DRIFTY_HYBRID_MULTI_DECODE_H

#include <drifty/hybrid/decode.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runs several stream decoders over one received stream and, per output bit,
 * keeps the bit from whichever is most confidently locked. Opaque handle - make
 * it with dt_multi_create(), free it with dt_multi_destroy().
 */
typedef struct dt_multi_decoder dt_multi_decoder;

/* clang-format off */
/*
 * Settings for dt_multi_create(). Use designated initializers; any tuning field
 * left 0 takes its default.
 *
 *   codes      : array of `codes_len` codes to decode against in parallel over a
 *                single shared received buffer and cadence. Required. They must
 *                share a rate (same dt_ccode_n) AND a constraint length (same
 *                dt_ccode_k) - one decoded bit corresponds to one message bit
 *                across all of them, and they advance in lockstep under one
 *                shared re-anchor over one trellis geometry - and otherwise
 *                typically differ only in their generator polynomials. A set
 *                that violates this is rejected (dt_multi_create returns NULL).
 *                Each code must outlive the multi-decoder.
 *   codes_len  : how many codes `codes` points to.
 *   stream     : decoder settings (decision_depth, drift, channel probabilities;
 *                see dt_stream_params) shared by every code's decoder. Required.
 *   lock_floor : the lock probability (see dt_stream_decode) the best-fitting
 *                code must reach for any bit to be committed; if none does,
 *                nothing is locked and the bit is DT_ERASURE. Default 0.6.
 *   lock_margin: how decisively the codes' likelihood-weighted vote must favour
 *                one bit value over the other - as a share of the total weight,
 *                so 0 commits on any majority and 1 never commits - for that bit
 *                to be emitted rather than DT_ERASURE. Default 0.2.
 */
/* clang-format on */
typedef struct {
  const dt_ccode *const *codes;
  size_t codes_len;
  dt_stream_params stream;
  double lock_floor;
  double lock_margin;
} dt_multi_params;

/*
 * Allocate a multi-decoder from `params` (must be non-NULL), building one
 * stream decoder per code in params->codes using params->stream. The codes must
 * outlive the multi-decoder; it owns the decoders it builds and frees them in
 * dt_multi_destroy(). Returns NULL on a NULL params, invalid settings, or out
 * of memory.
 */
dt_multi_decoder *dt_multi_create(const dt_multi_params *params);

/* Free a multi-decoder and every stream decoder it owns. Passing NULL is fine.
 */
void dt_multi_destroy(dt_multi_decoder *m);

/*
 * Decode a received bit stream with all of the decoders in the multi struct at
 * once. For each output-bit position the decoders' traced bits are combined by
 * likelihood weight - each code's weight falls off with how poorly it fits the
 * stream - and the heavier bit value is emitted when it leads the other by
 * lock_margin of the total weight. When no code is locked (none reaches
 * lock_floor) or the weighted vote is too close, that bit is DT_ERASURE.
 *
 * `in`/`n_in`, `out`/`max_out`, and the return value follow dt_stream_decode.
 *
 * `details`, if supplied, is an array of length `codes_len * max_out`.
 * For each position i and decoder index j, the details of that decoder
 * are found at
 *
 * details[codes_len * i + j]
 *
 * `out` and `details` may be NULL.
 */
int dt_multi_decode(dt_multi_decoder *d, const uint8_t *in, int n_in,
                    uint8_t *out, dt_decode_details *details, int max_out);

/*
 * Flush a multi-decoder at end of stream, draining the last bits still in
 * flight in every wrapped decoder. Like dt_stream_decode_flush: call repeatedly
 * until it returns 0, after which every bit has been decoded.
 */
int dt_multi_decode_flush(dt_multi_decoder *d, uint8_t *out,
                          dt_decode_details *details, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_HYBRID_MULTI_DECODE_H */
