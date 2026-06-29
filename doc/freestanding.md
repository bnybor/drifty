# drifty — Freestanding & embedded

drifty's core is **freestanding** in the C-standard sense: it does not depend on a
hosted C standard library and can be built for bare-metal and embedded targets. This
page covers what that guarantees, the `dt_*` proxy boundary through which the core
reaches the few libc facilities it needs, the two archives the build produces, and
how to port the proxies to a platform without a libc.

For the library overview see the [README](../README.md); for the symbol model see
[Data-flow semantics](data_flow_semantics.md).

## What "freestanding" means here

The core (everything under `src/`, archived as `libdrifty_bare.a`) is compiled
`-ffreestanding -fno-builtin` as plain **C11** and holds to the freestanding
contract:

- **No C standard library calls.** The core never calls `malloc`, `memcpy`,
  `logf`, or any other libc function directly. The only libc-shaped operations it
  performs go through the `dt_*` proxies below, which are the single, explicit
  boundary where libc is touched.
- **Only freestanding headers.** The public headers include nothing beyond the
  headers a freestanding implementation must provide — `<stdint.h>`, `<stddef.h>`,
  and `<stdbool.h>`. No `<stdio.h>`, `<stdlib.h>`, `<string.h>`, or `<math.h>`.
- **No I/O and no hidden global state to initialize.** Nothing in the core reads or
  writes files, the console, or the clock. All state lives in objects you create
  explicitly with the `_create` factories and free with `_destroy`.
- **One symbol per byte.** Every position is a single-byte [`dt_bit`](bit.md)
  rather than a packed bitfield, so there are no word-size or bit-order assumptions
  in the data plane. Internal floating-point math is native-endian and needs only
  an IEEE-754 `float`.
- **C11, no extensions required.** No compiler-specific intrinsics are needed to
  build the portable core. (The optional `-DDRIFTY_NATIVE=ON` adds host vector
  tuning for hosted builds; it is off by default and never required.)

## The `dt_*` proxy boundary

The handful of libc facilities the core needs are declared in
[`<drifty/stdlib.h>`](../stdlib/include/drifty/stdlib.h) and reached only through
these proxies. Each mirrors its standard counterpart's signature, so a declaration
is always in scope at the call site:

| Proxy | Mirrors | Used for |
|-------|---------|----------|
| `void *dt_malloc(size_t)` | `malloc` | allocate decoder/codec working state at `_create` |
| `void *dt_calloc(size_t, size_t)` | `calloc` | zero-initialized working state |
| `void *dt_realloc(void *, size_t)` | `realloc` | grow internal buffers as a stream lengthens |
| `void  dt_free(void *)` | `free` | release state at `_destroy` |
| `void *dt_memcpy(void *, const void *, size_t)` | `memcpy` | bulk symbol moves |
| `void *dt_memmove(void *, const void *, size_t)` | `memmove` | overlapping buffer compaction |
| `void *dt_memset(void *, int, size_t)` | `memset` | clear buffers / soft-bit records |
| `float dt_log(float)` | `logf` | branch metrics in the soft/MAP decoders |
| `float dt_exp(float)` | `expf` | branch metrics in the soft/MAP decoders |

That is the **entire** external surface the core requires. `dt_log` and `dt_exp`
are the only math functions, and only the soft / max-log-MAP decoders (`bcjr`,
`hybrid`, `maxir`) call them — a build that uses only `viterbi` or `vindel` needs
no `float` math at all (the linker drops the unused proxies).

## Two archives

