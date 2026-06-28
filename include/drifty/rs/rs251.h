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

#ifndef DRIFTY_RS_RS251_H
#define DRIFTY_RS_RS251_H

#include <drifty/block_decoder.h>
#include <drifty/block_encoder.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The rs251 block codec - a Reed-Solomon code over GF(251), presented through
 * the block encoder / decoder interfaces (<drifty/block_encoder.h>,
 * <drifty/block_decoder.h>). It is a systematic RS(n, k) code: k information
 * symbols become an n-symbol codeword with n - k parity symbols, and the decoder
 * corrects errors and erasures while 2*errors + erasures <= n - k. A natural
 * outer code for the inner convolutional codecs in <drifty/cc/>.
 *
 * Build a codec over a chosen (n, k) with the factories below and free it with
 * the matching _destroy(). Requires 1 <= k <= n <= 251.
 *
 * Buffer layout (dt_bit, one symbol per byte; see the block interfaces):
 *   - The decoded buffer is a whole number of bytes, each 8 dt_bit MSB-first; its
 *     length is the bytes that fit in k GF(251) symbols, times 8.
 *   - The encoded buffer is the n codeword symbols, each one GF(251) value as 8
 *     dt_bit MSB-first, so its length is 8 * n.
 * On decode, a received symbol whose 8 bits are not all DT_TRUE / DT_FALSE (i.e.
 * any DT_ERASURE / DT_INVALID / DT_ABSENT), or whose value is not a valid GF(251)
 * symbol (> 250), is treated as an erasure. decode() returns DT_ERR_DECODE when
 * the code cannot recover the block (2*errors + erasures > n - k).
 */

/* Build a block encoder for the systematic RS(n, k) code. Returns NULL on a bad
 * argument (n / k out of range) or out of memory. */
dt_block_encoder *dt_rs_rs251_block_encoder_create(uint16_t n, uint16_t k);
/* Free an encoder from dt_rs_rs251_block_encoder_create(). Passing NULL is fine. */
void dt_rs_rs251_block_encoder_destroy(dt_block_encoder *enc);

/* Build a block decoder for the systematic RS(n, k) code - an errors-and-erasures
 * hard-decision decoder. `s` is the number of spare (unspent) check symbols
 * required: a block spends 2*errors + erasures of its n - k check symbols, and a
 * decode that leaves fewer than `s` of them unspent is rejected as DT_ERR_DECODE
 * even though the algebra could correct it. Larger `s` trades correction power for
 * a smaller chance of silent miscorrection; s = 0 uses the full n - k budget.
 * Requires 0 <= s <= n - k. Returns NULL on a bad argument or out of memory. */
dt_block_decoder *dt_rs_rs251_block_decoder_create(uint16_t n, uint16_t k,
                                                   uint16_t s);
/* Free a decoder from dt_rs_rs251_block_decoder_create(). Passing NULL is fine. */
void dt_rs_rs251_block_decoder_destroy(dt_block_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_RS_RS251_H */
