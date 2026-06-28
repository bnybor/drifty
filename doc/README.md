# drifty — Documentation

drifty is a drift-tolerant convolutional coding library: it turns the
alignment-axis mess of a noisy channel (flips, erasures, insertions, deletions)
into clean value-axis facts (values plus erasures) on a stable grid, so an outer
code never has to reason about position.

## Contents

The model and the symbol API first, then the three interface framings, then the
codecs that implement them.

- **[Data-flow semantics](data_flow_semantics.md)** — the symbol model: what the
  `DT_` alphabet means and what each pipeline stage (source → encoder → channel →
  decoder → consumer) consumes and produces. Read this first; everything else
  assumes its vocabulary.
- **[Symbols (`bit`)](bit.md)** — the C API for the alphabet: the hard `dt_bit`,
  the soft `dt_soft_bit`, their values, and the predicate macros.
- **[Streaming interface](stream.md)** — `dt_stream_encoder` / `_decoder` /
  `_soft_decoder`: a continuous bit stream at a fixed delay. The
  [`cc/`](cc/README.md) codecs implement it.
- **[Block interface](block.md)** — `dt_block_encoder` / `_decoder` /
  `_soft_decoder`: fixed-size `(k, n)` blocks. The [`rs251`](rs/rs251.md) codec
  implements it.
- **[Frame interface](frame.md)** — `dt_frame_encoder` / `_decoder`: a stream split
  into delimited frames with uncoded passthrough. Defined but not yet implemented.
- **[Convolutional coding (`cc/`)](cc/README.md)** — the convolutional codecs: the
  shared encoder and the five decoders (`viterbi`, `vindel`, `hybrid`, `maxir`,
  `bcjr`), with per-codec references and a guide to choosing one.
- **[Reed–Solomon coding (`rs/`)](rs/rs251.md)** — the `rs251` block codec, an
  RS(n, k) outer code over GF(251).

Operations that can fail return the shared `dt_result` codes from
[`result.h`](../include/drifty/result.h).

## Source layout

| Path | What |
|------|------|
| `include/drifty/` | top-level public headers: the `dt_bit` alphabet (`bit.h`) and soft bit (`soft_bit.h`), shared result codes (`result.h`), and the streaming / block / frame codec interfaces |
| `include/drifty/cc/` | convolutional codec API (`ccode.h`, `encoder.h`, `viterbi.h`, `vindel.h`, `hybrid.h`, `maxir.h`, `bcjr.h`) |
| `include/drifty/rs/` | Reed–Solomon block codec API (`rs251.h`) |
| `src/cc/` | convolutional implementations — the shared `encoder/`, the `ccode` descriptor, and each codec's decode engine |
| `src/rs/` | the `rs251` block-codec adapter over the bundled `contrib/rs251` Reed–Solomon library |