The build (see [README → Build](../README.md#build)) produces two static
libraries that differ only in whether the proxies are defined:

- **`libdrifty_bare.a`** — the freestanding core with the `dt_*` proxies left
  **undefined**. Your final link must supply them. This is the archive for
  bare-metal and embedded targets: the core asks for nine symbols and touches
  nothing else.
- **`libdrifty.a`** — the same core with a default proxy implementation
  ([`stdlib/src/stdlib.c`](../stdlib/src/stdlib.c)) archived in, backed by the host
  libc (`malloc`/`free`, `mem*`, `logf`/`expf`) and linked against `libm`. Use this
  for ordinary hosted builds, where the defaults are exactly what you want.

The default implementation is deliberately trivial — each proxy is a one-line
forward to its libc counterpart — so it doubles as a reference for your own port:

```c
void *dt_malloc(size_t size) { return malloc(size); }
void *dt_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
float dt_log(float x) { return logf(x); }
/* ... and the rest, one line each ... */
```

## Porting the proxies

To run on a platform without a hosted libc, link `libdrifty_bare.a` and provide
your own definitions of the nine symbols above. Two are worth a note:

- **Allocation.** Decoders allocate working state at `_create` and may **grow**
  internal buffers with `dt_realloc` as the stream lengthens (the running decoders
  keep a received-history window; the `marker` frame codec keeps a token FIFO).
  Allocation is therefore not confined to `_create` — but every byte still flows
  through your `dt_malloc` / `dt_realloc` / `dt_free`, so you retain full control.
  Back them with a fixed static arena, a pool allocator, or an instrumented
  allocator and the core never reaches a heap you did not choose. The classic
  freestanding approach — point `dt_malloc`/`dt_realloc` at a bump or pool
  allocator over a static byte array and make `dt_free` a no-op (or pool release) —
  works directly.
- **Math.** `dt_log` / `dt_exp` need only single-precision accuracy. Route them to
  your platform's math library, a CMSIS-DSP routine, or a small polynomial
  approximation; the decoders use rough branch metrics and do not depend on
  last-bit precision.

A minimal bare-metal proxy file might look like:

```c
#include <drifty/stdlib.h>

static unsigned char arena[64 * 1024];
static size_t arena_top = 0;

void *dt_malloc(size_t size) {
  /* align up to 8 and bump; out-of-arena returns NULL, which the core treats
   * as an allocation failure (factories return NULL, callers must check). */
  size_t a = (arena_top + 7u) & ~(size_t)7u;
  if (a + size > sizeof arena) return 0;
  arena_top = a + size;
  return &arena[a];
}
void  dt_free(void *p) { (void)p; }            /* arena freed all at once */
void *dt_calloc(size_t n, size_t s) { /* size *n, then dt_memset 0 */ }
void *dt_realloc(void *p, size_t s) { /* copy-grow within the arena */ }

/* dt_memcpy / dt_memmove / dt_memset: trivial byte loops, or your platform's */
/* dt_log / dt_exp: your single-precision math                                */
```

Every `_create` can return `NULL` on allocation failure, so a fixed arena that
fills up surfaces cleanly as a failed factory call rather than undefined behavior —
check the factory return value, as the [README quick start](../README.md#quick-start)
shows.

## Size-optimized builds

For a smaller archive on space-constrained targets, configure a size-optimized
build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build
```

`MinSizeRel` compiles `-Os` and additionally drops the unwind tables
(`.eh_frame`) and the compiler `.ident` string — dead weight for a plain-C library
with no exceptions — and strips the localized internal symbol names left after the
archive's symbol-hiding pass. The trade-off is that debuggers and profilers cannot
unwind through these frames, which is acceptable for a minimum-size release.

## See also

- [README → Freestanding & embedded](../README.md#freestanding--embedded) — the
  overview this page expands on.
- [`<drifty/stdlib.h>`](../stdlib/include/drifty/stdlib.h) — the proxy
  declarations, and [`stdlib/src/stdlib.c`](../stdlib/src/stdlib.c) — the default
  host-libc implementation to model your port on.
- [Data-flow semantics](data_flow_semantics.md) and [Symbols (`bit`)](bit.md) —
  the one-symbol-per-byte alphabet that keeps the data plane free of word-size
  assumptions.
