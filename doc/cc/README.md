# drifty — Convolutional coding (`cc`)

The convolutional codecs. Every codec is built over a shared convolutional code
(`dt_cc_code`, see [`include/drifty/cc/ccode.h`](../../include/drifty/cc/ccode.h)):
pick a code, encode through the one shared **encoder**, send the coded stream over
the channel, and recover it with the **decoder** whose error model matches your
channel. The encoder is common to all codecs — the **decoder is what differs**.

The symbol alphabet these components speak is defined in
[Data-flow semantics](../data_flow_semantics.md); read it first.

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

All five correct substitutions (flipped bits) and erasures. They differ in whether
they track **drift** (inserted/dropped bits), whether they emit **soft** per-bit
consistencies as well as a hard decision, and how rich a channel model they take.

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
