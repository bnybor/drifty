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
 * The dt_ccode code type: building the convolutional code (its precomputed
 * trellis tables) and the accessors for it. This is the public ccode.h API,
 * shared by every codec built over a code (encode.c and decode.c consume the
 * tables this file fills in).
 */

#include <drifty/ccode.h>

#include <drifty/stdlib.h>

#include "ccode_internal.h"

/* Parity (XOR of all bits) of `bits`: 1 if it has an odd number of set bits. */
static int parity(unsigned int bits) {
  /* Fold down to a single parity bit. */
  bits ^= bits >> 16;
  bits ^= bits >> 8;
  bits ^= bits >> 4;
  bits ^= bits >> 2;
  bits ^= bits >> 1;
  return (int)(bits & 1u);
}

dt_ccode *dt_ccode_create(int K, const unsigned int *generators,
                        int num_generators) {
  /* K is documented as 2..9 (ccode.h). The upper bound also keeps 1 << (K-1)
   * well clear of int-shift UB and the trellis a sane size. */
  if (!generators || K < 2 || K > 9 || num_generators < 1) {
    return NULL;
  }

  dt_ccode *code = dt_calloc(1, sizeof(*code));
  if (!code) {
    return NULL;
  }
  code->K = K;
  code->n = num_generators;
  code->n_states = 1 << (K - 1);
  code->input_tap = 1u << (K - 1);

  code->generators = dt_malloc((size_t)code->n * sizeof(unsigned int));
  code->next_state = dt_malloc((size_t)code->n_states * 2 * sizeof(int));
  code->output = dt_malloc((size_t)code->n_states * 2 * code->n * sizeof(uint8_t));
  if (!code->generators || !code->next_state || !code->output) {
    dt_ccode_destroy(code);
    return NULL;
  }

  for (int generator_index = 0; generator_index < code->n; ++generator_index) {
    code->generators[generator_index] = generators[generator_index];
  }

  /* Precompute the trellis: (state, input bit) -> (next_state, output). */
  for (int state = 0; state < code->n_states; ++state) {
    for (int bit = 0; bit <= 1; ++bit) {
      unsigned int shift_register =
          ((unsigned int)bit << (K - 1)) | (unsigned int)state;
      uint8_t *out = &code->output[((size_t)(state * 2 + bit)) * code->n];
      for (int j = 0; j < code->n; ++j) {
        out[j] = (uint8_t)parity(shift_register & code->generators[j]);
      }
      int next_state = ((state >> 1) | (bit << (K - 2))) & (code->n_states - 1);
      code->next_state[state * 2 + bit] = next_state;
    }
  }

  return code;
}

dt_ccode *dt_ccode_create_standard(dt_standard_code which) {
  switch (which) {
    /* These generator sets and the d_free values in ccode.h are produced by
     * bench/dt_codesearch, which selects, per family, the codes that are
     * mutually distinguishable under the decoder's lock metric (rate-1/2 tops
     * out at three such codes; the wider rates reach five). */
    case DT_CODE_K3_RATE_1_2: {
      static const unsigned int generators[] = {005, 007};
      return dt_ccode_create(3, generators, 2);
    }
    case DT_CODE_K3_RATE_1_2_ALT1: {
      static const unsigned int generators[] = {001, 007};
      return dt_ccode_create(3, generators, 2);
    }
    case DT_CODE_K3_RATE_1_2_ALT2: {
      static const unsigned int generators[] = {003, 007};
      return dt_ccode_create(3, generators, 2);
    }
    case DT_CODE_K7_RATE_1_2: {
      static const unsigned int generators[] = {0171, 0133};
      return dt_ccode_create(7, generators, 2);
    }
    case DT_CODE_K7_RATE_1_2_ALT1: {
      static const unsigned int generators[] = {0043, 0175};
      return dt_ccode_create(7, generators, 2);
    }
    case DT_CODE_K7_RATE_1_2_ALT2: {
      static const unsigned int generators[] = {0107, 0156};
      return dt_ccode_create(7, generators, 2);
    }
    case DT_CODE_K7_RATE_1_3: {
      static const unsigned int generators[] = {0113, 0135, 0157};
      return dt_ccode_create(7, generators, 3);
    }
    case DT_CODE_K7_RATE_1_3_ALT1: {
      static const unsigned int generators[] = {0112, 0153, 0157};
      return dt_ccode_create(7, generators, 3);
    }
    case DT_CODE_K7_RATE_1_3_ALT2: {
      static const unsigned int generators[] = {0037, 0135, 0153};
      return dt_ccode_create(7, generators, 3);
    }
    case DT_CODE_K7_RATE_1_3_ALT3: {
      static const unsigned int generators[] = {0012, 0145, 0177};
      return dt_ccode_create(7, generators, 3);
    }
    case DT_CODE_K7_RATE_1_3_ALT4: {
      static const unsigned int generators[] = {0042, 0133, 0172};
      return dt_ccode_create(7, generators, 3);
    }
    case DT_CODE_K5_RATE_1_5: {
      static const unsigned int generators[] = {025, 027, 033, 035, 037};
      return dt_ccode_create(5, generators, 5);
    }
    case DT_CODE_K5_RATE_1_5_ALT1: {
      static const unsigned int generators[] = {007, 017, 025, 027, 035};
      return dt_ccode_create(5, generators, 5);
    }
    case DT_CODE_K5_RATE_1_5_ALT2: {
      static const unsigned int generators[] = {011, 032, 033, 035, 037};
      return dt_ccode_create(5, generators, 5);
    }
    case DT_CODE_K5_RATE_1_5_ALT3: {
      static const unsigned int generators[] = {013, 021, 023, 033, 037};
      return dt_ccode_create(5, generators, 5);
    }
    case DT_CODE_K5_RATE_1_5_ALT4: {
      static const unsigned int generators[] = {013, 024, 032, 033, 037};
      return dt_ccode_create(5, generators, 5);
    }
  }
  return NULL;
}

void dt_ccode_destroy(dt_ccode *code) {
  if (!code) {
    return;
  }
  dt_free(code->generators);
  dt_free(code->next_state);
  dt_free(code->output);
  dt_free(code);
}

int dt_ccode_n(const dt_ccode *code) { return code ? code->n : -1; }

int dt_ccode_k(const dt_ccode *code) { return code ? code->K : -1; }
