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
 * Viterbi codec: realizes the abstract dt_decoder interface over the Viterbi
 * hard-decision decode engine. The code handle is dt_cc_code throughout. The
 * decoder takes no channel-model parameters. To encode, use the standalone
 * encoder (src/cc/encoder).
 *
 * The decode engine (see viterbi/decode.c) is complete; this file is just the
 * vtable plumbing that adapts it to the abstract interface.
 */

#include <drifty/cc/viterbi.h>

#include "viterbi/decode.h" /* dt_cc_viterbi_stream_decoder + dt_cc_viterbi_stream_decode* */
#include <drifty/stdlib.h>

/* dt_bit is uint8_t (bit.h), the same element type the engine's decode buffers
 * use, so the dt_bit* <-> uint8_t* hand-offs below need no conversion. */

/* -- decoder --------------------------------------------------------------- */

static int viterbi_decoder_begin(dt_decoder *dec, dt_bit *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* no preamble to emit */
}

static int viterbi_decoder_decode(dt_decoder *dec, dt_bit *dst, size_t dst_len,
                                  const dt_bit *src, size_t src_len) {
  dt_cc_viterbi_stream_decoder *sd = dec->data;
  return dt_cc_viterbi_stream_decode(sd, src, (int)src_len, dst, (int)dst_len);
}

static int viterbi_decoder_finalize(dt_decoder *dec, dt_bit *dst, size_t dst_len) {
  dt_cc_viterbi_stream_decoder *sd = dec->data;
  return dt_cc_viterbi_stream_decode_flush(sd, dst, (int)dst_len);
}

dt_decoder *dt_cc_viterbi_decoder_create(const dt_cc_code *code) {
  if (!code) {
    return NULL;
  }
  dt_cc_viterbi_stream_decoder *sd = dt_cc_viterbi_stream_decoder_create(code);
  if (!sd) {
    return NULL;
  }
  dt_decoder *dec = dt_malloc(sizeof(*dec));
  if (!dec) {
    dt_cc_viterbi_stream_decoder_destroy(sd);
    return NULL;
  }
  dec->begin = viterbi_decoder_begin;
  dec->decode = viterbi_decoder_decode;
  dec->finalize = viterbi_decoder_finalize;
  dec->data = sd;
  return dec;
}

void dt_cc_viterbi_decoder_destroy(dt_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_cc_viterbi_stream_decoder_destroy(dec->data);
  dt_free(dec);
}
