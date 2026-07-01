# drifty — bit-stream pipes (`pipe/`)

The `pipe/` API is a small, composable toolkit for **wiring bit streams
together**. Where the [streaming codecs](../stream.md) are each driven directly
through their own vtable, a *pipe* is a uniform building block you can chain,
tee, gate, convert, and route — all over the same `dt_bit` / `dt_soft_bit`
[alphabet](../bit.md). Encoding, decoding, hard/soft conversion, fan-out, and
buffering are all just pipes, so a whole processing graph is assembled from one
kind of part.

It is **header-only where it can be** (the endpoints in
[`buffers.h`](../../include/drifty/pipe/buffers.h) allocate nothing) and
freestanding like the rest of drifty — the growable pipes use the `dt_malloc`
[proxy boundary](../freestanding.md).

## The three interfaces

Everything is one of three vtable types (each carries a private `data` pointer;
build with a factory, call through the function pointers):

| Type | Header | You… | Faces |
|------|--------|------|-------|
| **`dt_pipe_source`** | [source.h](../../include/drifty/pipe/source.h) | PULL a stream out | `pull` (hard), `soft_pull` (soft) |
| **`dt_pipe_sink`** | [sink.h](../../include/drifty/pipe/sink.h) | PUSH a stream in | `push` (hard), `soft_push` (soft), `finish` |
| **`dt_pipe`** | [pipe.h](../../include/drifty/pipe/pipe.h) | push in *and* pull out | `source`, `sink`, `begin`, `tick`, `finalize` |

A source and a sink each have a **hard face** (`dt_bit`) and a **soft face**
(`dt_soft_bit`). An implementation fills the face(s) it supports and leaves the
other NULL (a single-domain buffer endpoint), or makes it a no-op that yields
nothing (a converter pipe). Each call returns the number of elements moved, `0`
at end of stream, or a negative value on error.

```c
struct dt_pipe_source_t {
  int (*pull)(dt_pipe_source *, dt_bit *dst, size_t dst_len);
  int (*soft_pull)(dt_pipe_source *, dt_soft_bit *dst, size_t dst_len);
  void *data;
};
struct dt_pipe_sink_t {
  int (*push)(dt_pipe_sink *, const dt_bit *src, size_t src_len);
  int (*soft_push)(dt_pipe_sink *, const dt_soft_bit *src, size_t src_len);
  int (*finish)(dt_pipe_sink *);
  void *data;
};
```

## The pipe lifecycle

A `dt_pipe` is a **buffered converter with two ends**. You push a stream into its
sink and pull it back out of its source, but the ends are **not directly
chained**: input is buffered, output is buffered, and the pipe's work happens
only in `tick()`, which consumes the buffered input and produces the buffered
output. Nothing pushed reaches the source until a tick moves it across.

```c
struct dt_pipe_t {
  dt_pipe_source *(*source)(dt_pipe *);   // read end
  dt_pipe_sink   *(*sink)(dt_pipe *);     // write end
  int (*begin)(dt_pipe *);                // prepare to run
  int (*tick)(dt_pipe *);                 // consume input -> produce output
  int (*finalize)(dt_pipe *);             // flush what is still in flight
  void *data;
};
```

Drive it through three phases — `begin`, then interleave push / tick / pull, then
`finalize`. `begin` and `finalize` are *specialized ticks*: they run the same
work while also handling any preamble (begin) or trailing flush (finalize), which
matters when a pipe wraps a codec.

```c
dt_pipe *p = dt_pipe_encoder_create(enc);
dt_pipe_sink   *in  = p->sink(p);
dt_pipe_source *out = p->source(p);

p->begin(p);
in->push(in, info, n);          // buffer input
p->tick(p);                     // do the work (here: encode)
dt_bit coded[512];
int got = out->pull(out, coded, 512);
p->finalize(p);                 // flush the codec's trailer / tail
dt_pipe_encoder_destroy(p);
```

## `dt_pipe_pump` — the move primitive

`int dt_pipe_pump(dt_pipe_source *src, dt_pipe_sink *dst)` copies everything a
source has into a sink, forwarding whichever face carries data (hard or soft) to
the matching face of the sink. A face NULL on either end is skipped; a **NULL
`dst` drains the source and drops the bits** (a "to /dev/null"). It returns the
number of elements moved, or a negative value on error. This is the primitive the
compound and routing pipes use internally, and it is public for moving data
across an external source→sink boundary in one call.

