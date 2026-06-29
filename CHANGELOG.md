# Changelog

All notable changes to drifty are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-06-28

First stable release. drifty is a small, freestanding C library for forward error
correction on a bit stream, with drift-tolerant convolutional codecs that stay
aligned through inserted and dropped bits.

### Added

#### Convolutional coding (`cc/`)

- The shared convolutional code `dt_cc_code`: a catalogue of standard codes (K3/K7
  rate-1/2, K7 rate-1/3, K5 rate-1/5, each with alternates) and custom codes built
  from generator polynomials. `dt_cc_code_n` / `dt_cc_code_k` accessors.
- One shared encoder (`dt_cc_encoder_*`) that every codec encodes through; it carries
  `DT_INVALID` poison and `DT_ERASURE` deferral into the coded stream.
- Five stream decoders over the shared code, differing in the channel damage they
  correct:
  - **`viterbi`** — hard-decision Viterbi (flips, erasures); no parameters.
  - **`bcjr`** — MAP forward–backward on a bit-aligned channel; hard and full soft
    output; blind acquisition and re-acquisition.
  - **`vindel`** — drift-tolerant (inserted/dropped bits); hard decision.
  - **`hybrid`** — drift-tolerant; hard and full soft output; an expressive channel
    model (asymmetric flips, value-specific insertions, fixed-value overwrites);
    `DT_INVALID` round-trip and `DT_ABSENT`.
  - **`maxir`** — drift-tolerant max-log-MAP; hard and full soft output, including a
    per-position deletion-marginal `c_absent`.

#### Block coding (`bc/`)

- **`rs251`** — a systematic Reed–Solomon RS(n, k) code over GF(251) that corrects
  errors and erasures while `2·errors + erasures ≤ n − k`; hard and soft-input
  decoders, with a spare-check-symbol guard against miscorrection.

#### Frame coding (`fc/`)

- **`naive`** — fixed-length frames (both sides agree on the length); hard and soft
  decoders.
- **`marker`** — variable-length frames delimited by escape sequences, with full
  payload transparency; hard and soft decoders.

#### Symbol model and interfaces

- The `dt_bit` alphabet (`DT_TRUE`, `DT_FALSE`, `DT_ERASURE`, `DT_INVALID`,
  `DT_ABSENT`, `DT_NONE`) and the `dt_soft_bit` per-position consistencies.
- The streaming, block, and frame interfaces (encoder / decoder / soft decoder
  vtables) and the shared `dt_result` status codes.

#### Build, packaging, and docs

- Two static archives: `libdrifty.a` (self-contained, with the host-libc shims
  bundled) and `libdrifty_bare.a` (the freestanding core, with the `dt_*` proxies
  left for the consumer to supply).
- A CMake package: `find_package(drifty)` exporting `drifty::drifty` and
  `drifty::drifty_bare`. Installs the public headers, including `drifty/stdlib.h`.
- A freestanding-friendly core: no C standard library dependency, plain C11, one
  symbol per byte, suitable for bare-metal and embedded targets.
- A complete set of runnable [example programs](examples/) and reference
  [documentation](doc/).

[Unreleased]: https://github.com/bnybor/drifty/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/bnybor/drifty/releases/tag/v1.0.0
