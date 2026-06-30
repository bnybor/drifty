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

#ifndef DRIFTY_CC_DETECT_NOISY_H
#define DRIFTY_CC_DETECT_NOISY_H

#include <drifty/stream_soft_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The detect_noisy codec - the noisy-channel one of drifty's two blind
 * code-presence detectors.
 * It blindly detects whether a convolutional code is present in an arbitrary bit
 * stream, with no prior knowledge or coordination (no code, rate, generators, or
 * alignment known).
 *
 * detect_noisy is the NOISE-tolerant variant: where detect_clean needs near-exact
 * parity (it holds only to ~1% flips), detect_noisy scores BIASED parity checks via
 * a fast Walsh-Hadamard transform, which degrades gracefully with noise. It
 * tolerates flips (to ~5%, marginally to ~8%), indels (to ~2-3%), and light-moderate
 * COMBINATIONS of the two - at the cost of a ~64 KB transform histogram and somewhat
 * more compute (a heavier per-window transform, only partly offset by its coarser
 * sliding step - ~1.5-2x slower than detect_clean in measurement). For a clean /
 * very-low-noise
 * channel where footprint matters (a few KB, no transform), prefer detect_clean
 * (<drifty/cc/detect_clean.h>). Same API and output as this one.
 *
 * It is SOFT-OUTPUT ONLY and standalone: unlike the other cc decoders it is not
 * built over a dt_cc_code. It does not recover bit values; it reports, per stream
 * position, two confidences on the output dt_soft_bit (each in [0, 1], and they
 * need NOT sum to 1):
 *
 *   c_erasure = consistency with "a convolutional code IS present"
 *   c_absent  = consistency with "no code is present / the stream is random"
 *
 * These are two INDEPENDENT goodness-of-fit reads, not a probability split: each
 * answers "does the data fail to contradict this hypothesis?", so they need not
 * sum to 1. Both near 1 means no discriminating evidence - an all-erasure (or
 * otherwise all-non-bit) run, or the warm-up/flush tail - since nothing observed
 * contradicts either hypothesis. (high, low) reads as a code, (low, high) as
 * random; (low, low) is not reachable from the single bias statistic and is
 * reserved for a future catalogue-mismatch signal.
 *
 * All other dt_soft_bit fields are 0. (The coded-presence confidence rides in
 * c_erasure by the engine-c_lost -> soft-c_erasure convention; detect repurposes
 * that field.) See doc/cc/detect_noisy.md for the method (FWHT parity-check bias)
 * and its limits.
 *
 * Build a soft decoder with the factory below, drive it through its vtable (see
 * <drifty/stream_soft_decoder.h>), and free it with the matching _destroy().
 */

/* clang-format off */
/*
 * Channel model for dt_cc_detect_noisy_soft_decoder_create() - the same rich field set
 * as dt_cc_hybrid_stream_params / dt_cc_maxir_stream_params, so a channel you
 * already describe for an inner codec can be handed to detect unchanged. Use
 * designated initializers; any field left out is 0.
 *
 * detect_noisy's bias method tolerates flips, so flips are NOT what damages it; the
 * flip rates instead calibrate how strongly a no-peak window (no structure found)
 * is allowed to rule a code OUT: the more flip noise you tell detect_noisy to
 * expect, the more a random-looking window could still be a real code whose parity
 * bias the flips eroded into the floor (the bias decays as (1 - 2*p_flip)^w), so
 * c_erasure (consistency with a code) stays elevated rather than collapsing to 0.
 * The flip rates damp that ruling-out on the PRESENT axis (c_erasure); they do NOT
 * scale c_absent, which is a positive fit to the random model - the absence of a
 * bias peak is consistent with random whatever the channel, and an observed peak
 * contradicts random regardless of noise (noise only erodes bias, never
 * manufactures it). Indels are likewise TOLERATED (rows after a slip merely stop
 * contributing bias), so p_ins/p_del affect neither axis.
 *
 *   p_flip                            : how often a coded bit is flipped, 0 <=
 *                                       p_flip < 1. 0 means "expect a clean
 *                                       channel" (unlike hybrid/maxir, which
 *                                       require p_flip > 0). Keeps c_erasure up on
 *                                       no-peak windows.
 *   p_ovr_true / p_ovr_false /        : how often a coded bit is overwritten with
 *   p_ovr_erase                         a fixed value / erasure (the three sum to
 *                                       < 1). Flip-like; keep c_erasure up.
 *   p_ins_true / p_ins_false /        : insertion rates, and p_del the deletion
 *   p_ins_erase, p_del                  rate (their sum < 1) - the drift detect is
 *                                       built to tolerate. They affect neither
 *                                       axis (the sliding windows recover from
 *                                       indels by finding clean runs).
 *   decision_depth (>= 1), max_drift  : accepted for interface uniformity with the
 *   (>= 0)                              cc family but NOT used by the bias method
 *                                       (detect has its own window-based delay and
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
} dt_cc_detect_noisy_stream_params;

/* Build a soft-output detect decoder with the channel model in `params` (copied;
 * need not outlive the call). Returns NULL on a bad argument (including an invalid
 * `params`) or out of memory. */
dt_stream_soft_decoder *dt_cc_detect_noisy_soft_decoder_create(
    const dt_cc_detect_noisy_stream_params *params);
/* Free a soft decoder from dt_cc_detect_noisy_soft_decoder_create(). NULL is fine. */
void dt_cc_detect_noisy_soft_decoder_destroy(dt_stream_soft_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_DETECT_NOISY_H */
