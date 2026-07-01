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

#ifndef DRIFTY_CONTAINER_H
#define DRIFTY_CONTAINER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_container - a simple ownership bag. It holds (object, destroyer) pairs and, when
 * destroyed, calls each destroyer on its object - so a routine that builds up a pile
 * of heap objects can register each as it goes and free them all with a single
 * dt_container_destroy() at the end, instead of a long tail of individual destroy
 * calls repeated on every exit path.
 *
 * It is NOT a vtable type and never touches the objects except to free them; it just
 * remembers how to destroy each one. Objects are freed in REVERSE order of add (the
 * last registered is freed first), so an object may safely depend on one added before
 * it. `data` is not exposed - the handle is opaque.
 */
typedef struct dt_container_t dt_container;

/* Create an empty container. Returns NULL out of memory. */
dt_container *dt_container_create(void);

/* Register `obj` to be freed by `destroyer(obj)` when the container is destroyed. A
 * NULL `destroyer` holds `obj` without ever freeing it (to group a lifetime managed
 * elsewhere). Returns `obj`, so it can wrap a create call inline:
 *
 *   dt_pipe *p = dt_container_add(c, dt_pipe_encoder_create(enc),
 *                                 (void (*)(void *))dt_pipe_encoder_destroy);
 *
 * On failure (a NULL container, or out of memory) it frees `obj` with `destroyer` so
 * nothing leaks, and returns NULL; a NULL `obj` is a no-op returning NULL. */
void *dt_container_add(dt_container *container, void *obj, void (*destroyer)(void *));

/* Detach the first entry whose object is `obj`, WITHOUT calling its destroyer - the
 * caller takes back ownership. Does nothing if `obj` is not held. */
void dt_container_remove(dt_container *container, void *obj);

/* Destroy the container, calling each registered destroyer on its object in reverse
 * order of add. Passing NULL is fine. */
void dt_container_destroy(dt_container *container);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_CONTAINER_H */
