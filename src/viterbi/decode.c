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
 * Viterbi hard-decision decoder.
 *
 * STUB: the lifecycle plumbing is in place so the codec compiles, links, and the
 * public API works, but the decoding algorithm is not implemented yet -
 * dt_viterbi_stream_decode / _flush currently produce no output. The
 * forward add-compare-select pass and traceback go here.
 */

#include "decode.h"

#include <drifty/stdlib.h>

struct dt_viterbi_stream_decoder {
  const dt_ccode *code; /* borrowed; must outlive the decoder */
  /* TODO: Viterbi working state (per-state path metrics, the survivor/traceback
   * ring, the committed-bit cadence, and a buffer for received bits not yet
   * decided). */
};

dt_viterbi_stream_decoder *dt_viterbi_stream_decoder_create(
    const dt_ccode *code) {
  if (!code) {
    return NULL;
  }
  dt_viterbi_stream_decoder *d = dt_malloc(sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->code = code;
  return d;
}

void dt_viterbi_stream_decoder_destroy(dt_viterbi_stream_decoder *d) {
  dt_free(d); /* dt_free(NULL) is a no-op */
}

int dt_viterbi_stream_decode(dt_viterbi_stream_decoder *d, const uint8_t *in,
                             int n_in, uint8_t *out, int max_out) {
  if (!d || n_in < 0 || (n_in > 0 && !in) || max_out < 0 ||
      (max_out > 0 && !out)) {
    return DT_ERR_ARG;
  }
  (void)in;
  (void)out;
  /* TODO: buffer `in`, run the forward pass, and emit committed bits into `out`
   * (up to max_out). For now the decoder accepts input but produces nothing. */
  return 0;
}

int dt_viterbi_stream_decode_flush(dt_viterbi_stream_decoder *d, uint8_t *out,
                                   int max_out) {
  if (!d || max_out < 0 || (max_out > 0 && !out)) {
    return DT_ERR_ARG;
  }
  (void)out;
  /* TODO: drain the trellis at end-of-stream, emitting the last bits in flight. */
  return 0;
}
