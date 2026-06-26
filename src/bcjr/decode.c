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
 * BCJR (MAP / forward-backward) decoder - STUB.
 *
 * The sliding-window plan, for when this is implemented: run a forward (alpha)
 * recursion and a backward (beta) recursion over the trellis across a window of
 * `decision_depth` steps, combining them with the per-step branch metrics
 * (gamma) derived from the channel model to get each input bit's a-posteriori
 * log-likelihood. The hard decision is the sign of that LLR; the soft output is
 * the pair of normalised consistencies. Erasures contribute a neutral branch
 * metric (no evidence either way), exactly as in the Viterbi decoder.
 *
 * What is wired today: the decoder handle, the channel-model validation, and the
 * dual hard/soft output contract (dt_bcjr_stream_decode writes `out` and/or
 * `details`). What is missing: the alpha/beta/gamma recursions themselves -
 * decode and flush currently emit no bits. See the TODOs below.
 */

#include "decode.h"

#include <drifty/bit.h>
#include <drifty/stdlib.h>

#include "../ccode_internal.h"

/* Trellis dimensions and the channel model, taken from the code and params at
 * create time. The working buffers the recursions will need (alpha/beta rings, a
 * received-bit window, the gamma branch metrics) are not allocated yet - they
 * land with the algorithm. */
struct dt_bcjr_stream_decoder {
  const dt_ccode *code; /* borrowed; must outlive the decoder */

  int n;              /* code->n: coded bits per step / per group */
  int num_states;     /* code->n_states: 1 << (K-1)               */
  int decision_depth; /* sliding-window / look-ahead depth        */

  /* Branch-metric constants, in cost (negative-log-likelihood) units, derived
   * from the channel model. */
  float cost_match, cost_miss, cost_erase;
};

dt_bcjr_stream_decoder *dt_bcjr_stream_decoder_create(
    const dt_ccode *code, const dt_bcjr_stream_params *params) {
  if (!code || !params) {
    return NULL;
  }
  const int decision_depth = params->decision_depth;
  const float p_flip = params->p_flip;
  const float p_erase = params->p_erase;
  if (decision_depth < 1) {
    return NULL;
  }
  if (!(p_flip > 0.0f && p_flip < 1.0f) || !(p_erase >= 0.0f && p_erase < 1.0f)) {
    return NULL;
  }

  dt_bcjr_stream_decoder *d = dt_calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->code = code;
  d->n = code->n;
  d->num_states = code->n_states;
  d->decision_depth = decision_depth;

  /* Channel model: a coded bit is erased with prob p_erase; otherwise it is
   * received and flipped with prob p_flip. Costs are negative log-likelihoods,
   * the gamma terms the forward-backward recursions will sum along each branch. */
  d->cost_match = -dt_log((1.0f - p_erase) * (1.0f - p_flip));
  d->cost_miss = -dt_log((1.0f - p_erase) * p_flip);
  d->cost_erase = -dt_log(p_erase); /* +inf when p_erase == 0 (never read) */

  return d;
}

void dt_bcjr_stream_decoder_destroy(dt_bcjr_stream_decoder *d) {
  if (!d) {
    return;
  }
  dt_free(d); /* dt_free(NULL) is a no-op */
}

int dt_bcjr_stream_decode(dt_bcjr_stream_decoder *d, const uint8_t *in, int n_in,
                          uint8_t *out, dt_bcjr_decode_details *details,
                          int max_out) {
  if (!d || n_in < 0 || (n_in > 0 && !in) || max_out < 0 ||
      (max_out > 0 && !out && !details)) {
    return DT_ERR_ARG;
  }
  /* TODO: buffer `in`, advance the forward (alpha) recursion, and once a step is
   * decision_depth behind the frontier, combine it with the backward (beta)
   * recursion to commit one decoded bit (into `out`) and its soft output (into
   * `details`). Emits nothing until then. */
  (void)in;
  (void)out;
  (void)details;
  return 0;
}

int dt_bcjr_stream_decode_flush(dt_bcjr_stream_decoder *d, uint8_t *out,
                                dt_bcjr_decode_details *details, int max_out) {
  if (!d || max_out < 0 || (max_out > 0 && !out && !details)) {
    return DT_ERR_ARG;
  }
  /* TODO: at end of stream, run the backward recursion over the bits still in
   * the window and drain the remaining decisions. Emits nothing until then. */
  (void)out;
  (void)details;
  return 0;
}
