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

/* Encode one input bit's coded group from the running registers and advance
 * them. `bit` is the 0/1 value used to drive the trellis; `in_unknown` is 1 when
 * the input was non-boolean, so its value is unknown. *unknown tracks which
 * shift-register positions currently hold such an unknown bit (the same K-1-bit
 * geometry as *state). An output bit is the encoder's DT_INVALID poison marker
 * iff its generator taps any unknown position - i.e. it would carry the unknown
 * value - so a non-boolean input poisons exactly the coded bits that carry it and
 * the MAXIR decoder reads its originating step as a true tie (see decode.c).
 * Returns the number of bits written (the code's n). */
static int emit_group(const dt_ccode *code, int *state, unsigned int *unknown,
                      int bit, int in_unknown, uint8_t *out) {
  /* Full K-bit register's unknown mask: the new input at the top (input_tap ==
   * 1 << (K-1)) over the carried-forward state positions. */
  const unsigned int reg_unknown =
      (in_unknown ? code->input_tap : 0u) | *unknown;
  const uint8_t *group = &code->output[((size_t)(*state * 2 + bit)) * code->n];
  for (int j = 0; j < code->n; ++j) {
    if (code->generators[j] & reg_unknown) {
      /* Generator taps an unknown bit: there is no clean parity to emit. */
      out[j] = DT_INVALID;
    } else {
      /* code->output holds the codeword as raw 0/1; emit it as bit symbols. */
      out[j] = group[j] ? DT_TRUE : DT_FALSE;
    }
  }
  /* Advance state and the unknown mask in lockstep (same shift geometry). */
  const unsigned int state_mask = (unsigned int)(code->n_states - 1);
  *state = code->next_state[*state * 2 + bit];
  *unknown = ((*unknown >> 1) |
              (in_unknown ? (1u << (code->K - 2)) : 0u)) & state_mask;
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
    /* A non-boolean input (DT_ERASURE / DT_INVALID) has no recoverable value:
     * poison its coded bits. DT_BIT just supplies a definite bit (0) to keep the
     * trellis advancing; it never reaches a clean output. */
    const int in_unknown = !DT_IS_BIT(bits[i]);
    written += emit_group(code, &current_state, &current_unknown,
                          DT_BIT(bits[i]), in_unknown, out + written);
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
                          DT_BIT(DT_FALSE), 0, out + written);
  }
  *state = current_state;
  *unknown = current_unknown;
  return written;
}
