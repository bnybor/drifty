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

#ifndef DRIFTY_BLOCK_ENCODER_H
#define DRIFTY_BLOCK_ENCODER_H

#include <drifty/bit.h>
#include <drifty/result.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_block_encoder - the encode side of a block codec, called through function
 * pointers. It is the fixed-size-block counterpart to the streaming
 * dt_stream_encoder (<drifty/stream_encoder.h>): instead of a flowing stream, it
 * encodes one whole block at a time.
 *
 * The encoder owns both buffers and they are a fixed size for the life of the
 * encoder - decoded_len() is the block's information size and encoded_len() its
 * coded size (the code's k and n). Unlike the streaming interface there is no
 * per-call length: you fill the whole decoded block. Drive one instance as:
 *
 *   1. Write your input symbols into decoded_buf(), decoded_len() bits of them.
 *   2. Call encode() until it stops returning DT_AGAIN:
 *
 *        dt_result r;
 *        while ((r = enc->encode(enc)) == DT_AGAIN) { }
 *        if (r != DT_OK) { ... error ... }
 *
 *      encode() may make progress incrementally; DT_AGAIN means "not finished,
 *      call again", DT_OK means done, any other negative DT_ERR_* is an error.
 *   3. Read the coded symbols out of encoded_buf(), encoded_len() bits of them.
 *
 * To encode another block, overwrite decoded_buf(), call reset(), and repeat.
 *
 * Buffers carry one transmit-domain symbol per bit (dt_bit): DT_TRUE / DT_FALSE
 * to protect, DT_ERASURE for a don't-care deferred to the channel, or DT_INVALID
 * for a deliberate non-value (see <drifty/bit.h> and doc/data_flow_semantics.md).
 *
 * `data` is the implementation's private state - do not touch it. Build an
 * encoder with a factory and free it with the matching _destroy().
 */
typedef struct dt_block_encoder_t dt_block_encoder;
struct dt_block_encoder_t {
  // Number of bits in the decoded (input) block.
  size_t (*decoded_len)(dt_block_encoder *enc);
  // The encoder-owned buffer of decoded (input) bits; the caller fills it.
  dt_bit *(*decoded_buf)(dt_block_encoder *enc);
  // Number of bits in the encoded (output) block.
  size_t (*encoded_len)(dt_block_encoder *enc);
  // The encoder-owned buffer of encoded (output) bits; the caller reads it.
  dt_bit *(*encoded_buf)(dt_block_encoder *enc);

  /*
   * Encode the decoded buffer into the encoded buffer.
   *
   * This may be an incremental operation.  If the encoder has not failed
   * but is only partially complete it will return DT_AGAIN and expect
   * the caller to retry.
   */
  dt_result (*encode)(dt_block_encoder *enc);

  /*
   * Reset the internal state of the encoder.  Call this if you change
   * the buffer of decoded bits, before encoding it.
   */
  dt_result (*reset)(dt_block_encoder *enc);

  // implementation-private state; do not access
  void *data;
};

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_BLOCK_ENCODER_H */
