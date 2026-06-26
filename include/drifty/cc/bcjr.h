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

#ifndef DRIFTY_BCJR_H
#define DRIFTY_BCJR_H

#include <drifty/cc/ccode.h>
#include <drifty/decoder.h>
#include <drifty/encoder.h>
#include <drifty/soft_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The bcjr codec - a convolutional encoder and BCJR (MAP / forward-backward)
 * decoder over a dt_ccode. Where the viterbi codec finds the single most likely
 * path, BCJR computes the per-bit a-posteriori probability of each input bit,
 * which makes it a natural soft-output decoder. Like viterbi it corrects flipped
 * and erased bits and does not track drift (inserted or dropped bits - for that
 * use the vindel or hybrid codecs). This is the single header to include for its
 * public API.
 *
 * Build a codec object over a dt_ccode with one of the factories below, drive it
 * through its vtable (see encoder.h / decoder.h / soft_decoder.h), and free it
 * with the matching _destroy(). The code must outlive everything built from it.
 *
 * NOTE: the encoder is the standard convolutional encoder and is fully
 * implemented; the hard and soft decoders are stubs (the vtable plumbing is
 * wired and the channel model validated, but no bits come out until the
 * forward-backward algorithm lands - see src/cc/bcjr/decode.c).
 */

/* clang-format off */
/*
 * Decoder channel-model settings for dt_bcjr_decoder_create() and
 * dt_bcjr_soft_decoder_create(). Use designated initializers; any field you
 * leave out is 0.
 *
 *   decision_depth : output delay, in bits, before each bit is committed - the
 *                    sliding window the backward recursion spans. Bigger is more
 *                    reliable but slower to emit. Try ~6 * dt_ccode_k().
 *                    Required (must be >= 1).
 *   p_flip         : how often a coded bit is flipped, 0 < p_flip < 1 (e.g.
 *                    0.01 for 1%). Sets the branch likelihoods. Required.
 *   p_erase        : how often a received bit is DT_ERASURE. 0 (the default) if
 *                    you never mark erasures.
 *
 * Rough probabilities are fine; only their relative sizes matter.
 */
/* clang-format on */
typedef struct {
  int decision_depth;
  float p_flip;
  float p_erase;
} dt_bcjr_stream_params;

/* Build an encoder over `code`. Returns NULL on a bad argument or out of
 * memory. */
dt_encoder *dt_bcjr_encoder_create(const dt_ccode *code);
/* Free an encoder from dt_bcjr_encoder_create(). Passing NULL is fine. */
void dt_bcjr_encoder_destroy(dt_encoder *enc);

/* Build a hard-decision BCJR decoder over `code` with the channel model in
 * `params`. `params` is copied and need not outlive the call. Returns NULL on a
 * bad argument (including an invalid `params`) or out of memory. */
dt_decoder *dt_bcjr_decoder_create(const dt_ccode *code,
                                   const dt_bcjr_stream_params *params);
/* Free a decoder from dt_bcjr_decoder_create(). Passing NULL is fine. */
void dt_bcjr_decoder_destroy(dt_decoder *dec);

/* Build a soft-output BCJR decoder - same inputs as dt_bcjr_decoder_create(),
 * but it reports per-bit consistencies instead of a hard decision. Returns NULL
 * on a bad argument or out of memory. */
dt_soft_decoder *dt_bcjr_soft_decoder_create(
    const dt_ccode *code, const dt_bcjr_stream_params *params);
/* Free a soft decoder from dt_bcjr_soft_decoder_create(). NULL is fine. */
void dt_bcjr_soft_decoder_destroy(dt_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_BCJR_H */
