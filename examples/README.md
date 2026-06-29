# drifty — examples

A complete set of small, runnable programs, each demonstrating one capability of
drifty. They use **only the public API** (`#include <drifty/...>`) and link the
public library, so each is also a template for your own code.

Each example lives in its own subfolder as `main.c`. The shared
[`util.h`](util.h) holds the boring scaffolding every example needs — a
deterministic PRNG, channel simulators (flip / erase / insert / delete), the
stream encode/decode drive loops, and print helpers. **None of `util.h` is part of
drifty**; it just keeps each example focused on the API it shows off.

## Building and running

The examples build by default with a top-level build:

```sh
cmake -S . -B build
cmake --build build
```

(or configure with `-DDRIFTY_BUILD_EXAMPLES=ON` explicitly). Each program lands in
`build/examples/<name>/<name>` and takes no arguments:

```sh
./build/examples/01_basics/01_basics
./build/examples/03_decoders/03_decoders
```

They are deterministic (fixed PRNG seeds), so output is reproducible.

To build one against an installed `libdrifty.a` instead, the public-API include
and a link against the library plus `libm` is all you need, e.g.
`cc 01_basics/main.c -Iexamples -ldrifty -lm`.

## The examples

Read them roughly in order — each builds on the vocabulary of the last.

| # | Example | Demonstrates |
|---|---------|--------------|
| 01 | **[basics](01_basics)** | The core round trip: pick a code, encode, corrupt, decode (Viterbi). Convolutional codes correct flipped bits. |
| 02 | **[custom_code](02_custom_code)** | Defining your own convolutional code from generator polynomials (`dt_cc_code_create`), not just the presets. |
| 03 | **[decoders](03_decoders)** | All five decoders side by side: `viterbi` / `bcjr` on a bit-aligned (flip+erasure) channel; `vindel` / `hybrid` / `maxir` additionally tracking **drift** (inserted/dropped bits). |
| 04 | **[soft_output](04_soft_output)** | The soft decoder: per-bit `dt_soft_bit` consistencies (`c_true` / `c_false` / `c_erasure` / `c_locked` …) and how they move with reliability. |
| 05 | **[symbols](05_symbols)** | The non-boolean transmit symbols `DT_INVALID` (deliberate non-value / poison) and `DT_ERASURE` (deferred value) round-tripping end to end. |
| 06 | **[acquisition](06_acquisition)** | Blind acquisition (the `c_locked` ramp), the lock signal, and `DT_ABSENT` — the decoder refusing to emit confident garbage off a stream it can't track. |
| 07 | **[block_rs251](07_block_rs251)** | The `rs251` Reed–Solomon **block** code: errors + erasures within the `2·err + eras ≤ n−k` budget, a failure just past it, and the soft-input decoder. |
| 08 | **[frames_naive](08_frames_naive)** | The **frame** interface with fixed-length (`naive`) frames and the `OUTSIDE → BEGIN → INSIDE → END` boundary state machine. |
| 09 | **[frames_marker](09_frames_marker)** | Variable-length (`marker`) frames, escape-sequence transparency (any payload survives), hard and soft frame decoders. |
| 10 | **[concatenated](10_concatenated)** | The full stack: an `rs251` outer code over a `hybrid` **soft** inner code across a drifting channel, with the hard path alongside for contrast. The worked example from [doc/concatenated.md](../doc/concatenated.md). |

## Capability coverage

Between them the examples exercise the whole public surface: the [shared
code](../doc/cc/ccode.md) (presets and custom), the [shared
encoder](../doc/cc/encoder.md), all five [convolutional
decoders](../doc/cc/README.md) (hard and soft), drift tolerance, the full symbol
alphabet (`DT_TRUE` / `DT_FALSE` / `DT_ERASURE` / `DT_INVALID` / `DT_ABSENT`),
lock / blind acquisition, the [`rs251` block code](../doc/bc/rs251.md) (hard and
soft), the [frame codecs](../doc/fc/README.md) (`naive` and `marker`, hard and
soft), and the [concatenated stack](../doc/concatenated.md). See
[`doc/`](../doc/README.md) for the reference behind each.
