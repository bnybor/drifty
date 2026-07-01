# 12 — pipe

Composing bit-stream plumbing with the [`pipe/`](../../doc/pipe/README.md) API.
Every other example drives a single codec through its vtable; `pipe/` is a small
toolkit for **wiring bit streams together**. Everything is built from three
interfaces over the same `dt_bit` / `dt_soft_bit` alphabet:

- **`dt_pipe_source`** — something you PULL a stream out of (`pull` / `soft_pull`).
- **`dt_pipe_sink`** — something you PUSH a stream into (`push` / `soft_push` /
  `finish`).
- **`dt_pipe`** — a buffered converter with *both* ends: push into its sink, tick
  it, pull from its source. Input and output are buffered and the work happens
  only in `tick()`, so the two ends are **not** directly chained. A pipe is driven
  `begin → (push / tick / pull)* → finalize`.

Concrete pipes wrap codecs, convert domains, route streams, or chain other pipes —
and because they all speak the same two faces, they compose freely.

## What it shows

- **Part A — endpoints + `dt_pipe_pump`.** `dt_pipe_buffer_source` / `_sink`
  ([buffers.h](../../include/drifty/pipe/buffers.h)) wrap a plain array with no
  allocation; `dt_pipe_pump(src, dst)` copies everything a source has into a sink.
- **Part B — one pipe's lifecycle.** A softening pipe (`dt_pipe_softening_create`)
  lifts hard bits to one-hot soft records. Pushing then pulling *before* a tick
  yields nothing — the input is buffered and only `tick()` converts it (no
  chaining).
- **Part C — a pipeline.** `dt_pipeline_create` chains an encoder pipe
  (`dt_pipe_encoder_create`) and a decoder pipe (`dt_pipe_decoder_create`) from
  [streams.h](../../include/drifty/pipe/streams.h) into one compound `dt_pipe` that
  round-trips a message. The compound is driven exactly like any single pipe.
- **Part D — fan-out.** `dt_pipe_splitter_create`
  ([multi.h](../../include/drifty/pipe/multi.h)) tees its input to its own output
  *and* a side sink, so a monitor can watch a stream without disturbing it.

## Run

```sh
./build/examples/12_pipe/12_pipe
```

## Expected output

```
Part A - a source, a sink, and dt_pipe_pump between them:
  pumped in              011000111000000110110011  (24 bits)
  landed out             011000111000000110110011  (24 bits)
  moved 24 bits; output matches input: yes

Part B - a single pipe: softening lifts hard bits to soft records.
Push into its sink, tick to convert, pull records from its source:
  pulled before tick: 0 records (buffered, not yet converted)
  after tick: 6 records - each hard bit is now one-hot soft:
    bit 0 -> c_true=0 c_false=1
    bit 0 -> c_true=0 c_false=1
    bit 1 -> c_true=1 c_false=0
    bit 0 -> c_true=0 c_false=1
    bit 0 -> c_true=0 c_false=1
    bit 0 -> c_true=0 c_false=1

Part C - a pipeline of codec pipes: [encoder | decoder] round-trips a
message. The compound is itself a dt_pipe, driven like any other:
  200 info bits -> pipeline -> 206 recovered bits, 0 errors

Part D - a splitter tees its input to its own output AND a side sink,
so a monitor can watch a stream without disturbing it:
  input                  01011110010100101111  (20 bits)
  main output            01011110010100101111  (20 bits)
  monitor copy           01011110010100101111  (20 bits)
  output and monitor both match the input: yes
```

## Reading it

- **Part A** is the whole model in miniature: a `dt_pipe_source` produces, a
  `dt_pipe_sink` consumes, and `dt_pipe_pump` drives one into the other until the
  source is dry. `dt_pipe_pump(src, NULL)` would instead drain and discard.
- **Part B** shows why a pipe has *two* buffers. `push` fills the input buffer;
  `pull` drains the output buffer; only `tick()` moves data across (converting on
  the way). That is why the pre-tick pull returns 0 — the two ends are decoupled.
  Each hard bit becomes a one-hot record (`DT_TRUE → c_true = 1`).
- **Part C** produces `206` recovered bits for `200` info bits and `0` errors: the
  extra 6 are the decoder's decision-depth tail, flushed by `finalize()`; the
  message interior is recovered exactly. Two codec pipes chained into a
  `dt_pipeline` are driven with a single `begin` / `tick` / `finalize` — the
  compound *is* a `dt_pipe`, so it nests inside larger graphs.
- **Part D** copies the input to three places at once — the splitter's own output
  and every side sink — matching hard and soft faces automatically. The diverter /
  selector / valve / drain in `multi.h` route the same way but choose *one*
  destination or source.

## See also

- [Pipe API (`doc/pipe/`)](../../doc/pipe/README.md) — the full toolkit: the three
  interfaces, the lifecycle, and the catalogue of pipe builders.
- [Streaming interface](../../doc/stream.md) — the `dt_stream_encoder` /
  `_decoder` / `_soft_decoder` vtables the codec pipes wrap.
- [01 basics](../01_basics) — the same encode/decode round trip driven directly,
  without pipes.
