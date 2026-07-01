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

#ifndef DRIFTY_CC_ENCODER_H
#define DRIFTY_CC_ENCODER_H

#include <drifty/cc/ccode.h>
#include <drifty/stream_encoder.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The shared convolutional encoder: the single dt_stream_encoder that every cc codec
 * encodes through, built over a dt_cc_code (<drifty/cc/ccode.h>) and driven through
 * the streaming vtable (<drifty/stream_encoder.h>).
 */

/* Build an encoder over `code` - the convolutional encoder every codec
 * encodes through. Input bits are normally DT_FALSE / DT_TRUE; it also carries
 * non-boolean inputs - a DT_INVALID input emits DT_INVALID coded bits (structural
 * poison) and a DT_ERASURE input emits DT_ERASURE coded bits (deferred to the
 * channel) - which decoders that read those markers (e.g. maxir, bcjr) rely on.
 * Returns NULL on a bad argument or out of memory. */
dt_stream_encoder *dt_cc_encoder_create(const dt_cc_code *code);
/* Free an encoder from dt_cc_encoder_create(). Passing NULL is fine. */
void dt_cc_encoder_destroy(dt_stream_encoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CC_ENCODER_H */
