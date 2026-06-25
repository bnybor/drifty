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

int dt_abs(int value) { return abs(value); }
double dt_log(double x) { return log(x); }
double dt_exp(double x) { return exp(x); }
