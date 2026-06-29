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

#ifndef STDLIB_INCLUDE_DRIFTY_STDLIB_H_
#define STDLIB_INCLUDE_DRIFTY_STDLIB_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The freestanding core (encode.c, decode.c) is built -nostdlib
 * -ffreestanding -fno-builtin and must not pull in the C standard library
 * directly. It reaches the few libc facilities it needs through these dt_*
 * proxies instead, so this translation unit (stdlib.c) is the single, explicit
 * boundary where libc is touched. Each proxy mirrors its standard counterpart's
 * signature, so a declaration is always in scope at the call site (an implicitly
 * declared allocator would be assumed to return int, truncating 64-bit pointers).
 */

void *dt_malloc(size_t size);
void *dt_calloc(size_t count, size_t size);
void *dt_realloc(void *ptr, size_t size);
void dt_free(void *ptr);

void *dt_memcpy(void *dest, const void *src, size_t n);
void *dt_memmove(void *dest, const void *src, size_t n);
void *dt_memset(void *dest, int value, size_t n);

float dt_log(float x);
float dt_exp(float x);

#ifdef __cplusplus
}
#endif

#endif /* STDLIB_INCLUDE_DRIFTY_STDLIB_H_ */
