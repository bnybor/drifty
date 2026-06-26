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

#ifndef DRIFTY_HYBRID_H
#define DRIFTY_HYBRID_H

#include <drifty/ccode.h>
#include <drifty/decoder.h>
#include <drifty/encoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * Decoder channel-model settings for dt_hybrid_decoder_create() (and the
 * lower-level dt_stream_decoder_create()). Use designated initializers; any
 * field you leave out is 0.
 *
 *   decision_depth : output delay, in bits, before each bit is committed. Bigger
 *                    is more reliable but slower to emit. Try ~6 * dt_ccode_k().
 *                    Required (must be >= 1).
 *   p_flip         : how often a coded bit is flipped, 0 < p_flip < 1 (e.g.
 *                    0.01 for 1%). Required.
 *   max_drift      : how far alignment may slip from inserted/dropped bits before
 *                    the decoder loses track. 0 (the default) corrects flipped
 *                    bits only; 4-8 also recovers from insertions and deletions.
 *   p_ins_true,
 *   p_ins_false,
 *   p_ins_erase    : how often a spurious DT_TRUE / DT_FALSE / DT_ERASURE bit is
 *                    inserted into the stream, per bit and at any position. Their
 *                    sum is the overall insertion rate; it sets how readily the
 *                    decoder realigns, while the split only biases which value it
 *                    expects an inserted bit to carry. So an even true/false split
 *                    behaves the same as one combined rate - set them unequal only
 *                    to favour one inserted value.
 *   p_del          : how often a coded bit is dropped, per bit and at any position.
 *                    The insertion rates and p_del together must sum to < 1, and
 *                    are required when max_drift > 0; leave 0 otherwise.
 *   p_ovr_true,
 *   p_ovr_false,
 *   p_ovr_erase    : how often a coded bit is overwritten with a fixed DT_TRUE /
 *                    DT_FALSE / DT_ERASURE, regardless of what was sent. The three
 *                    must sum to < 1 (the remainder is the chance the bit arrives
 *                    normally). All 0 (the default) if there are no overwrites;
 *                    p_ovr_erase doubles as the plain erasure rate.
 *
 * Rough probabilities are fine; only their relative sizes matter.
 */
/* clang-format on */
typedef struct {
  int decision_depth;
  int max_drift;
  double p_flip;
  double p_ins_true;
  double p_ins_false;
  double p_ins_erase;
  double p_del;
  double p_ovr_true;
  double p_ovr_false;
  double p_ovr_erase;
} dt_hybrid_stream_params;

dt_encoder *dt_hybrid_encoder_create(const dt_ccode *code);
void dt_hybrid_encoder_destroy(dt_encoder *enc);

dt_decoder *dt_hybrid_decoder_create(const dt_ccode *code,
                                     const dt_hybrid_stream_params *params);
void dt_hybrid_decoder_destroy(dt_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_HYBRID_H */
