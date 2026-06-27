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

#ifndef DRIFTY_CC_MAXIR_H
#define DRIFTY_CC_MAXIR_H

#include <drifty/cc/ccode.h>
#include <drifty/decoder.h>
#include <drifty/soft_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The maxir codec - a convolutional encoder and MAXIR (max-log-MAP /
 * forward-backward) decoder over a dt_cc_code. Where the viterbi codec finds the
 * single most likely path, MAXIR computes the per-bit a-posteriori weight of each
 * input bit, which makes it a natural soft-output decoder. It corrects flipped
 * and erased bits, tracks drift (inserted or dropped bits) like the vindel and
 * hybrid codecs, and re-acquires sync after a sustained loss of lock. This is the
 * single header to include for its public API.
 *
 * Build a decoder over a dt_cc_code with one of the factories below, drive it
 * through its vtable (see decoder.h / soft_decoder.h), and free it with the
 * matching _destroy(). The code must outlive everything built from it. To
 * encode, use the standalone encoder in <drifty/cc/encoders.h>.
 */

/* clang-format off */
/*
 * Decoder channel-model settings for dt_cc_maxir_decoder_create() and
 * dt_cc_maxir_soft_decoder_create(). Use designated initializers; any field you
 * leave out is 0.
 *
 *   decision_depth : output delay, in bits, before each bit is committed - the
 *                    sliding window the backward recursion spans. Bigger is more
 *                    reliable but slower to emit. Try ~6 * dt_cc_code_k().
 *                    Required (must be >= 1).
 *   max_drift      : how far alignment may slip from inserted/dropped bits before
 *                    the decoder loses track. 0 (the default) corrects flipped
 *                    bits only; 4-8 also recovers from insertions and deletions.
 *   p_flip         : how often a coded bit is flipped, 0 < p_flip < 1 (e.g.
 *                    0.01 for 1%). Required.
 *   p_ins_true,
 *   p_ins_false,
 *   p_ins_erase    : how often a spurious DT_TRUE / DT_FALSE / DT_ERASURE bit is
 *                    inserted into the stream, per bit and at any position. Their
 *                    sum is the overall insertion rate; it sets how readily the
 *                    decoder realigns, while the split only biases which value it
 *                    expects an inserted bit to carry. Required when max_drift > 0.
 *   p_del          : how often a coded bit is dropped, per bit and at any position.
 *                    The insertion rates and p_del together must sum to < 1, and
 *                    are required when max_drift > 0; leave 0 otherwise.
 *   p_ovr_true,
 *   p_ovr_false,
 *   p_ovr_erase    : how often a coded bit is overwritten with a fixed DT_TRUE /
 *                    DT_FALSE / DT_ERASURE, regardless of what was sent. The three
 *                    must sum to < 1. All 0 (the default) if there are no
 *                    overwrites; p_ovr_erase doubles as the plain erasure rate.
 *
 * Rough probabilities are fine; only their relative sizes matter.
 */
/* clang-format on */
typedef struct {
  int decision_depth;
  int max_drift;
  float p_flip;
  float p_ins_true;
  float p_ins_false;
  float p_ins_erase;
  float p_del;
  float p_ovr_true;
  float p_ovr_false;
  float p_ovr_erase;
} dt_cc_maxir_stream_params;

/* Build a hard-decision MAXIR decoder over `code` with the channel model in
 * `params`. `params` is copied and need not outlive the call. Returns NULL on a
 * bad argument (including an invalid `params`) or out of memory. */
dt_decoder *dt_cc_maxir_decoder_create(const dt_cc_code *code,
                                   const dt_cc_maxir_stream_params *params);
/* Free a decoder from dt_cc_maxir_decoder_create(). Passing NULL is fine. */
void dt_cc_maxir_decoder_destroy(dt_decoder *dec);

/* Build a soft-output MAXIR decoder - same inputs as dt_cc_maxir_decoder_create(),
 * but it reports per-bit consistencies instead of a hard decision. Returns NULL
 * on a bad argument or out of memory. */
dt_soft_decoder *dt_cc_maxir_soft_decoder_create(
    const dt_cc_code *code, const dt_cc_maxir_stream_params *params);
/* Free a soft decoder from dt_cc_maxir_soft_decoder_create(). NULL is fine. */
void dt_cc_maxir_soft_decoder_destroy(dt_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_MAXIR_H */
