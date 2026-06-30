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

#ifndef DRIFTY_CC_DETECT_LEAN_H
#define DRIFTY_CC_DETECT_LEAN_H

#include <drifty/stream_soft_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The detect_lean codec - the LEAN of drifty's two blind code-presence detectors.
 * It blindly detects whether a convolutional code is present in an arbitrary bit
 * stream, with no prior knowledge or coordination (no code, rate, generators, or
 * alignment known).
 *
 * detect_lean is the cheap, embeddable variant: it uses exact GF(2) rank deficiency
 * (a few KB of state, no large tables), tolerates inserted/dropped bits and ~1%
 * flips, but no more. For a flip-tolerant detector (flips ~8%, indels, and
 * combinations, at a ~64 KB / heavier-compute cost) use detect_full
 * (<drifty/cc/detect_full.h>). Same API and output as this one.
 *
 * It is SOFT-OUTPUT ONLY and standalone: unlike the other cc decoders it is not
 * built over a dt_cc_code. It does not recover bit values; it reports, per stream
 * position, two confidences on the output dt_soft_bit (each in [0, 1], and they
 * need NOT sum to 1):
 *
 *   c_erasure = confidence a convolutional code IS encoded onto the stream
 *   c_absent  = confidence a convolutional code is NOT encoded onto the stream
 *
 * All other dt_soft_bit fields are 0. (The coded-presence confidence rides in
 * c_erasure by the engine-c_lost -> soft-c_erasure convention; detect repurposes
 * that field.) See doc/cc/detect_lean.md for the method (GF(2) rank deficiency)
 * and its limits.
 *
 * Build a soft decoder with the factory below, drive it through its vtable (see
 * <drifty/stream_soft_decoder.h>), and free it with the matching _destroy().
 */

/* clang-format off */
/*
 * Channel model for dt_cc_detect_lean_soft_decoder_create() - the same rich field set
 * as dt_cc_hybrid_stream_params / dt_cc_maxir_stream_params, so a channel you
 * already describe for an inner codec can be handed to detect unchanged. Use
 * designated initializers; any field left out is 0.
 *
 * detect's GF(2) rank method needs EXACT parity within a window to see a code,
 * which a FLIP breaks - so the flip rates calibrate how much a NULL result (no
 * structure found) can be trusted: the more flip noise you tell detect to expect,
 * the less a clean-looking stream can be confidently declared code-FREE (a code
 * could be present but hidden by the flips). It lowers c_absent accordingly; it
 * never inflates c_lost (found parity checks are real regardless of expected noise
 * - noise only destroys structure, never creates it). Indels are TOLERATED by the
 * sliding windows, so p_ins/p_del do not lower c_absent.
 *
 *   p_flip                            : how often a coded bit is flipped, 0 <=
 *                                       p_flip < 1. 0 means "expect a clean
 *                                       channel" (unlike hybrid/maxir, which
 *                                       require p_flip > 0). Damps c_absent.
 *   p_ovr_true / p_ovr_false /        : how often a coded bit is overwritten with
 *   p_ovr_erase                         a fixed value / erasure (the three sum to
 *                                       < 1). Flip-like; damp c_absent.
 *   p_ins_true / p_ins_false /        : insertion rates, and p_del the deletion
 *   p_ins_erase, p_del                  rate (their sum < 1) - the drift detect is
 *                                       built to tolerate. They do NOT damp
 *                                       c_absent (the sliding windows recover from
 *                                       indels by finding clean runs).
 *   decision_depth (>= 1), max_drift  : accepted for interface uniformity with the
 *   (>= 0)                              cc family but NOT used by the rank method
 *                                       (detect has its own block-based delay and
 *                                       does not track drift). Validated only.
 *
 * Rough probabilities are fine; only their rough magnitude matters.
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
} dt_cc_detect_lean_stream_params;

/* Build a soft-output detect decoder with the channel model in `params` (copied;
 * need not outlive the call). Returns NULL on a bad argument (including an invalid
 * `params`) or out of memory. */
dt_stream_soft_decoder *dt_cc_detect_lean_soft_decoder_create(
    const dt_cc_detect_lean_stream_params *params);
/* Free a soft decoder from dt_cc_detect_lean_soft_decoder_create(). NULL is fine. */
void dt_cc_detect_lean_soft_decoder_destroy(dt_stream_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_DETECT_LEAN_H */
