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

#include <drifty/stdlib.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

void *dt_malloc(size_t size) { return malloc(size); }
void *dt_calloc(size_t count, size_t size) { return calloc(count, size); }
void *dt_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void dt_free(void *ptr) { free(ptr); }

void *dt_memcpy(void *dest, const void *src, size_t n) {
  return memcpy(dest, src, n);
}
void *dt_memmove(void *dest, const void *src, size_t n) {
  return memmove(dest, src, n);
}
void *dt_memset(void *dest, int value, size_t n) {
  return memset(dest, value, n);
}

/* Single-precision logf/expf, not log/exp: on a 32-bit MCU with a single-
 * precision FPU the double versions are software-emulated and far slower. */
float dt_log(float x) { return logf(x); }
float dt_exp(float x) { return expf(x); }
