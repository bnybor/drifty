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
  `_soft_decoder`: fixed-size `(k, n)` blocks. The [`rs251`](bc/rs251.md) codec
  implements it.
- **[Frame interface](frame.md)** — `dt_frame_encoder` / `_decoder` /
  `_soft_decoder`: a stream split into delimited frames with uncoded passthrough.
  The [`fc/`](fc/README.md) codecs implement it.
- **[Convolutional coding (`cc/`)](cc/README.md)** — the convolutional codecs: the
  [shared code](cc/ccode.md), the shared encoder, and the five decoders
  (`viterbi`, `vindel`, `hybrid`, `maxir`, `bcjr`) plus the `detect_clean` /
  `detect_noisy` meta-codecs (blind code-presence detection — cheap vs
  noise-tolerant), with per-codec references and a guide to choosing one.
- **[Block coding (`bc/`)](bc/rs251.md)** — block-code implementations; currently
  `rs251`, a Reed–Solomon RS(n, k) code over GF(251) (hard and soft decoders).
- **[Frame coding (`fc/`)](fc/README.md)** — frame-delimiting codecs that carry no
  error correction of their own: `naive` (fixed-length frames) and `marker`
  (variable-length, escape-delimited), each with a hard and a soft decoder.
- **[Bit-stream pipes (`pipe/`)](pipe/README.md)** — a composable toolkit for
  wiring bit streams: the `dt_pipe_source` / `dt_pipe_sink` / `dt_pipe` interfaces,
  the buffered `begin`/`tick`/`finalize` lifecycle, `dt_pipe_pump`, and a catalogue
  of pipe builders (stream- and frame-codec adapters, hard/soft converters,
  `dt_pipeline`, and the splitter / diverter / selector / valve routing pipes).
- **[Object container (`container.h`)](../include/drifty/container.h)** —
  `dt_container`, a small ownership bag that holds `(object, destroyer)` pairs and
  frees them all in reverse order with one `dt_container_destroy()`, so a routine that
  builds many `_create`d objects can register each as it goes instead of trailing a
  long list of individual destroy calls down every exit path.
- **[Freestanding & embedded](freestanding.md)** — running the core without a
  hosted C library: the `dt_*` proxy boundary, the bare vs full archives, and how
  to port the proxies (allocation, math) to a bare-metal target.
- **[Soft decoding](soft_decoding.md)** — the soft front ends (`bcjr`, `hybrid`,
  `maxir`): what the per-bit consistencies mean, how `c_absent` differs across
  decoders, and using soft output across the inner→outer boundary.
- **[Worked example: concatenation](concatenated.md)** — a complete runnable
  program stacking the layers: `rs251` outer over a `hybrid` soft inner across a
  drift channel (hard path alongside for contrast), showing the soft front end and
  the inner→outer erasure bridge end to end.

Operations that can fail return the shared `dt_result` codes from
[`result.h`](../include/drifty/result.h).

## Source layout

| Path | What |
|------|------|
| `include/drifty/` | top-level public headers: the `dt_bit` alphabet (`bit.h`) and soft bit (`soft_bit.h`), shared result codes (`result.h`), the `dt_container` ownership helper (`container.h`), and the streaming / block / frame codec interfaces |
| `include/drifty/cc/` | convolutional codec API (`ccode.h`, `encoder.h`, `viterbi.h`, `vindel.h`, `hybrid.h`, `maxir.h`, `bcjr.h`, `detect_clean.h`, `detect_noisy.h`) |
| `include/drifty/bc/` | block-code API (`rs251.h` — Reed–Solomon over GF(251)) |
| `include/drifty/fc/` | frame-code API (`naive.h` — fixed-length frames; `marker.h` — escape-delimited frames) |
| `include/drifty/pipe/` | bit-stream pipe API (`source.h`, `sink.h`, `pipe.h`, `buffers.h`, `streams.h`, `frames.h`, `pipes.h`, `multi.h`) |
| `src/cc/` | convolutional implementations — the shared `encoder/`, the `ccode` descriptor, and each codec's decode engine |
| `src/bc/` | the `rs251` block-codec adapter over the bundled `contrib/rs251` Reed–Solomon library |
| `src/fc/` | frame-codec implementations (`naive`, `marker`) |
| `src/pipe/` | pipe implementations — converters and the executor (`pipes.c`), codec adapters (`streams.c`), the pipeline (`pipeline.c`), and the multi-way routing pipes (`multi.c`) |
