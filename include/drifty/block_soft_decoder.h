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

#ifndef DRIFTY_BLOCK_SOFT_DECODER_H
#define DRIFTY_BLOCK_SOFT_DECODER_H

#include <drifty/bit.h>
#include <drifty/result.h>
#include <drifty/soft_bit.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_block_soft_decoder - the decode side of a block codec that consumes
 * soft-bit channel input. It is the soft-input counterpart of dt_block_decoder
 * (<drifty/block_decoder.h>): the same fixed-size-block model and vtable, but its
 * encoded (received) buffer holds graded dt_soft_bit values - per-symbol
 * consistencies - rather than hard dt_bit symbols. It still produces a hard
 * decoded output.
 *
 * The decoder owns both buffers and they are a fixed size for the life of the
 * decoder - encoded_len() is the block's coded size and decoded_len() its
 * information size (the code's n and k). Unlike the streaming interface there is
 * no per-call length: you fill the whole encoded block. Drive one instance as:
 *
 *   1. Write the received soft bits into encoded_buf(), encoded_len() of them.
 *   2. Call decode() until it stops returning DT_AGAIN:
 *
 *        dt_result r;
 *        while ((r = dec->decode(dec)) == DT_AGAIN) { }
 *        if (r != DT_OK) { ... error ... }
 *
 *      decode() may make progress incrementally; DT_AGAIN means "not finished,
 *      call again", DT_OK means done, any other negative DT_ERR_* is an error.
 *   3. Read the recovered symbols out of decoded_buf(), decoded_len() bits of them.
 *
 * To decode another block, overwrite encoded_buf(), call reset(), and repeat.
 *
 * encoded_buf() holds the received symbols as dt_soft_bit records (graded
 * consistencies; see <drifty/soft_bit.h>). decoded_buf() holds one output-domain
 * symbol per recovered information position: DT_TRUE / DT_FALSE recovered,
 * DT_ERASURE for a value that could not be recovered, DT_INVALID for a recovered
 * non-value, or DT_ABSENT for a position with no verdict (see <drifty/bit.h> and
 * doc/data_flow_semantics.md).
 *
 * `data` is the implementation's private state - do not touch it. Build a
 * decoder with a factory and free it with the matching _destroy().
 */
typedef struct dt_block_soft_decoder_t dt_block_soft_decoder;
struct dt_block_soft_decoder_t {
  // Number of bits in the decoded (output) block.
  size_t (*decoded_len)(dt_block_soft_decoder *dec);
  // The decoder-owned buffer of decoded (output) bits; the caller reads it.
  dt_bit *(*decoded_buf)(dt_block_soft_decoder *dec);
  // Number of soft bits in the encoded (input) block.
  size_t (*encoded_len)(dt_block_soft_decoder *dec);
  // The decoder-owned buffer of encoded (input) soft bits; the caller fills it.
  dt_soft_bit *(*encoded_buf)(dt_block_soft_decoder *dec);

  /*
   * Decode the encoded buffer into the decoded buffer.
   *
   * This may be an incremental operation.  If the decoder has not failed
   * but is only partially complete it will return DT_AGAIN and expect
   * the caller to retry.
   */
  dt_result (*decode)(dt_block_soft_decoder *dec);

  /*
   * Reset the internal state of the decoder.  Call this if you change
   * the buffer of encoded soft bits, before decoding it.
   */
  dt_result (*reset)(dt_block_soft_decoder *dec);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_BLOCK_SOFT_DECODER_H */
