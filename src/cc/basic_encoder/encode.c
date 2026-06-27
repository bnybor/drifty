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

#include "encode.h"

#include "../ccode_internal.h"

/* ------------------------------------------------------------------------- */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------- */

/* Encode one input bit `bit` from state *state: copy the code's n output bits
 * to `out`, advance *state, and return the number of bits written (the code's
 * n). */
static int emit_group(const dt_ccode *code, int *state, int bit, uint8_t *out) {
  const uint8_t *group = &code->output[((size_t)(*state * 2 + bit)) * code->n];
  for (int j = 0; j < code->n; ++j) {
    /* code->output holds the codeword as raw 0/1; emit it as bit symbols. */
    out[j] = group[j] ? DT_TRUE : DT_FALSE;
  }
  *state = code->next_state[*state * 2 + bit];
  return code->n;
}

int dt_basic_encode(const dt_ccode *code, const uint8_t *bits, int n_bits,
                    int *state, uint8_t *out) {
  if (!code || !state || n_bits < 0 || (n_bits > 0 && !bits) || !out) {
    return DT_ERR_ARG;
  }
  if (*state < 0 || *state >= code->n_states) {
    return DT_ERR_ARG;
  }

  int current_state = *state, written = 0;
  for (int i = 0; i < n_bits; ++i) {
    written += emit_group(code, &current_state, DT_BIT(bits[i]), out + written);
  }
  *state = current_state;
  return written;
}

int dt_basic_encode_flush(const dt_ccode *code, int *state, uint8_t *out) {
  if (!code || !state || !out) {
    return DT_ERR_ARG;
  }
  if (*state < 0 || *state >= code->n_states) {
    return DT_ERR_ARG;
  }

  /* Feed K-1 zero bits, which shift the state register back to 0. */
  int current_state = *state, written = 0;
  for (int i = 0; i < code->K - 1; ++i) {
    written += emit_group(code, &current_state, DT_BIT(DT_FALSE), out + written);
  }
  *state = current_state;
  return written;
}