## Endpoints — [`buffers.h`](../../include/drifty/pipe/buffers.h)

Concrete source/sink endpoints over a caller-provided array. They **allocate
nothing**: declare one on the stack, init it over your buffer, and use the
returned interface pointer.

| Endpoint | Realizes | Backs |
|----------|----------|-------|
| `dt_pipe_buffer_source` | `dt_pipe_source` (hard) | reads `dt_bit` from an array |
| `dt_pipe_buffer_soft_source` | `dt_pipe_source` (soft) | reads `dt_soft_bit` from an array |
| `dt_pipe_buffer_sink` | `dt_pipe_sink` (hard) | writes `dt_bit` into an array |
| `dt_pipe_buffer_soft_sink` | `dt_pipe_sink` (soft) | writes `dt_soft_bit` into an array |

Each `dt_pipe_buffer_*_init(&obj, buf, len)` returns the interface pointer; read
the struct's public `pos` (source cursor) or `len` (sink fill) directly.

## The pipe catalogue

All of these are instances of the one `dt_pipe` type, built by factories with a
matching `_destroy` (and, where noted, extra control calls). Unless stated, a
factory returns NULL out of memory, and the pipe does **not** own what you hand
it (codecs, sinks, sources, or stage pipes are the caller's to free).

### Converters & custom pipes — [`pipes.h`](../../include/drifty/pipe/pipes.h)

| Builder | Does |
|---------|------|
| `dt_pipe_hardening_create()` | sink accepts hard **and** soft; `tick` hardens (argmax) to hard on the source (soft_pull is a no-op) |
| `dt_pipe_softening_create()` | mirror: hard input lifted to soft; source yields soft (pull is a no-op) |
| `dt_pipe_executor_create(begin, tick, finalize, data)` | a pipe whose phases are **your** functions, each handed a `src` (draws the input buffer) and `dst` (appends the output buffer); NULL fn = no-op. The general block the others specialize. |

### Codec adapters — [`streams.h`](../../include/drifty/pipe/streams.h)

Wrap a [streaming codec](../stream.md) as a pipe. The codec is not owned (free it
after the pipe with its own `_destroy`).

| Builder | Wraps | Direction |
|---------|-------|-----------|
| `dt_pipe_encoder_create(dt_stream_encoder *)` | encoder | info bits → coded bits (hard → hard) |
| `dt_pipe_decoder_create(dt_stream_decoder *)` | decoder | coded → recovered (hard → hard) |
| `dt_pipe_soft_decoder_create(dt_stream_soft_decoder *)` | soft decoder | coded → soft records (hard → soft) |

### Frame codec adapters — [`frames.h`](../../include/drifty/pipe/frames.h)

Wrap a [frame codec](../frame.md) as a pipe — the frame-codec counterpart of the
streaming adapters above. The codec is not owned. A frame codec has boundary
operations the plain `begin`/`tick`/`finalize` lifecycle cannot carry, so they ride
as extra, non-vtable calls on the pipe.

| Builder | Wraps | Direction |
|---------|-------|-----------|
| `dt_pipe_frame_encoder_create(dt_frame_encoder *)` | frame encoder | payload → framed coded bits (hard → hard) |
| `dt_pipe_frame_decoder_create(dt_frame_decoder *)` | frame decoder | framed → recovered (hard → hard) |
| `dt_pipe_frame_soft_decoder_create(dt_frame_soft_decoder *)` | soft frame decoder | framed → recovered records (soft → soft) |

- **Encoder**: open and close each frame with `dt_pipe_frame_encoder_begin_frame(p)`
  / `dt_pipe_frame_encoder_end_frame(p)`, which emit the delimiters into the output
  buffer around the push/tick that feeds the frame's payload.
- **Decoders**: a `tick` copies bits only up to the next **frame-state change** and
  then **stalls** — it copies nothing further until `dt_pipe_frame_decoder_advance(p)`
  steps past the boundary. So a decoder is walked boundary by boundary: `tick` to copy
  the next run, read `dt_pipe_frame_decoder_get_state(p)` (OUTSIDE / BEGIN / INSIDE /
  END) and pull the output, then `advance`. Each tick's output is one run (a frame's
  payload, or the verbatim bits between frames); `finalize` flushes through any stall.
  The soft decoder has the matching `_get_state` / `_advance`.

### Compounds

