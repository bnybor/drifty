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

#ifndef DRIFTY_VINDEL_H
#define DRIFTY_VINDEL_H

#include <drifty/ccode.h>
#include <drifty/decoder.h>
#include <drifty/encoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The vindel codec - drifty's convolutional, drift-tolerant implementation of
 * the encoder / decoder interfaces. This is the single header to include for
 * the whole public API.
 *
 * Build a codec object over a dt_ccode with one of the factories below, drive
 * it through its vtable (see encoder.h / decoder.h), and free it with the
 * matching _destroy(). The code must outlive everything built from it.
 */

/* clang-format off */
/*
 * Decoder channel-model settings for dt_vindel_decoder_create(). Use designated
 * initializers; any field you leave out is 0.
 *
 *   decision_depth : output delay, in bits, before each bit is committed. Bigger
 *                    is more reliable but slower to emit. Try ~6 * dt_ccode_k().
 *                    Required (must be >= 1).
 *   p_sub          : how often a received bit is flipped, 0 < p_sub < 1 (e.g.
 *                    0.01 for 1%). Required.
 *   max_drift      : how far alignment may slip from inserted/dropped bits before
 *                    the decoder loses track. 0 (the default) corrects flipped
 *                    bits only; 4-8 also recovers from insertions and deletions.
 *   p_ins, p_del   : how often a coded bit is inserted / dropped, per bit and at
 *                    any position (p_ins + p_del < 1). Required when
 *                    max_drift > 0; leave 0 otherwise.
 *   p_erase        : how often a received bit is DT_ERASURE. 0 (the default) if
 *                    you never mark erasures.
 *
 * Rough probabilities are fine; only their relative sizes matter.
 */
/* clang-format on */
typedef struct {
  int decision_depth;
  int max_drift;
  double p_sub;
  double p_ins;
  double p_del;
  double p_erase;
} dt_vindel_stream_params;

/* Build an encoder over `code`. Returns NULL on a bad argument or out of
 * memory. */
dt_encoder *dt_vindel_encoder_create(const dt_ccode *code);
/* Free an encoder from dt_vindel_encoder_create(). Passing NULL is fine. */
void dt_vindel_encoder_destroy(dt_encoder *enc);

/* Build a hard-decision decoder over `code` with the channel model in `params`.
 * `params` is copied and need not outlive the call. Returns NULL on a bad
 * argument (including an invalid `params`) or out of memory. */
dt_decoder *dt_vindel_decoder_create(const dt_ccode *code,
                                     const dt_vindel_stream_params *params);
/* Free a decoder from dt_vindel_decoder_create(). Passing NULL is fine. */
void dt_vindel_decoder_destroy(dt_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_VINDEL_H */
