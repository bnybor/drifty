# drifty — frame interface

The frame codec interface: a continuous stream split into delimited **frames**,
with bits *between* frames passing through uncoded. It is the streaming interface
plus frame delimiters — protected payload framed, unprotected headers or sync
verbatim. Three function-pointer vtables over the `dt_bit` alphabet
([symbols](bit.md)):

- `dt_frame_encoder` ([`frame_encoder.h`](../include/drifty/frame_encoder.h))
- `dt_frame_decoder` ([`frame_decoder.h`](../include/drifty/frame_decoder.h))
- `dt_frame_soft_decoder`
  ([`frame_soft_decoder.h`](../include/drifty/frame_soft_decoder.h)) — the frame
  decoder in fully soft bits (`dt_soft_bit` ([symbols](bit.md)) throughout, no hard
  bits). It reframes a soft stream — the middle stage between a
  [`stream_soft_decoder`](stream.md) and a [`block_soft_decoder`](block.md) — and
  shares the same `get_state()` frame-state machine.

The frame codecs in [`fc/`](fc/README.md) implement these interfaces:
[`naive`](fc/naive.md) (fixed-length frames) and [`marker`](fc/marker.md)
(variable-length, escape-delimited).

## Encoder

The encoder drives the frame boundaries. Beyond `begin` and `finalize` it adds
`begin_frame` / `end_frame` to bracket a frame; `encode` codes bits inside a frame
and copies them verbatim outside one:

```
begin -> [ encode (verbatim) | begin_frame -> encode* -> end_frame ]* -> finalize
```

`begin_frame` and `end_frame` pair up and do not nest. Every call writes into `dst`
and returns the number of bits written, or a negative value on error.

## Decoder

The decoder *discovers* the boundaries the encoder wrote and reports them through a
state it exposes via `get_state()`:

```
OUTSIDE -> (BEGIN -> INSIDE -> END)* -> OUTSIDE
```

(`dt_frame_decoder_state`: `OUTSIDE` / `BEGIN` / `INSIDE` / `END`; `OUTSIDE` and
`INSIDE` are sustained, `BEGIN` and `END` momentary.) Inside a frame the received
bits are decoded and corrected; outside one they pass through verbatim — the
inverse of the encoder's split.

`decode()` advances until **either** the next state transition **or** `dst` fills,
whichever comes first, and returns the bits written. Because a full buffer can stop
a call before a transition, check `get_state()` after *every* `decode()` rather
than assuming a boundary was reached; as with the streaming decoder, a `decode()`
that returns exactly `dst_len` has more buffered (call again with `src_len` 0 to
drain).

## See also

- [Frame coding (`fc/`)](fc/README.md) — the codecs that implement this interface
  ([`naive`](fc/naive.md), [`marker`](fc/marker.md)), and a guide to choosing one.
- [Streaming interface](stream.md) — frames are the streaming interface plus
  delimiters.
- [Symbols](bit.md) — the `dt_bit` alphabet, including the `DT_ABSENT` that an
  upstream decoder's output can carry into a frame decoder's input.