| Builder | Does |
|---------|------|
| `dt_pipeline_create(dt_pipe **stages, size_t count)` | a **linear** compound `dt_pipe`: each of its begin/tick/finalize moves bits from stage to stage (input → A, A runs, A → B, B runs, …, last → output) and drives that stage's call. A pipeline is a `dt_pipe`, so it nests. |
| `dt_pipeline_add(pipe, stage)` / `dt_pipeline_remove(pipe, stage)` | reshape a pipeline between ticks — append a last stage, or remove one (order preserved) |
| `dt_pipe_container_create()` | a vtable that **holds** a set of pipes and drives their `begin`/`tick`/`finalize` together, but does **not** move bits between them (unlike a pipeline). `add(pipe, destroyer)` records how to free each; `remove` unlinks without destroying; `destroy` frees the owned pipes in **reverse** add order. |

### Multi-way routing — [`multi.h`](../../include/drifty/pipe/multi.h)

Fan-out, fan-in, and gating. All match hard/soft faces automatically (a
destination lacking the carried face is skipped). Their `begin` is a no-op — the
routing runs in `tick`/`finalize`, so external endpoints are not touched early.

| Builder | Shape | Control |
|---------|-------|---------|
| `dt_pipe_splitter_create(sinks, n)` | copy input → own output **and** every sink | — |
| `dt_pipe_combiner_create(sources, n)` | own input then each source → output (concatenate) | — |
| `dt_pipe_diverter_create(sinks, n)` | input → own output **or** one sink | `dt_pipe_diverter_select(pipe, idx)` — 0 = output, k = sinks[k-1] |
| `dt_pipe_selector_create(sources, n)` | own input **or** one source → output (others drained) | `dt_pipe_selector_select(pipe, idx)` — 0 = own input, k = sources[k-1] |
| `dt_pipe_valve_create()` | input → output (open) or dropped (closed) | `dt_pipe_valve_open` / `dt_pipe_valve_close` (created open) |
| `dt_pipe_drain_create()` | input discarded | — |
| `dt_pipe_push_to_create(dt_pipe_sink *)` | input → a fixed external sink (a wired-in diverter) | — |
| `dt_pipe_pull_from_create(dt_pipe_source *)` | a fixed external source → output (a wired-in selector) | — |

## Composing: pipeline vs. container

Both hold several pipes, but they compose different things:

- A **pipeline** is a *data* composite — one `dt_pipe` that streams a bit stream
  through its stages in order, moving bits between them each tick. Use it to build
  a processing chain (e.g. `encoder → decoder`, or `pull_from(radio) → soft_decoder
  → hardening → push_to(log)`).
- A **container** is a *lifecycle* composite — it drives `begin`/`tick`/`finalize`
  on a set of otherwise-independent pipes (which you feed and drain yourself) and
  owns their teardown as a group. Use it for a bank of side branches you want
  begun / ticked / finalized and destroyed together.

## Ownership & lifetime

- Endpoints (`buffers.h`) are caller-owned structs — no `_destroy`; they must
  outlive any pipe using them.
- A pipe's `_destroy` frees only the pipe (and its own buffers). Wrapped **codecs**
  (streams.h), **sinks/sources** handed to routing pipes, and **stage pipes** of a
  pipeline are **not** owned — free them yourself, after the pipe.
- The exception is the **container**, which owns the pipes added with a non-NULL
  `destroyer` and frees them on `dt_pipe_container_destroy` (reverse order); pass a
  NULL destroyer to have it drive but not own a pipe.

## See also

- [Example 12 — pipe](../../examples/12_pipe) — a runnable tour: endpoints and
  `dt_pipe_pump`, one pipe's lifecycle, a pipeline round-trip, and a splitter tee.
- [Example 14 — pipeline](../../examples/14_pipeline) — the capstone: a whole
  detection-routed receiver built as one composed graph (pipelines, two diverters
  steered by executor pipes, a combiner, the codec and frame adapters) held in a pipe
  container, with `dt_container` for cleanup. The caller only pushes bits and reads the
  final frame pipe. See its `layout.dot` for the graph.
- [Streaming interface](../stream.md) — the `dt_stream_encoder` / `_decoder` /
  `_soft_decoder` vtables the codec pipes wrap.
- [Symbols (`bit`)](../bit.md) and [Soft decoding](../soft_decoding.md) — the hard
  `dt_bit` and soft `dt_soft_bit` faces every pipe moves.
