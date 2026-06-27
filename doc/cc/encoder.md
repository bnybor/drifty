# drifty — encoder

The one convolutional **encoder** every cc codec encodes through. It is
standalone and code-only — it depends on the shared `dt_cc_code`, not on any
codec — so the same coded stream feeds whichever decoder
([`viterbi`](viterbi.md), [`vindel`](vindel.md), [`hybrid`](hybrid.md),
[`maxir`](maxir.md), or [`bcjr`](bcjr.md)) shares the code. The *decoder* is what
differs between codecs; the encoder is common to all of them.

For the symbol alphabet (`DT_TRUE` / `DT_FALSE` / `DT_ERASURE` / `DT_INVALID`) and
what each interface means, see [Data-flow semantics](../data_flow_semantics.md).

## What it does

It expands an information stream into a longer, redundant coded stream by the
convolutional code: each input bit drives the code's shift register and produces
`dt_cc_code_n(code)` coded output bits. For an all-boolean message the output is
exactly the plain convolutional code.

Beyond plain bits, it carries each input position's **semantics** through to the
coded stream:

| Input symbol | Coded output |
|--------------|--------------|
| `DT_TRUE` / `DT_FALSE` | the code's bound booleans for that step |
| `DT_INVALID` (a deliberate non-value) | the coded positions that depend on it are emitted `DT_INVALID` — *structural poison*, landing exactly on those positions and nowhere else |
| `DT_ERASURE` (a don't-care) | the coded positions that depend on it are emitted `DT_ERASURE` — *deferred* to the channel to resolve |

Only the soft/forward–backward decoders ([`maxir`](maxir.md), [`bcjr`](bcjr.md))
read the `DT_INVALID` poison back out; for plain boolean messages none of this
machinery is engaged and every codec sees the same coded bits.

> **Encoder output is not decoder input** — the channel lies between them. Meaning
> that depends on which side you are on (most importantly the don't-care vs
> don't-know readings of `DT_ERASURE`) must not be carried across as the same fact.

## API

```c
#include <drifty/cc/encoder.h>

dt_encoder *dt_cc_encoder_create(const dt_cc_code *code);
void        dt_cc_encoder_destroy(dt_encoder *enc);
```

`dt_cc_encoder_create` returns a `dt_encoder` over `code` (NULL on a bad argument
or out of memory). The code must outlive the encoder. The matching decoder is
built from the same `dt_cc_code` with that codec's `dt_cc_<codec>_decoder_create`.

## Driving the encoder

Drive the returned `dt_encoder` through its vtable (see `<drifty/encoder.h>`):

```c
dt_encoder *enc = dt_cc_encoder_create(code);

int n = enc->begin(enc, dst, dst_len);                 // once, first
n += enc->encode(enc, dst + n, dst_len - n, src, src_len);  // any number of times
n += enc->finalize(enc, dst + n, dst_len - n);         // once, last

dt_cc_encoder_destroy(enc);
```

- **`begin`** — initialise and write any preamble. This encoder emits none, so
  `begin(enc, NULL, 0)` is fine.
- **`encode`** — append the coded bits for `src` (one transmit-domain symbol per
  information position) to `dst`. Call as many times as you like; the encoder
  carries its shift-register state across calls, so a message may be encoded in
  any chunking.
- **`finalize`** — flush the tail: it feeds `K-1` terminating groups that drain
  the register back to state 0, so the decoder can recover the last bits cleanly.
  Call once, at end of stream.

Every call writes into `dst` (capacity `dst_len`) and returns the number of coded
bits written, or a negative `DT_CC_ERR_*` code on a bad argument — most often too
little room.

### Sizing the output

`n` information bits become **up to `(n + K) * dt_cc_code_n(code)`** coded bits
once the flush tail is counted (`K = dt_cc_code_k(code)`). Size `dst` from the
input length and `dt_cc_code_n(code)` accordingly.

## Notes

- The one-shot engine entry points behind the vtable
  (`dt_cc_encoder_encode` / `dt_cc_encoder_flush`, declared in
  `src/cc/encoder/encode.h`) are **private** — only the
  `dt_cc_encoder_create` / `_destroy` factories are public API.
- There is exactly one encoder implementation (`src/cc/encoder/`); the per-codec
  and earlier "basic" encoders were removed as redundant.
