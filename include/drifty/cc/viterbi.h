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

#ifndef DRIFTY_CC_VITERBI_H
#define DRIFTY_CC_VITERBI_H

#include <drifty/cc/ccode.h>
#include <drifty/stream_decoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The viterbi codec - a plain convolutional encoder and Viterbi hard-decision
 * decoder over a dt_cc_code. Unlike the hybrid and vindel codecs it does not
 * track drift (inserted or dropped bits) and takes no channel-model parameters:
 * the decoder is built from the code alone. This is the single header to
 * include for its public API.
 *
 * Build a decoder over a dt_cc_code with the factory below, drive it through its
 * vtable (see decoder.h), and free it with the matching _destroy(). The code
 * must outlive everything built from it. To encode, use the standalone encoder
 * in <drifty/cc/encoder.h>.
 */

/* Build a hard-decision Viterbi decoder over `code`. Takes no channel-model
 * parameters. Returns NULL on a bad argument or out of memory. */
dt_stream_decoder *dt_cc_viterbi_decoder_create(const dt_cc_code *code);
/* Free a decoder from dt_cc_viterbi_decoder_create(). Passing NULL is fine. */
void dt_cc_viterbi_decoder_destroy(dt_stream_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_VITERBI_H */
