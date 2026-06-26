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
 * Multi-encoder: the sender's mirror of the multi-decoder. It holds a family of
 * codes that share a rate and constraint length and ONE encoder state shared
 * across them, and on each call encodes with a code chosen by index. The shared
 * state is well defined because a convolutional encoder's state transition is a
 * function of the constraint length and the input bits only, not the generator
 * polynomials (see emit_group/next_state in encode.c): a same-K family steps the
 * single state in lockstep, so switching the chosen index between calls changes
 * the code mid-stream without breaking continuity - the encode-side image of the
 * multi-decoder following a code change.
 */

#include "multi_encode.h"

#include "encode.h"
#include <drifty/stdlib.h>

struct dt_multi_encoder {
  const dt_ccode **codes; /* [n] borrowed; each must outlive the encoder */
  size_t n;
  int state; /* one shared shift-register state for all codes */
};

dt_multi_encoder *dt_multi_encode_create(const dt_multi_encode_params *params) {
  if (!params || (params->codes_len > 0 && !params->codes)) {
    return NULL;
  }
  dt_multi_encoder *e = dt_calloc(1, sizeof(*e));
  if (!e) {
    return NULL;
  }
  e->n = params->codes_len;

  if (params->codes_len > 0) {
    /* calloc so a build failure partway leaves the rest NULL and
     * dt_multi_encode_destroy can free what was built. */
    e->codes = dt_calloc(params->codes_len, sizeof(const dt_ccode *));
    if (!e->codes) {
      dt_multi_encode_destroy(e);
      return NULL;
    }
    for (size_t j = 0; j < params->codes_len; ++j) {
      /* Reject a NULL entry outright. The rate/length comparison below cannot
       * catch a NULL at index 0: dt_ccode_n/k(NULL) == -1, so codes[0] == NULL
       * compares equal to itself (-1 == -1) and an all-NULL set would slip
       * through. (The decode-side gate is spared this because dt_multi_create
       * runs dt_decode_ctx_init on codes[0] first, which rejects a NULL code.) */
      if (!params->codes[j]) {
        dt_multi_encode_destroy(e);
        return NULL;
      }
      /* One shared state drives every code, so all codes must agree on the
       * constraint length (dt_ccode_k) it steps and the rate (dt_ccode_n) each
       * step emits; otherwise the chosen code's stream would not be coherent
       * with the others'. Mirrors dt_multi_create's gate. */
      if (dt_ccode_n(params->codes[j]) != dt_ccode_n(params->codes[0]) ||
          dt_ccode_k(params->codes[j]) != dt_ccode_k(params->codes[0])) {
        dt_multi_encode_destroy(e);
        return NULL;
      }
      e->codes[j] = params->codes[j];
    }
  }
  return e;
}

void dt_multi_encode_destroy(dt_multi_encoder *e) {
  if (!e) {
    return;
  }
  dt_free(e->codes);
  dt_free(e);
}

/* Common argument check for the two public entry points; mirrors dt_ccode_encode's
 * contract plus the bounds-safety contract of dt_multi_decode (max_out). */
static int encode_args_ok(const dt_multi_encoder *e, int idx, const uint8_t *out,
                          int max_out) {
  return e && idx >= 0 && (size_t)idx < e->n && !(max_out > 0 && !out) &&
         max_out >= 0;
}

int dt_multi_encode(dt_multi_encoder *e, int idx, const uint8_t *bits, int n_bits,
                    uint8_t *out, int max_out) {
  if (!encode_args_ok(e, idx, out, max_out) || n_bits < 0 ||
      (n_bits > 0 && !bits)) {
    return DT_ERR_ARG;
  }
  /* dt_ccode_n >= 1 for any valid code (num_generators >= 1), and codes[idx] is
   * non-NULL after the create-time NULL gate, so this division is exactly
   * equivalent to n_bits * n > max_out but cannot overflow (n_bits * n would be
   * signed-overflow UB for large n_bits, wrapping negative and silently passing
   * the bound). */
  const int n = dt_ccode_n(e->codes[idx]);
  if (n_bits > max_out / n) {
    return DT_ERR_ARG;
  }
  return dt_ccode_encode(e->codes[idx], bits, n_bits, &e->state, out);
}

int dt_multi_encode_flush(dt_multi_encoder *e, int idx, uint8_t *out,
                          int max_out) {
  if (!encode_args_ok(e, idx, out, max_out)) {
    return DT_ERR_ARG;
  }
  if ((dt_ccode_k(e->codes[idx]) - 1) * dt_ccode_n(e->codes[idx]) > max_out) {
    return DT_ERR_ARG;
  }
  return dt_ccode_encode_flush(e->codes[idx], &e->state, out);
}
