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

#ifndef DRIFTY_HYBRID_DRIFTY_H
#define DRIFTY_HYBRID_DRIFTY_H

/*
 * Umbrella header for the full drifty API. The library is split into the
 * sender's half (encode.h - pick a code and encode), the receiver's half
 * (decode.h - stream-decode received bits), and a blind code comparison
 * (compare.h - do two streams share a code?). Include this to get all of them
 * at once, or include just the part you need.
 */
#include <drifty/hybrid/compare.h>
#include <drifty/hybrid/decode.h>
#include <drifty/hybrid/encode.h>
#include <drifty/hybrid/multi_decode.h>
#include <drifty/hybrid/multi_encode.h>

#endif /* DRIFTY_HYBRID_DRIFTY_H */
