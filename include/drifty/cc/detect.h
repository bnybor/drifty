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

#ifndef DRIFTY_CC_DETECT_H
#define DRIFTY_CC_DETECT_H

#include <drifty/stream_soft_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The detect codec - a META-codec that blindly detects the presence of a
 * convolutional code in an arbitrary bit stream, with no prior knowledge or
 * coordination (no code, rate, generators, or alignment known).
 *
 * It is SOFT-OUTPUT ONLY and standalone: unlike the other cc decoders it is not
 * built over a dt_cc_code and takes no channel-model parameters - the factory
 * takes nothing at all. It does not recover bit values; it reports, per stream
 * position, two confidences on the output dt_soft_bit (each in [0, 1], and they
 * need NOT sum to 1):
 *
 *   c_erasure = confidence a convolutional code IS encoded onto the stream
 *   c_absent  = confidence a convolutional code is NOT encoded onto the stream
 *
 * All other dt_soft_bit fields are 0. (The coded-presence confidence rides in
 * c_erasure by the engine-c_lost -> soft-c_erasure convention; detect repurposes
 * that field.) Detection works in the clean / very-low-noise regime; see
 * doc/cc/detect.md for the method (GF(2) rank deficiency) and its limits.
 *
 * Build a soft decoder with the factory below, drive it through its vtable (see
 * <drifty/stream_soft_decoder.h>), and free it with the matching _destroy().
 */

/* Build a soft-output detect decoder. Takes no parameters. Returns NULL on out of
 * memory. */
dt_stream_soft_decoder *dt_cc_detect_soft_decoder_create(void);
/* Free a soft decoder from dt_cc_detect_soft_decoder_create(). NULL is fine. */
void dt_cc_detect_soft_decoder_destroy(dt_stream_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_DETECT_H */
