# Changelog

All notable changes to drifty are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.1.0] - 2026-06-29

### Added

- **`detect_clean`** and **`detect_noisy`** — two new soft-only **meta-codecs** that
  blindly detect whether a convolutional code is present in an arbitrary bit stream,
  with no prior knowledge or coordination (no code, rate, generators, or alignment).
  Each reports, per position, two INDEPENDENT consistency reads (they need not sum to
  1): `c_erasure` (carrying the internal `c_lost`) = consistency with a code present,
  `c_absent` = consistency with random; both near 1 is the no-discriminating-evidence
  state. They share one API and output and differ only in footprint vs noise
  tolerance:
  - **`detect_clean`** (`dt_cc_detect_clean_soft_decoder_create`) — exact GF(2)
    **sliding** strided-window rank deficiency. A few KB of state and no transform;
    **indel-tolerant** (the sliding windows only need a locally indel-free aligned
    run) and sharply localizing, holding to ~1 % flips and ~2 % indels. The
    embeddable default for clean / very-low-noise streams. See `doc/cc/detect_clean.md`.
  - **`detect_noisy`** (`dt_cc_detect_noisy_soft_decoder_create`) — parity-check
    **bias** scored by a fast Walsh–Hadamard transform, which degrades gracefully
    with noise: it tolerates flips (~5–8 %), indels (~2–3 %), and light–moderate
    combinations of the two, at the cost of a ~64 KB transform histogram and somewhat
    more compute (~1.5–2× slower than detect_clean). See `doc/cc/detect_noisy.md`.

  Both factories take the same rich channel model as `hybrid` / `maxir`, used to
  calibrate the **code-present** read: expected flip noise holds it up on an
  otherwise-unstructured window (a real code could be hidden by the flips), while the
  no-code read is a model-independent fit to random and indels move neither.
  Includes an `examples/11_detect` demo that localizes a coded segment in random
  noise with `detect_clean`, then contrasts the two through a bit-flip and a combined
  flip+drift channel (where `detect_clean` collapses and `detect_noisy` holds on).

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

[Unreleased]: https://github.com/bnybor/drifty/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/bnybor/drifty/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/bnybor/drifty/releases/tag/v1.0.0
