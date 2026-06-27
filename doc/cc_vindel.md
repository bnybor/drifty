# drifty — `vindel` decoder

A **drift-tolerant Viterbi** decoder over a convolutional code (`dt_cc_code`).
It corrects flipped and erased bits like [`viterbi`](cc_viterbi.md), and in
addition tracks **drift** — inserted and dropped coded bits — recovering the
alignment as it decodes. It is a hard-decision decoder (no soft output).

For the symbol alphabet and what each interface means, see
[Data-flow semantics](data_flow_semantics.md).

## Algorithm

`vindel` extends the Viterbi trellis with a second axis: as well as the encoder
**state**, each path tracks its **drift** (net insertions − deletions) within a
bounded window. The most likely path through this state×drift trellis jointly
resolves *where* each bit is and *what* it is, so the decoder absorbs insertions
and deletions and emits a clean, fixed grid of information positions.

## What it corrects

| Channel effect | Handled? |
|----------------|----------|
| Substitution (flipped bit) | ✅ yes (`p_sub`) |
| Erasure (`DT_ERASURE` received) | ✅ yes (`p_erase`) |
| Insertion / deletion (drift) | ✅ yes, up to `max_drift` (`p_ins` / `p_del`) |
| Overwrite to a fixed value | ❌ not modelled — see [`hybrid`](cc_hybrid.md) / [`maxir`](cc_maxir.md) |
| `DT_INVALID` poison round-trip | ❌ not modelled — see [`maxir`](cc_maxir.md) |

## API

```c
#include <drifty/cc/vindel.h>

typedef struct {
  int   decision_depth;
  int   max_drift;
  float p_sub;
  float p_ins;
  float p_del;
  float p_erase;
} dt_cc_vindel_stream_params;

dt_decoder *dt_cc_vindel_decoder_create(const dt_cc_code *code,
                                        const dt_cc_vindel_stream_params *params);
void        dt_cc_vindel_decoder_destroy(dt_decoder *dec);
```

`params` is copied and need not outlive the call. The code must outlive the
decoder. To encode, use the standalone encoder in `<drifty/cc/encoder.h>`.

## Channel-model parameters

Use designated initializers; any field left out is 0. Rough probabilities are
fine — only their relative sizes matter.

| Field | Meaning |
|-------|---------|
| `decision_depth` | Output delay in bits before each bit is committed. Bigger is more reliable but slower to emit. Try ~`6 * dt_cc_code_k()`. **Required (≥ 1).** |
| `p_sub` | How often a received bit is flipped, `0 < p_sub < 1` (e.g. `0.01` for 1%). **Required.** |
| `max_drift` | How far alignment may slip before the decoder loses track. `0` (default) corrects flips only; `4`–`8` also recovers from insertions and deletions. |
| `p_ins`, `p_del` | How often a coded bit is inserted / dropped, per bit (`p_ins + p_del < 1`). **Required when `max_drift > 0`**; leave 0 otherwise. |
| `p_erase` | How often a received bit is `DT_ERASURE`. `0` (default) if you never mark erasures. |

## Driving the decoder

Identical to every cc decoder — drive the `dt_decoder` vtable through
`begin` → `decode` (repeat) → `finalize` (see `<drifty/decoder.h>`). Mind the
**warm-up delay** (first ~`decision_depth` bits are unreliable) and the
**buffering** rule (a `decode` returning exactly `dst_len` has more buffered —
call again with `src_len == 0` to drain). Deletions surface on output as
`DT_ABSENT`; insertions simply disappear from the restored grid.

## Choosing `vindel`

Pick `vindel` for a drifting channel (insertions/deletions plus flips/erasures)
when a **hard** decision is enough and the channel has no fixed-value overwrites.
If you need **soft** output, or the channel overwrites bits, use
[`hybrid`](cc_hybrid.md); if you also need `DT_INVALID` round-tripping or
re-acquisition after a sustained loss of lock, use [`maxir`](cc_maxir.md).
