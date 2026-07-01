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
 * dt_container - a growable array of (object, destroyer) pairs that frees them all,
 * in reverse order of add, on destroy. See <drifty/container.h>.
 */

#include <drifty/container.h>

#include <drifty/stdlib.h>

#include <stddef.h>

typedef struct {
  void *obj;
  void (*destroyer)(void *);
} dt_container_entry;

struct dt_container_t {
  dt_container_entry *entries;
  size_t n;   // entries in use
  size_t cap; // entries allocated
};

dt_container *dt_container_create(void) {
  dt_container *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->entries = NULL;
  c->n = 0;
  c->cap = 0;
  return c;
}

void *dt_container_add(dt_container *container, void *obj,
                       void (*destroyer)(void *)) {
  if (!obj) {
    return NULL;
  }
  if (!container) {
    if (destroyer) {
      destroyer(obj);
    }
    return NULL;
  }
  if (container->n == container->cap) {
    size_t ncap = container->cap ? container->cap * 2 : 8;
    dt_container_entry *ne = dt_realloc(container->entries, ncap * sizeof(*ne));
    if (!ne) {
      if (destroyer) {
        destroyer(obj);
      }
      return NULL;
    }
    container->entries = ne;
    container->cap = ncap;
  }
  container->entries[container->n].obj = obj;
  container->entries[container->n].destroyer = destroyer;
  container->n++;
  return obj;
}

void dt_container_remove(dt_container *container, void *obj) {
  if (!container) {
    return;
  }
  for (size_t i = 0; i < container->n; ++i) {
    if (container->entries[i].obj == obj) {
      for (size_t j = i + 1; j < container->n; ++j) {
        container->entries[j - 1] = container->entries[j];
      }
      container->n--;
      return;
    }
  }
}

void dt_container_destroy(dt_container *container) {
  if (!container) {
    return;
  }
  for (size_t i = container->n; i > 0; --i) {
    dt_container_entry *e = &container->entries[i - 1];
    if (e->destroyer) {
      e->destroyer(e->obj);
    }
  }
  dt_free(container->entries);
  dt_free(container);
}
