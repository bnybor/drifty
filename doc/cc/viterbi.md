# drifty — `viterbi` decoder

A plain **Viterbi hard-decision** decoder over a convolutional code
(`dt_cc_code`). It is the simplest and fastest of the cc decoders: it corrects
flipped and erased bits, but does **not** track drift (insertions or deletions)
and takes **no channel-model parameters** — it is built from the code alone.

For the symbol alphabet (`DT_TRUE` / `DT_FALSE` / `DT_ERASURE` / `DT_INVALID` /
`DT_ABSENT`) and what each interface means, see
[Data-flow semantics](../data_flow_semantics.md).

## Algorithm

Viterbi finds the **single most likely transmitted path** through the code's
trellis (maximum-likelihood sequence estimation) and reads the information bits
off that path. Because alignment is assumed fixed (no drift), the trellis has one
axis — the encoder state — and the decision at each step is a hard symbol.

## What it corrects

| Channel effect | Handled? |
|----------------|----------|
| Substitution (flipped bit) | ✅ yes |
| Erasure (`DT_ERASURE` received) | ✅ yes — contributes no evidence either way |
| Insertion / deletion (drift) | ❌ no — assumes a fixed, known grid |
| `DT_INVALID` poison round-trip | ❌ not modelled |

If your channel inserts or drops bits, use [`vindel`](vindel.md),
[`hybrid`](hybrid.md), or [`maxir`](maxir.md) instead.

## API

```c
#include <drifty/cc/viterbi.h>

dt_decoder *dt_cc_viterbi_decoder_create(const dt_cc_code *code);
void        dt_cc_viterbi_decoder_destroy(dt_decoder *dec);
```

There are no channel-model parameters: the decoder is configured entirely by the
shared `dt_cc_code`. The code must outlive the decoder. To encode, use the
standalone encoder in `<drifty/cc/encoder.h>` (`dt_cc_encoder_*`).

## Driving the decoder

Drive the returned `dt_decoder` through its vtable (see `<drifty/decoder.h>`):

```c
dec->begin(dec, dst, dst_len);                       // once, first
dec->decode(dec, dst, dst_len, src, src_len);        // any number of times
dec->finalize(dec, dst, dst_len);                    // once, last
```

- **`src`** is the received transmit-domain stream; **`dst`** receives one
  output-domain symbol per recovered information position.
- **Warm-up delay.** A bit is committed only after a fixed look-ahead (the
  *decision depth*, ~`6 * dt_cc_code_k(code)` internally), so output trails input;
  the first ~decision-depth decoded bits are unreliable warm-up — discard them or
  skip a known preamble.
- **Buffering.** If a `decode` call returns exactly `dst_len`, the decoder has
  more buffered: call `decode` again with `src_len == 0` to drain before feeding
  more.

## Choosing `viterbi`

Pick `viterbi` when the channel only **flips** (and possibly **erases**) bits on a
**known, fixed grid**, and you want the cheapest hard-decision decoder. If you
need soft output on the same flip-only channel, use [`bcjr`](bcjr.md); if the
grid can drift, step up to [`vindel`](vindel.md) / [`hybrid`](hybrid.md) /
[`maxir`](maxir.md).
