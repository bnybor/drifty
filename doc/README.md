# drifty — Documentation

drifty is a drift-tolerant convolutional coding library: it turns the
alignment-axis mess of a noisy channel (flips, erasures, insertions, deletions)
into clean value-axis facts (values plus erasures) on a stable grid, so an outer
code never has to reason about position.

## Contents

- **[Data-flow semantics](data_flow_semantics.md)** — the symbol model. What the
  `DT_` alphabet means and what each pipeline stage (source → encoder → channel →
  decoder → consumer) consumes and produces. Read this first; everything else
  assumes its vocabulary.

- **[Convolutional coding (`cc/`)](cc/README.md)** — the convolutional codecs: the
  shared encoder and the five decoders (`viterbi`, `vindel`, `hybrid`, `maxir`,
  `bcjr`), with per-codec API references and a guide to choosing one.

## Source layout

| Path | What |
|------|------|
| `include/drifty/` | public headers — `bit.h`, the streaming interfaces `stream_encoder.h` / `stream_decoder.h` / `stream_soft_decoder.h`, the block-interface stubs `block_encoder.h` / `block_decoder.h` / `block_soft_decoder.h` (reserved; not yet implemented), and `cc/` |
| `include/drifty/cc/` | per-codec public API (`ccode.h`, `encoder.h`, `viterbi.h`, `vindel.h`, `hybrid.h`, `maxir.h`, `bcjr.h`) |
| `src/cc/` | implementations — the shared `encoder/`, the `ccode` descriptor, and each codec's decode engine |
