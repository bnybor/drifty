# drifty — `hybrid` decoder

A convolutional, **drift-tolerant** decoder over a `dt_cc_code` with both a
**hard-decision** and a **soft-output** front end. On top of flips, erasures, and
drift, it models a rich channel: spurious insertions split by value, deletions,
and **overwrites** that force a coded bit to a fixed value regardless of what was
sent.

For the symbol alphabet and what each interface means, see
[Data-flow semantics](../data_flow_semantics.md).

## Algorithm

Like [`vindel`](vindel.md), `hybrid` decodes over a state×drift trellis that
jointly recovers alignment and value, absorbing insertions and deletions within a
bounded drift window. Its branch metrics carry the full channel model below, and
it can emit a graded per-position **consistency** (soft output) in addition to the
hard symbol.

## What it corrects

| Channel effect | Handled? |
|----------------|----------|
| Substitution (flipped bit) | ✅ yes (`p_flip`) |
| Erasure (`DT_ERASURE`) | ✅ yes (`p_ovr_erase` doubles as the plain erasure rate) |
| Insertion / deletion (drift) | ✅ yes, up to `max_drift` |
| Value-biased insertion | ✅ yes (`p_ins_true` / `p_ins_false` / `p_ins_erase`) |
| Overwrite to fixed value | ✅ yes (`p_ovr_true` / `p_ovr_false` / `p_ovr_erase`) |
| `DT_INVALID` poison round-trip | ➖ not surfaced — soft output leaves `c_invalid` / `c_absent` at 0; use [`maxir`](maxir.md) for the full alphabet |

## API

```c
#include <drifty/cc/hybrid.h>

typedef struct {
  int   decision_depth;
  int   max_drift;
  float p_flip;
  float p_ins_true, p_ins_false, p_ins_erase;
  float p_del;
  float p_ovr_true, p_ovr_false, p_ovr_erase;
} dt_cc_hybrid_stream_params;

/* hard decision */
dt_stream_decoder      *dt_cc_hybrid_decoder_create(const dt_cc_code *code,
                                             const dt_cc_hybrid_stream_params *params);
void             dt_cc_hybrid_decoder_destroy(dt_stream_decoder *dec);

/* soft output */
dt_stream_soft_decoder *dt_cc_hybrid_soft_decoder_create(const dt_cc_code *code,
                                                  const dt_cc_hybrid_stream_params *params);
void             dt_cc_hybrid_soft_decoder_destroy(dt_stream_soft_decoder *dec);
```

Both factories take the same `params` (copied; need not outlive the call). The
code must outlive everything built from it. To encode, use the standalone encoder
in `<drifty/cc/encoder.h>`.

## Channel-model parameters

Use designated initializers; any field left out is 0. Rough probabilities are
fine — only their relative sizes matter.

| Field | Meaning |
|-------|---------|
| `decision_depth` | Output delay in bits before a bit is committed. Try ~`6 * dt_cc_code_k()`. **Required (≥ 1).** |
| `p_flip` | How often a coded bit is flipped, `0 < p_flip < 1`. **Required.** |
| `max_drift` | How far alignment may slip before the decoder loses track. `0` (default) corrects flips only; `4`–`8` recovers insertions/deletions. |
| `p_ins_true`, `p_ins_false`, `p_ins_erase` | How often a spurious `DT_TRUE` / `DT_FALSE` / `DT_ERASURE` bit is inserted. Their **sum** is the overall insertion rate (it sets how readily the decoder realigns); the split only biases which value an inserted bit is expected to carry. |
| `p_del` | How often a coded bit is dropped. The insertion rates and `p_del` together must sum to `< 1`. **Required when `max_drift > 0`.** |
| `p_ovr_true`, `p_ovr_false`, `p_ovr_erase` | How often a coded bit is overwritten with a fixed `DT_TRUE` / `DT_FALSE` / `DT_ERASURE` regardless of what was sent. The three must sum to `< 1`; all 0 if there are no overwrites. `p_ovr_erase` doubles as the plain erasure rate. |

## Driving the decoder

Hard and soft front ends are driven the same way — `begin` → `decode` (repeat) →
`finalize` — and share the same warm-up delay and buffering rule (see
`<drifty/stream_decoder.h>` and `<drifty/stream_soft_decoder.h>`). `hybrid` emits no preamble,
so `begin(dec, NULL, 0)` is fine.

### Soft output

The soft front end reports a `dt_soft_bit` per recovered position — a
graded consistency in `[0, 1]` for each hypothesis (not a probability split, so
the fields need not sum to 1):

| Field | Hypothesis |
|-------|-----------|
| `c_false` / `c_true` | the position holds `DT_FALSE` / `DT_TRUE` |
| `c_erasure` | the value is unrecoverable (`DT_ERASURE`) |
| `c_invalid`, `c_absent` | left **0** by `hybrid` (use [`maxir`](maxir.md) for these) |
| `c_locked` | the decoder is correctly tracking the stream — low during warm-up or after losing sync |

The hard symbol is the argmax projection over the alphabet (recoverability-first).

## Choosing `hybrid`

Pick `hybrid` for a drifting channel that also **overwrites** bits, when you want
a hard decision and/or soft consistencies and do **not** need `DT_INVALID`
round-tripping or sync re-acquisition. For the full output alphabet (poison and
absent consistencies) and re-acquisition after a sustained loss of lock, use
[`maxir`](maxir.md).
