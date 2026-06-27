# drifty — `bcjr` decoder

A **BCJR** (MAP / forward–backward) decoder over a `dt_cc_code`, with both a
**hard-decision** and a **soft-output** front end. Where
[`viterbi`](viterbi.md) finds the single most likely path, BCJR computes the
per-bit a-posteriori *probability* of each information bit, which makes it a
natural soft-output decoder. Like `viterbi`, it corrects flipped and erased bits
but does **not** track drift.

For the symbol alphabet and what each interface means, see
[Data-flow semantics](../data_flow_semantics.md).

## Algorithm

BCJR runs a forward and a backward recursion over the code trellis and combines
them across a sliding window (`decision_depth`) to compute, for each information
bit, the a-posteriori probability of 0 vs 1 (and the erasure/poison hypotheses).
Alignment is assumed fixed — there is no drift axis — so the trellis has a single
state dimension.

> **Independent from [`maxir`](maxir.md).** Both are forward–backward decoders,
> but `bcjr` is the flip/erasure-only variant and `maxir` is the drift-tolerant,
> full-channel-model one. They are separate implementations — do not assume a
> change to one applies to the other.

## What it corrects

| Channel effect | Handled? |
|----------------|----------|
| Substitution (flipped bit) | ✅ yes (`p_flip`) |
| Erasure (`DT_ERASURE` received) | ✅ yes (`p_erase`) |
| Insertion / deletion (drift) | ❌ no — assumes a fixed, known grid |
| Overwrite to a fixed value | ❌ not modelled |

If your channel inserts or drops bits, use [`maxir`](maxir.md) (same
soft-output style, drift-tolerant) or [`vindel`](vindel.md) /
[`hybrid`](hybrid.md).

## API

```c
#include <drifty/cc/bcjr.h>

typedef struct {
  int   decision_depth;
  float p_flip;
  float p_erase;
} dt_cc_bcjr_stream_params;

/* hard decision */
dt_stream_decoder      *dt_cc_bcjr_decoder_create(const dt_cc_code *code,
                                           const dt_cc_bcjr_stream_params *params);
void             dt_cc_bcjr_decoder_destroy(dt_stream_decoder *dec);

/* soft output */
dt_stream_soft_decoder *dt_cc_bcjr_soft_decoder_create(const dt_cc_code *code,
                                                const dt_cc_bcjr_stream_params *params);
void             dt_cc_bcjr_soft_decoder_destroy(dt_stream_soft_decoder *dec);
```

Both factories take the same `params` (copied; need not outlive the call). The
code must outlive everything built from it. To encode, use the standalone encoder
in `<drifty/cc/encoder.h>`.

## Channel-model parameters

Use designated initializers; any field left out is 0. Rough probabilities are
fine — only their relative sizes matter.

| Field | Meaning |
|-------|---------|
| `decision_depth` | Output delay in bits — the sliding window the backward recursion spans. Bigger is more reliable but slower to emit. Try ~`6 * dt_cc_code_k()`. **Required (≥ 1).** |
| `p_flip` | How often a coded bit is flipped, `0 < p_flip < 1` (e.g. `0.01` for 1%). Sets the branch likelihoods. **Required.** |
| `p_erase` | How often a received bit is `DT_ERASURE`. `0` (default) if you never mark erasures. |

## Driving the decoder

Hard and soft front ends are driven the same way — `begin` → `decode` (repeat) →
`finalize` — sharing the warm-up delay (first ~`decision_depth` bits are
unreliable) and the buffering rule (a `decode` returning exactly `dst_len` has
more buffered — call again with `src_len == 0` to drain). See `<drifty/decoder.h>`
and `<drifty/soft_decoder.h>`.

### Soft output

`bcjr` populates the **full** `dt_stream_soft_decoder_out` alphabet. Each field is a
graded consistency in `[0, 1]` (not a probability split, so the fields need not
sum to 1):

| Field | Hypothesis |
|-------|-----------|
| `c_false` / `c_true` | the position holds `DT_FALSE` / `DT_TRUE` |
| `c_erasure` | the value is unrecoverable (`DT_ERASURE`) |
| `c_invalid` | a recovered non-value (`DT_INVALID`) |
| `c_absent` | the position was deleted or not synchronized (`DT_ABSENT`) |
| `c_locked` | the decoder is correctly tracking the stream — low during warm-up |

The hard symbol is the argmax projection over the alphabet (recoverability-first).

## Choosing `bcjr`

Pick `bcjr` when the channel only **flips** and **erases** bits on a **fixed
grid** and you want **soft** per-bit information (e.g. to feed an outer
soft-input code). For a hard decision on the same channel, [`viterbi`](viterbi.md)
is lighter; if the grid can drift, use [`maxir`](maxir.md).
