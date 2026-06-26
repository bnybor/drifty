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

/*
 * MAXIR encoder: the ordinary convolutional encoder (the same one viterbi and
 * vindel use). MAXIR differs only in how it decodes; the transmitted codeword is
 * the plain convolutional code over the shared dt_ccode trellis tables.
 */

#include "encode.h"

#include "../ccode_internal.h"

/* ------------------------------------------------------------------------- */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------- */

/* The encoder carries two independent "no clean value" registers, each with the
 * same K-1-bit shift geometry as *state, packed into the single *unknown word:
 * the erasure register in the low DT_ENC_MASK_BITS bits, the invalid register
 * just above it. K <= 9 keeps each register <= 8 bits, so both fit in 16 bits. */
#define DT_ENC_MASK_BITS 8u
#define DT_ENC_MASK ((1u << DT_ENC_MASK_BITS) - 1u)

/* Encode one input bit's coded group from the running registers and advance
 * them. `bit` is the 0/1 value driving the trellis. A non-boolean input carries
 * no clean value in one of two distinct ways, tracked as two registers:
 *   in_erasure - the input is unbound (DT_ERASURE). A coded bit whose generator
 *     taps such a position has an unbound parity and is emitted DT_ERASURE: a
 *     value deferred to the channel, which may concretize it (encoder output is
 *     not decoder input). Linearity confines the unbound bit's whole influence
 *     to exactly these taps, so deferral is unbiased.
 *   in_invalid - the input is bound but non-boolean (DT_INVALID): structural
 *     poison. A coded bit tapping it is emitted DT_INVALID, which the decoder
 *     reads as a true tie (see decode.c). Poison cannot be concretized, so it
 *     dominates: per output bit the precedence is INVALID > ERASURE > clean.
 * `bit` (a definite 0 for a non-boolean input) only keeps the trellis advancing;
 * it never reaches a clean output, since exactly the taps that would carry it are
 * marked instead. Returns the number of bits written (the code's n). */
static int emit_group(const dt_ccode *code, int *state, unsigned int *unknown,
                      int bit, int in_erasure, int in_invalid, uint8_t *out) {
  const unsigned int era_carried = *unknown & DT_ENC_MASK;
  const unsigned int inv_carried = (*unknown >> DT_ENC_MASK_BITS) & DT_ENC_MASK;
  /* Full K-bit registers: the new input at the top (input_tap == 1 << (K-1))
   * over the carried-forward positions, one register per kind of unknown. */
  const unsigned int era_reg = (in_erasure ? code->input_tap : 0u) | era_carried;
  const unsigned int inv_reg = (in_invalid ? code->input_tap : 0u) | inv_carried;
  const uint8_t *group = &code->output[((size_t)(*state * 2 + bit)) * code->n];
  for (int j = 0; j < code->n; ++j) {
    if (code->generators[j] & inv_reg) {
      /* Taps an invalid position: poison, not a clean parity, not concretizable. */
      out[j] = DT_INVALID;
    } else if (code->generators[j] & era_reg) {
      /* Taps an unbound position: parity is unbound, deferred to the channel. */
      out[j] = DT_ERASURE;
    } else {
      /* code->output holds the codeword as raw 0/1; emit it as bit symbols. */
      out[j] = group[j] ? DT_TRUE : DT_FALSE;
    }
  }
  /* Advance state and both registers in lockstep (same shift geometry). */
  const unsigned int state_mask = (unsigned int)(code->n_states - 1);
  const unsigned int top_bit = 1u << (code->K - 2);
  *state = code->next_state[*state * 2 + bit];
  const unsigned int era_next =
      ((era_carried >> 1) | (in_erasure ? top_bit : 0u)) & state_mask;
  const unsigned int inv_next =
      ((inv_carried >> 1) | (in_invalid ? top_bit : 0u)) & state_mask;
  *unknown = (inv_next << DT_ENC_MASK_BITS) | era_next;
  return code->n;
}

int dt_maxir_encode(const dt_ccode *code, const uint8_t *bits, int n_bits,
                   int *state, unsigned int *unknown, uint8_t *out) {
  if (!code || !state || !unknown || n_bits < 0 || (n_bits > 0 && !bits) ||
      !out) {
    return DT_ERR_ARG;
  }
  if (*state < 0 || *state >= code->n_states) {
    return DT_ERR_ARG;
  }

  int current_state = *state, written = 0;
  unsigned int current_unknown = *unknown;
  for (int i = 0; i < n_bits; ++i) {
    /* Two distinct non-boolean inputs. DT_INVALID is bound but not a boolean -
     * structural poison. DT_ERASURE is unbound - a value deferred to the channel.
     * DT_BIT supplies a definite 0 only to keep the trellis advancing; for either
     * non-boolean input it never reaches a clean output. */
    const int in_bit = DT_IS_BIT(bits[i]);
    const int in_invalid = !in_bit && DT_IS_BOUND(bits[i]);
    const int in_erasure = !in_bit && !DT_IS_BOUND(bits[i]);
    written += emit_group(code, &current_state, &current_unknown,
                          DT_BIT(bits[i]), in_erasure, in_invalid, out + written);
  }
  *state = current_state;
  *unknown = current_unknown;
  return written;
}

int dt_maxir_encode_flush(const dt_ccode *code, int *state, unsigned int *unknown,
                         uint8_t *out) {
  if (!code || !state || !unknown || !out) {
    return DT_ERR_ARG;
  }
  if (*state < 0 || *state >= code->n_states) {
    return DT_ERR_ARG;
  }

  /* Feed K-1 known zero bits, which shift the state register back to 0 and clear
   * any unknown positions still in flight. */
  int current_state = *state, written = 0;
  unsigned int current_unknown = *unknown;
  for (int i = 0; i < code->K - 1; ++i) {
    written += emit_group(code, &current_state, &current_unknown,
                          DT_BIT(DT_FALSE), 0, 0, out + written);
  }
  *state = current_state;
  *unknown = current_unknown;
  return written;
}
