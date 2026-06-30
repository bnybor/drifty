# drifty — Convolutional coding (`cc`)

The convolutional codecs. Every codec is built over a shared convolutional code
([`dt_cc_code`](ccode.md)): pick a code, encode through the one shared
**encoder**, send the coded stream over
the channel, and recover it with the **decoder** whose error model matches your
channel. The encoder is common to all codecs — the **decoder is what differs**.

The symbol alphabet these components speak is defined in
[Data-flow semantics](../data_flow_semantics.md); read it first.

## The code

- **[ccode](ccode.md)** — `dt_cc_code` and `dt_cc_code_*`. The convolutional code
  every codec shares: the ready-made catalogue, the custom-code generator format,
  and the `dt_cc_code_n` / `dt_cc_code_k` accessors. The encoder and every decoder
  are built over one of these.

## Encoder

- **[encoder](encoder.md)** — `dt_cc_encoder_*`. The single convolutional encoder
  every codec encodes through. Standalone and code-only; carries `DT_INVALID`
  poison and `DT_ERASURE` deferral through to the coded stream.

## Decoders

| Decoder | Algorithm | Drift (ins/del) | Soft output | Channel parameters |
|---------|-----------|:---------------:|:-----------:|--------------------|
| **[viterbi](viterbi.md)** | Viterbi ML, hard-decision | — | — | none (built from the code alone) |
| **[bcjr](bcjr.md)** | BCJR (MAP, forward–backward) | — | ✅ full alphabet | `p_flip`, `p_erase` |
| **[vindel](vindel.md)** | drift-tolerant Viterbi | ✅ | — | `p_sub`, `p_ins`/`p_del`, `p_erase` |
| **[hybrid](hybrid.md)** | drift-tolerant, hard + soft | ✅ | ✅ full alphabet | `p_flip`, insertions ×3, `p_del`, overwrites ×3 |
| **[maxir](maxir.md)** | drift-tolerant max-log-MAP | ✅ | ✅ full alphabet | `p_flip`, insertions ×3, `p_del`, overwrites ×3 |
| **[detect_clean](detect_clean.md)** | blind code-presence detector (GF(2) rank) | ✅ tolerant | ✅ `c_erasure`/`c_absent` | `p_flip`, insertions ×3, `p_del`, overwrites ×3 (calibration only) |
| **[detect_noisy](detect_noisy.md)** | blind code-presence detector (FWHT parity bias) | ✅ tolerant | ✅ `c_erasure`/`c_absent` | `p_flip`, insertions ×3, `p_del`, overwrites ×3 (calibration only) |

The first five correct substitutions (flipped bits) and erasures. They differ in
whether they track **drift** (inserted/dropped bits), whether they emit **soft**
per-bit consistencies as well as a hard decision, and how rich a channel model they
take. The last two are the odd ones out — **meta-codecs** that do not decode at
all: each blindly detects whether a convolutional code is present in an arbitrary
stream (no code/rate/alignment known) and reports a per-position code-present
(`c_erasure`) vs no-code (`c_absent`) consistency. Neither is
built over a `dt_cc_code`, but both take the same rich channel model as `hybrid` /
`maxir` (used to calibrate the code-present read when flips are expected). Both also
read a **`DT_INVALID`** symbol as *two-sided evidence* — an un-encodable placement (a
lone invalid, or runs of differing length) both contradicts a code and favors no-code,
reading `(low, high)`. They share one API and output and differ only in footprint vs
noise tolerance: **`detect_clean`** uses exact GF(2)
rank — a few KB, for clean / very-low-noise streams ([page](detect_clean.md)) —
while **`detect_noisy`** scores parity-check *bias* via a Walsh–Hadamard transform —
a ~64 KB histogram and somewhat more compute, tolerating flips, indels, and combinations
([page](detect_noisy.md)).

## Choosing a decoder

- **Fixed grid (no drift), hard decision** → [`viterbi`](viterbi.md) — the
  cheapest.
- **Fixed grid, soft output** → [`bcjr`](bcjr.md) — same flip/erasure channel as
  viterbi, but per-bit a-posteriori soft information.
- **Drifting channel, hard decision, no overwrites** → [`vindel`](vindel.md).
- **Drifting channel with fixed-value overwrites, hard and/or soft, with
  `DT_INVALID` round-trip and the full output alphabet (including `DT_ABSENT`)** →
  [`hybrid`](hybrid.md).
- **Strongest: all of the above with the heavier full-forward-backward per-bit
  soft detail, which degrades most gracefully on an uncertain channel** →
  [`maxir`](maxir.md).

All four lock-tracking decoders (`bcjr`, `vindel`, `hybrid`, `maxir`) also
**re-acquire sync** after a sustained loss of lock — re-seeding the trellis to
relock downstream; only `viterbi`, which starts from a known state, does not.

> `maxir` and `bcjr` are both forward–backward decoders but are **independent**
> implementations (drift-tolerant vs flip/erasure-only) — a change to one does not
> apply to the other.

## Driving any codec

Every decoder is a `dt_stream_decoder` (or `dt_stream_soft_decoder`) driven through
the same three-phase vtable — `begin` → `decode` (repeat) → `finalize` — with a
warm-up delay and a buffering rule; the encoder is a `dt_stream_encoder` driven
`begin` → `encode` → `finalize`. See the [streaming interface](../stream.md) for
the full contract, and each codec's page for the specifics.
