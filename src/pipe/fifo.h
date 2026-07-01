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
 * Internal growable FIFOs for the pipe implementations (pipes.c, streams.c): a
 * dt_bit queue and a dt_soft_bit queue, each a contiguous buffer with a valid
 * region [head, tail) that is compacted toward the front and doubled on demand.
 * Header-only (static inline) so each translation unit gets its own copy; not a
 * public header.
 */

#ifndef DRIFTY_SRC_PIPE_FIFO_H
#define DRIFTY_SRC_PIPE_FIFO_H

#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>

#include <limits.h>
#include <stddef.h>

/* Clamp a size_t count to the int the pull/push/tick contracts return. */
static inline int dt_fifo_clamp_int(size_t n) {
  return n > (size_t)INT_MAX ? INT_MAX : (int)n;
}

typedef struct {
  dt_soft_bit *buf;
  size_t cap, head, tail; // valid region [head, tail)
} dt_soft_fifo;

typedef struct {
  dt_bit *buf;
  size_t cap, head, tail;
} dt_bit_fifo;

/* Ensure room for `n` more elements: compact toward the front, then grow.
 * Returns 0, or -1 out of memory. */
static inline int dt_soft_fifo_reserve(dt_soft_fifo *f, size_t n) {
  if (f->tail + n <= f->cap) {
    return 0;
  }
  if (f->head > 0) {
    size_t len = f->tail - f->head;
    dt_memmove(f->buf, f->buf + f->head, len * sizeof(*f->buf));
    f->head = 0;
    f->tail = len;
    if (f->tail + n <= f->cap) {
      return 0;
    }
  }
  size_t need = f->tail + n, ncap = f->cap ? f->cap : 64;
  while (ncap < need) {
    ncap *= 2;
  }
  dt_soft_bit *nb = dt_realloc(f->buf, ncap * sizeof(*nb));
  if (!nb) {
    return -1;
  }
  f->buf = nb;
  f->cap = ncap;
  return 0;
}

static inline int dt_bit_fifo_reserve(dt_bit_fifo *f, size_t n) {
  if (f->tail + n <= f->cap) {
    return 0;
  }
  if (f->head > 0) {
    size_t len = f->tail - f->head;
    dt_memmove(f->buf, f->buf + f->head, len * sizeof(*f->buf));
    f->head = 0;
    f->tail = len;
    if (f->tail + n <= f->cap) {
      return 0;
    }
  }
  size_t need = f->tail + n, ncap = f->cap ? f->cap : 64;
  while (ncap < need) {
    ncap *= 2;
  }
  dt_bit *nb = dt_realloc(f->buf, ncap * sizeof(*nb));
  if (!nb) {
    return -1;
  }
  f->buf = nb;
  f->cap = ncap;
  return 0;
}

/* Append `n` elements from `src`; returns the count stored, or -1 out of memory. */
static inline int dt_bit_fifo_append(dt_bit_fifo *f, const dt_bit *src, size_t n) {
  size_t k = (size_t)dt_fifo_clamp_int(n);
  if (dt_bit_fifo_reserve(f, k) < 0) {
    return -1;
  }
  for (size_t i = 0; i < k; ++i) {
    f->buf[f->tail + i] = src[i];
  }
  f->tail += k;
  return (int)k;
}

static inline int dt_soft_fifo_append(dt_soft_fifo *f, const dt_soft_bit *src, size_t n) {
  size_t k = (size_t)dt_fifo_clamp_int(n);
  if (dt_soft_fifo_reserve(f, k) < 0) {
    return -1;
  }
  for (size_t i = 0; i < k; ++i) {
    f->buf[f->tail + i] = src[i];
  }
  f->tail += k;
  return (int)k;
}

/* Drain up to `n` elements into `dst`; returns the count taken (0 when empty). */
static inline int dt_soft_fifo_drain(dt_soft_fifo *f, dt_soft_bit *dst, size_t n) {
  size_t avail = f->tail - f->head;
  size_t k = (size_t)dt_fifo_clamp_int(n < avail ? n : avail);
  for (size_t i = 0; i < k; ++i) {
    dst[i] = f->buf[f->head + i];
  }
  f->head += k;
  if (f->head == f->tail) {
    f->head = f->tail = 0;
  }
  return (int)k;
}

static inline int dt_bit_fifo_drain(dt_bit_fifo *f, dt_bit *dst, size_t n) {
  size_t avail = f->tail - f->head;
  size_t k = (size_t)dt_fifo_clamp_int(n < avail ? n : avail);
  for (size_t i = 0; i < k; ++i) {
    dst[i] = f->buf[f->head + i];
  }
  f->head += k;
  if (f->head == f->tail) {
    f->head = f->tail = 0;
  }
  return (int)k;
}

#endif /* DRIFTY_SRC_PIPE_FIFO_H */
