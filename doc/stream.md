# drifty — streaming interface

The streaming codec interface: a continuous bit stream, fed and drained
incrementally at a fixed delay, with no message lengths or block boundaries to
manage. Three function-pointer vtables over the `dt_bit` alphabet
([symbols](bit.md)):

- `dt_stream_encoder` ([`stream_encoder.h`](../include/drifty/stream_encoder.h)) —
  input bits → coded bits.
- `dt_stream_decoder` ([`stream_decoder.h`](../include/drifty/stream_decoder.h)) —
  received bits → recovered bits (a hard decision).
- `dt_stream_soft_decoder`
  ([`stream_soft_decoder.h`](../include/drifty/stream_soft_decoder.h)) — the same
  as the decoder, but each recovered position is a `dt_soft_bit` of consistencies
  rather than one hard bit.

The convolutional [`cc/`](cc/README.md) codecs implement this interface: build a
decoder with a codec's `dt_cc_<codec>_decoder_create` factory (and the soft
decoder where the codec offers one), and encode through the shared
`dt_cc_encoder_create`. Then drive the returned vtable.

## Phases

Drive each instance through three phases:

| Phase | Encoder | Decoder |
|-------|---------|---------|
| `begin` | once, first — write any preamble | once, first — consume any preamble |
| `encode` / `decode` | any number of times — append coded bits for `src` | any number of times — feed received bits, out come recovered bits |
| `finalize` | once, last — flush the terminating tail | once, last — drain bits still in flight |

Every call writes into `dst` (capacity `dst_len`) and returns the number of bits
written, or a negative value on a bad argument (most often too little room).
`data` is the implementation's private state — do not touch it.

## `src` and `dst`

- The encoder's `src` is one transmit-domain symbol per information position
  (`DT_TRUE` / `DT_FALSE` to protect, `DT_ERASURE` for a deferred don't-care,
  `DT_INVALID` for a deliberate non-value); its `dst` is the coded stream.
- The decoder's `src` is the received transmit-domain stream on an unknown,
  drifting grid; its `dst` is one output-domain symbol per recovered position
  (adding `DT_ABSENT`). The soft decoder writes a `dt_soft_bit` per position
  instead.

See [Data-flow semantics](data_flow_semantics.md) for the full symbol treatment.

## Two decoder behaviours

- **Warm-up delay.** A bit is committed only after a fixed look-ahead (the decision
  depth), so output trails input and the first ~decision-depth decoded bits are
  unreliable warm-up — discard them, or send a known preamble you skip.
- **Buffering.** The decoder keeps recovered bits that don't fit in `dst` rather
  than dropping them. When a `decode` call returns exactly `dst_len` (it filled the
  buffer), call `decode` again with no new input (`src_len` 0) to drain the rest
  before feeding more.

The soft decoder behaves identically — same phases, warm-up, and buffering — but
its `decode` / `finalize` write `dt_soft_bit` records.

## See also

- [`cc/`](cc/README.md) — the codecs that implement this interface, and a guide to
  choosing one.
- [Block interface](block.md) — the fixed-size-block alternative.
- [Frame interface](frame.md) — the streaming interface split into delimited
  frames.
