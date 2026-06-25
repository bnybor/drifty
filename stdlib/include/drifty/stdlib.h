#ifndef STDLIB_INCLUDE_DRIFTY_STDLIB_H_
#define STDLIB_INCLUDE_DRIFTY_STDLIB_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The freestanding core (encode.c, decode.c, compare.c) is built -nostdlib
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

int dt_abs(int value);
double dt_log(double x);
double dt_exp(double x);

#ifdef __cplusplus
}
#endif

#endif /* STDLIB_INCLUDE_DRIFTY_STDLIB_H_ */
