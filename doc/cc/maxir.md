# drifty — `maxir` decoder

The most capable cc decoder: a **drift-tolerant max-log-MAP**
(forward–backward) decoder over a `dt_cc_code`, with both **hard-decision** and
**soft-output** front ends. Where [`viterbi`](viterbi.md) finds the single most
likely path, `maxir` computes a per-bit a-posteriori *weight* for each
information bit. It corrects flips and erasures, tracks drift, models the same
rich channel as [`hybrid`](hybrid.md), surfaces the full output alphabet
(including `DT_INVALID` poison and `DT_ABSENT`), and **re-acquires sync** after a
sustained loss of lock.

For the symbol alphabet and what each interface means, see
[Data-flow semantics](../data_flow_semantics.md).

## Algorithm

`maxir` runs a max-log-MAP (a max-log approximation of BCJR) forward–backward
recursion over a state×drift super-trellis: the forward and backward passes meet
across a sliding window (`decision_depth`) to weigh every hypothesis for each
restored information position. Decoding jointly recovers alignment (absorbing
insertions/deletions) and value, and the soft weights populate every
output-domain hypothesis.

> **Independent from [`bcjr`](bcjr.md).** Both are forward–backward decoders,
> but `maxir` is the drift-tolerant, full-channel-model variant and `bcjr` is the
> flip/erasure-only variant. They are separate implementations — do not assume a
> change to one applies to the other.

## What it corrects

| Channel effect | Handled? |
|----------------|----------|
| Substitution (flipped bit) | ✅ yes (`p_flip`) |
| Erasure (`DT_ERASURE`) | ✅ yes (`p_ovr_erase` doubles as the plain erasure rate) |
| Insertion / deletion (drift) | ✅ yes, up to `max_drift` |
| Value-biased insertion | ✅ yes (`p_ins_true` / `p_ins_false` / `p_ins_erase`) |
| Overwrite to fixed value | ✅ yes (`p_ovr_true` / `p_ovr_false` / `p_ovr_erase`) |
| `DT_INVALID` poison round-trip | ✅ yes — recovered as `DT_INVALID` |
| Re-acquisition after sustained loss of lock | ✅ yes |

## API

```c
#include <drifty/cc/maxir.h>

typedef struct {
  int   decision_depth;
  int   max_drift;
  float p_flip;
  float p_ins_true, p_ins_false, p_ins_erase;
  float p_del;
  float p_ovr_true, p_ovr_false, p_ovr_erase;
} dt_cc_maxir_stream_params;

/* hard decision */
dt_stream_decoder      *dt_cc_maxir_decoder_create(const dt_cc_code *code,
                                            const dt_cc_maxir_stream_params *params);
void             dt_cc_maxir_decoder_destroy(dt_stream_decoder *dec);

/* soft output */
dt_stream_soft_decoder *dt_cc_maxir_soft_decoder_create(const dt_cc_code *code,
                                                 const dt_cc_maxir_stream_params *params);
void             dt_cc_maxir_soft_decoder_destroy(dt_stream_soft_decoder *dec);
```

Both factories take the same `params` (copied; need not outlive the call). The
code must outlive everything built from it. To encode, use the standalone encoder
in `<drifty/cc/encoder.h>`.

## Channel-model parameters

Use designated initializers; any field left out is 0. Rough probabilities are
fine — only their relative sizes matter. (The fields match
[`hybrid`](hybrid.md)'s model.)

| Field | Meaning |
|-------|---------|
| `decision_depth` | Output delay in bits — the sliding window the backward recursion spans. Try ~`6 * dt_cc_code_k()`. **Required (≥ 1).** |
| `p_flip` | How often a coded bit is flipped, `0 < p_flip < 1`. **Required.** |
| `max_drift` | How far alignment may slip before the decoder loses track. `0` (default) corrects flips only; `4`–`8` recovers insertions/deletions. |
| `p_ins_true`, `p_ins_false`, `p_ins_erase` | How often a spurious `DT_TRUE` / `DT_FALSE` / `DT_ERASURE` bit is inserted. Their sum is the overall insertion rate; the split biases the expected inserted value. **Required when `max_drift > 0`.** |
| `p_del` | How often a coded bit is dropped. Insertion rates and `p_del` must sum to `< 1`. **Required when `max_drift > 0`.** |
| `p_ovr_true`, `p_ovr_false`, `p_ovr_erase` | How often a coded bit is overwritten with a fixed value regardless of what was sent. The three must sum to `< 1`; all 0 if there are no overwrites. `p_ovr_erase` doubles as the plain erasure rate. |

## Driving the decoder

Hard and soft front ends are driven the same way — `begin` → `decode` (repeat) →
`finalize` — sharing the warm-up delay and buffering rule (see
`<drifty/stream_decoder.h>` and `<drifty/stream_soft_decoder.h>`).

### Soft output

`maxir` populates the **full** `dt_stream_soft_decoder_out` alphabet (unlike `hybrid`,
which leaves `c_invalid` / `c_absent` at 0). Each field is a graded consistency in
`[0, 1]` (not a probability split):

| Field | Hypothesis |
|-------|-----------|
| `c_false` / `c_true` | the position holds `DT_FALSE` / `DT_TRUE` |
| `c_erasure` | the value is unrecoverable (`DT_ERASURE`) |
| `c_invalid` | the coded support arrived as the encoder's poison — a recovered non-value (`DT_INVALID`) |
| `c_absent` | the position was deleted or not synchronized (`DT_ABSENT`) |
| `c_locked` | the decoder is correctly tracking the stream — low during warm-up or after losing sync |

The hard symbol is the argmax projection over the alphabet (recoverability-first).

## Choosing `maxir`

Pick `maxir` when you need the strongest decoder: a drifting, overwriting channel,
soft information across the full output alphabet, `DT_INVALID` round-tripping, or
recovery after a sustained loss of lock. If the channel is flip/erasure-only on a
fixed grid, the lighter [`bcjr`](bcjr.md) gives the same soft-output style
without the drift machinery; for a hard-only drifting channel without overwrites,
[`vindel`](vindel.md) is cheaper.
