# drifty — symbols (`dt_bit` and `dt_soft_bit`)

Every position that flows through drifty — source, coded, received, recovered — is
one byte holding one symbol, never a packed bitfield. The native unit is the
*position*; a *value* is a property a position may or may not carry, which is why
the alphabet is richer than `{0, 1}`. The headers are
[`bit.h`](../include/drifty/bit.h) (hard symbols) and
[`soft_bit.h`](../include/drifty/soft_bit.h) (their soft counterpart).

This page is the C API for the alphabet; for what the symbols *mean* at each
interface — the transmit vs output domains, and why one symbol can carry two
readings — see [Data-flow semantics](data_flow_semantics.md).

## The hard symbol: `dt_bit`

`dt_bit` is a `uint8_t` (its underlying typedef name is `dt_bit_t`). It holds one
of six values:

| Symbol | Value | Meaning |
|--------|------:|---------|
| `DT_TRUE` | `0x0F` | a bound boolean: value 1 |
| `DT_FALSE` | `0x0E` | a bound boolean: value 0 |
| `DT_ERASURE` | `0x08` | a present position with no value bound — *don't-care* in, *don't-know* out |
| `DT_INVALID` | `0x0C` | a present position bound to a deliberate non-value (neither true nor false); round-trips |
| `DT_ABSENT` | `0x10` | a position the decoder infers was deleted or cannot place — **output only** |
| `DT_NONE` | `0x00` | the absence of any symbol; in no data flow, so zeroed storage is detectably not a symbol |

The values are not arbitrary: each symbol is a point in a small flag space — one
flag per axis of the model — so the predicates below are plain mask tests.

| Flag | Value | Axis |
|------|------:|------|
| `DT_DELETED` | `0x10` | the slot is asserted missing (output only) |
| `DT_PRESENT` | `0x08` | a slot exists in the transmit grid |
| `DT_BOUND` | `0x04` | a value is specified for the slot |
| `DT_BOOLEAN` | `0x02` | the bound value is a truth value |
| `DT_VALUE` | `0x01` | the truth value itself (meaningful iff `DT_BOOLEAN`) |

### Domains

Two domains partition where a symbol may legitimately appear:

- **transmit** — `{DT_TRUE, DT_FALSE, DT_ERASURE, DT_INVALID}`: what an encoder
  accepts and what travels the channel.
- **output** — the transmit symbols plus `DT_ABSENT`: what a decoder may emit.

`DT_NONE` is in neither.

### Predicates

Each macro is a single mask test:

| Macro | True for | Use |
|-------|----------|-----|
| `DT_IS_TRANSMIT(s)` | T, F, E, I | a valid encoder/decoder input symbol |
| `DT_IS_OUTPUT(s)` | T, F, E, I, ABSENT | a symbol a decoder may emit (excludes NONE) |
| `DT_IS_BOUND(s)` | T, F, I | carries a bound value (not an erasure) |
| `DT_IS_BIT(s)` | T, F | a usable boolean |
| `DT_IS_ABSENT(s)` | ABSENT | marked deleted (an erasure to an outer code) |
| `DT_BIT(s)` | — | extract the 0/1 payload; meaningful only when `DT_IS_BIT(s)` |

To recover a plain bit from a symbol, guard then extract:

```c
if (DT_IS_BIT(sym)) {
    int b = DT_BIT(sym);   /* 0 or 1 */
}
```

## The soft symbol: `dt_soft_bit`

A soft decoder reports each recovered position not as one hard `dt_bit` but as a
`dt_soft_bit` — the soft-decision counterpart, a graded *consistency* in `[0, 1]`
for each output-domain hypothesis: how well that reading fits the received stream.
The fields are **not** a probability split and need not sum to 1.

| Field | Hypothesis |
|-------|-----------|
| `c_false` / `c_true` | the position holds `DT_FALSE` / `DT_TRUE` |
| `c_erasure` | the value is unrecoverable (`DT_ERASURE`) |
| `c_invalid` | a recovered non-value (`DT_INVALID`) |
| `c_absent` | the position was deleted or not synchronized (`DT_ABSENT`) |
| `c_locked` | the decoder is tracking a valid coded stream — low during warm-up or after losing sync; independent of the value fields |

The hard symbol is the argmax projection of the value fields over the alphabet
(recoverability-first; see [Data-flow semantics](data_flow_semantics.md)). An
implementation need not model every hypothesis, but the three value-recovering soft
cc decoders (`bcjr`, `hybrid`, `maxir`) all populate the full alphabet — `c_invalid`
and `c_absent` included — and each hard decoder emits `DT_INVALID` for a poisoned slot
and `DT_ABSENT` where it loses lock.

The soft interfaces ([streaming](stream.md), [block](block.md)) carry `dt_soft_bit`
wherever the hard ones carry `dt_bit`.
