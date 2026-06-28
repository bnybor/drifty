# drifty

A small C library for **forward error correction on a bit stream**. You add
redundancy when you encode; on the other end you decode and recover your bits with
errors corrected — including bits that were **flipped, inserted, dropped, or
lost**. Inserted and dropped bits normally knock an error-correcting code out of
sync; drifty's drift-tolerant codecs stay aligned through them.

It works on a continuous stream: feed received bits in, read corrected bits out at
a fixed delay, with no message lengths or frame boundaries to manage.

## Concepts

A **code** (`dt_cc_code`) is the redundancy scheme — pick a ready-made one or define
your own; the sender and receiver must use the same code. Encoding and decoding go
through small **interfaces** you call by function pointer:

- `dt_stream_encoder` — turn input bits into coded bits.
- `dt_stream_decoder` — turn received bits back into input bits (a hard decision).
- `dt_stream_soft_decoder` — the same, but report per-bit *consistencies* rather than a
  hard decision.

drifty ships **five codecs** that implement these interfaces over a `dt_cc_code`,
differing in what channel damage they correct and how much you pay for it:

- **`viterbi`** — a plain Viterbi hard-decision decoder. Corrects flipped and
  erased bits. Simplest and fastest, and takes no settings.
- **`bcjr`** — a MAP (forward-backward) decoder for the same flips-and-erasures
  channel as `viterbi`, adding a **soft decoder** (per-bit consistencies) and the
  ability to lock onto a stream you join mid-flight. Hard or soft output.
- **`vindel`** — adds drift tolerance: it stays aligned even when bits are
  inserted or dropped. Hard decision; a small channel model to set.
- **`hybrid`** — drift-tolerant like `vindel`, and additionally offers a **soft
  decoder** (per-bit consistencies) and the expressive channel model below.
- **`maxir`** — `bcjr`'s drift-tolerant sibling: the same max-log-MAP decoder and
  **full** soft output, extended to stay aligned through inserted and dropped
  bits. The most detailed soft output of the five, and the heaviest decoder.

Build one with its `dt_cc_<codec>_*_create` factories — `dt_cc_viterbi_*`, `dt_cc_bcjr_*`,
`dt_cc_vindel_*`, `dt_cc_hybrid_*`, or `dt_cc_maxir_*` — and include its single header
(`<drifty/cc/viterbi.h>`, `<drifty/cc/bcjr.h>`, `<drifty/cc/vindel.h>`,
`<drifty/cc/hybrid.h>`, or `<drifty/cc/maxir.h>`). They share the code type and
the encoder, so you can swap codecs without re-encoding. See
[Choosing a codec](#choosing-a-codec).

Bits are carried one per byte as `dt_bit` symbols (defined in `<drifty/bit.h>`):
`DT_TRUE` / `DT_FALSE` for a bound value, `DT_ERASURE` for a position whose value
is unspecified (a *don't-care* on the way in, a *don't-know* on the way out), and
`DT_INVALID` for a deliberate non-value; a decoder may additionally emit
`DT_ABSENT` for a position it infers was dropped in transit. See
[Data-flow semantics](doc/data_flow_semantics.md) for the complete symbol model
— the transmit vs output domains and what each stage consumes and produces.

## Documentation

This README is the overview. The full reference lives in [`doc/`](doc/README.md):

- [Data-flow semantics](doc/data_flow_semantics.md) — the complete `DT_` symbol
  model: the transmit vs output domains and what each pipeline stage consumes and
  produces.
- [Convolutional coding (`doc/cc/`)](doc/cc/README.md) — per-codec reference for
  the shared encoder and the five decoders (`viterbi`, `vindel`, `hybrid`,
  `maxir`, `bcjr`), with a guide to choosing one.
- [Block coding (`doc/bc/`)](doc/bc/rs251.md) — the `rs251` Reed–Solomon block
  codec, an RS(n, k) code over GF(251).

## Choosing a codec

All five share the same code (`dt_cc_code`) and the same encoder; they differ only
in the decoder. Pick the least capable one that covers your channel — it is the
simplest and fastest.

|                       | `viterbi` | `bcjr` | `vindel` | `hybrid` | `maxir` |
| --------------------- | :-------: | :----: | :------: | :------: | :-----: |
| Flips & erasures      |     ✓     |   ✓    |    ✓     |    ✓     |    ✓    |
| Drift (insert / drop) |     —     |   —    |    ✓     |    ✓     |    ✓    |
| Blind acquisition     |     —     |   ✓    |    ✓     |    ✓     |    ✓    |
| Soft output           |     —     | ✓ full |    —     |    ✓     | ✓ full  |
| Channel model         |   none    | rates  |  rates   |   rich   |  rich   |

*Full* soft output (the max-log-MAP codecs `bcjr` and `maxir`) additionally reports
the `c_invalid` and `c_absent` consistencies that `hybrid` leaves at 0 — see
[Soft decoding](#soft-decoding).

- Use **`viterbi`** when the received stream stays bit-aligned — the channel only
  flips or erases bits, never inserts or drops them (most wired links, framed
  packets, anything with its own clock recovery). It is a standard convolutional
  FEC: fastest, with nothing to configure.
- Use **`bcjr`** on that same bit-aligned channel when you want **soft** output —
  per-bit consistencies for an outer code or a downstream decision — or need to
  lock onto a stream you join mid-flight. It runs the MAP (forward-backward)
  algorithm: the same flips-and-erasures channel as `viterbi`, more information
  out, a little slower.
- Use **`vindel`** when the stream can lose sync — bits get inserted or dropped,
  so position drifts — and a hard 0/1 per bit is all you need. It tracks the
  drift and re-anchors; you tell it roughly how often each impairment happens.
- Use **`hybrid`** when you also need **soft** output — per-bit consistencies to
  feed an outer code or a downstream decision — or an expressive channel model
  (asymmetric flips, value-specific insertions, stuck/overwritten bits). It is the
  general-purpose default for a drifting channel; `maxir` goes further on
  soft-output detail at more cost.
- Use **`maxir`** for that same drift tolerance with the **fullest** soft output:
  it is `bcjr`'s max-log-MAP decoder extended to a drifting channel (the same
  expressive channel model as `hybrid`), additionally reporting the `c_invalid` /
  `c_absent` consistencies `hybrid` leaves at 0. It runs a full forward-backward
  pass for every bit, so it is the heaviest of the five — reach for it when that
  extra per-bit detail earns its cost.

Drift tolerance is not free: `vindel`, `hybrid`, and `maxir` do proportionally more
work as their drift window widens, while `viterbi` and `bcjr` run a fixed-width
trellis with none of that machinery. The examples below use the `hybrid` codec; the
other four follow the same vtable pattern, differing only in their `_create`
signatures (`viterbi` takes just the code; `bcjr` and `vindel` take simpler
parameter sets; `maxir` takes the same rich set as `hybrid`).

## Quick start

```c
/* 1. Pick a code (sender and receiver must agree). */
dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
/* or roll your own: dt_cc_code_create(K, generators, num_generators) */

/* 2. Encode. Each interface is a vtable you call through: begin writes any
 *    preamble, encode appends coded bits, finalize flushes the tail. */
dt_stream_encoder *enc = dt_cc_encoder_create(code);
dt_bit coded[CAP];                /* size ~ (n_bits + K) * dt_cc_code_n(code) */
int clen  = enc->begin(enc, coded, CAP);
clen     += enc->encode(enc, coded + clen, CAP - clen, bits, n_bits);
clen     += enc->finalize(enc, coded + clen, CAP - clen);
dt_cc_encoder_destroy(enc);

/* 3. Decode the received bits. */
dt_stream_decoder *dec = dt_cc_hybrid_decoder_create(code, &(dt_cc_hybrid_stream_params){
    .decision_depth = 40,   /* output delay; try ~6 * the code's K */
    .max_drift      = 4,    /* set 0 to correct flips only         */
    .p_flip = 0.01, .p_ins_true = 0.005, .p_ins_false = 0.005, .p_del = 0.01,
});
dt_bit out[OUT];
dec->begin(dec, NULL, 0);            /* consume a preamble if any; none here */
int n  = dec->decode(dec, out, OUT, received, n_received);
/* ... call decode again as more bits arrive ... */
n     += dec->finalize(dec, out + n, OUT - n);   /* drain the tail at end-of-stream */
dt_cc_hybrid_decoder_destroy(dec);

dt_cc_code_destroy(code);   /* the code must outlive everything built from it */
```

`dt_cc_code`, `dt_stream_encoder`, and `dt_stream_decoder` are made with `_create` and freed with
`_destroy`. Every `begin` / `encode` / `decode` / `finalize` call returns the
number of bits it wrote into the output buffer, or a negative value on a bad
argument (such as too little room). `dt_cc_code_n()` gives output-bits-per-input-bit
for sizing buffers.

A decoder buffers internally: it keeps any decoded bits that don't fit in the
output buffer you gave it, rather than dropping them. When a `decode` call returns
exactly the capacity you passed (`out` filled), call `decode` again with a fresh
buffer — and no new input (`src_len` 0) — to drain the rest before feeding more.

The decoder produces output after a short delay (`decision_depth` bits) and locks
on whether you start at the beginning of a stream or join one mid-flight — so
discard the first ~`decision_depth` decoded bits (or send a known preamble you can
skip).

## Decoder settings

These are the **`hybrid`** decoder's settings, set in `dt_cc_hybrid_stream_params`;
anything you leave out defaults to 0. **`maxir`** takes the same set
(`dt_cc_maxir_stream_params`). **`vindel`** uses a simpler one
(`dt_cc_vindel_stream_params`: `decision_depth`, `max_drift`, and the rates `p_sub`,
`p_ins`, `p_del`, `p_erase`), **`bcjr`** a smaller one still
(`dt_cc_bcjr_stream_params`: just `decision_depth`, `p_flip`, and `p_erase` — no
drift settings), and **`viterbi`** has none — its decoder is built from the code
alone.

| Field            | What it does |
|------------------|--------------|
| `decision_depth` | Output delay in bits. Larger = more reliable, more latency. Try ~6× the code's K. Required. |
| `p_flip`         | How often a coded bit is flipped (e.g. 0.01 = 1%). Required. |
| `max_drift`      | How far alignment may slip from inserted/dropped bits. 0 (default) corrects flips only; 4–8 also handles insertions and deletions. |
| `p_ins_true`, `p_ins_false`, `p_ins_erase` | How often a spurious `DT_TRUE` / `DT_FALSE` / `DT_ERASURE` bit is inserted (per bit, at any position). Needed when `max_drift > 0`. |
| `p_del`          | How often a coded bit is dropped (per bit, at any position). Needed when `max_drift > 0`. |
| `p_ovr_true`, `p_ovr_false`, `p_ovr_erase` | How often a coded bit is overwritten with a fixed `DT_TRUE` / `DT_FALSE` / `DT_ERASURE`, regardless of what was sent. `p_ovr_erase` is the plain erasure rate — set it (or `p_erase` on `vindel`/`bcjr`) above 0 whenever the received stream can carry `DT_ERASURE`, whether from channel erasures or from encoding `DT_ERASURE` don't-cares; left at 0, an erased position costs infinity and the decoder loses lock there. |

You don't need exact probabilities — rough, order-of-magnitude values are fine;
only their relative sizes matter. They mainly control how readily the decoder
assumes an inserted/dropped bit versus a plain flipped one:

- **Insert/drop probability too low:** the decoder resists realigning and tries to
  explain a real inserted/dropped bit as a run of flipped bits. A badly missed one
  can throw it out of sync for a stretch. Lean this way only if inserts and drops
  are genuinely rare.
- **Insert/drop probability too high:** the decoder over-corrects, imagining
  inserts/drops to explain ordinary noise and adding a few extra errors. This
  degrades gently.

When unsure, tune on representative data rather than chasing exact numbers.

## Soft decoding

When a hard 0/1 isn't enough — for example to feed an outer code — a soft decoder
reports, per bit position, a set of *consistencies* in `[0, 1]`: the goodness-of-fit
of each hypothesis, not a probability split (they need not sum to 1). Of the five
codecs, **`bcjr`**, **`hybrid`**, and **`maxir`** offer a soft decoder — `bcjr` on
a bit-aligned channel, `hybrid` and `maxir` when you also need drift tolerance.

```c
dt_stream_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(code, &params);
dt_soft_bit soft[OUT];
int n  = sd->begin(sd, NULL, 0);     /* the hybrid codec uses no preamble */
n     += sd->decode(sd, soft + n, OUT - n, received, n_received);
n     += sd->finalize(sd, soft + n, OUT - n);
/* soft[i].c_true / c_false - consistency the bit was true / false
 * soft[i].c_erasure        - consistency its value is unrecoverable
 * soft[i].c_locked         - consistency the decoder is tracking this code   */
dt_cc_hybrid_soft_decoder_destroy(sd);
```

`dt_soft_bit` also carries `c_invalid` — the position reads as the
encoder's deliberate non-value (`DT_INVALID`) — and `c_absent` — the position reads
as dropped in transit (`DT_ABSENT`). The hybrid codec does not model those and
leaves them 0, while the *full* max-log-MAP codecs (`bcjr` and `maxir`) populate
them.

## Block and frame codecs

The codecs above are convolutional *inner* codes driven as a continuous stream.
drifty also defines two other codec framings over the same `dt_bit` alphabet, each
a small vtable interface whose operations return `dt_result` status codes
(`<drifty/result.h>`):

- **Block** — fixed-size `(k, n)` blocks: `<drifty/block_encoder.h>`,
  `<drifty/block_decoder.h>`, and a soft-input `<drifty/block_soft_decoder.h>`.
- **Frame** — a stream split into delimited frames with uncoded passthrough
  between them: `<drifty/frame_encoder.h>`, `<drifty/frame_decoder.h>`.

The implemented block codec is **[`rs251`](doc/bc/rs251.md)**
(`<drifty/bc/rs251.h>`, `dt_bc_rs251_block_*`): a systematic Reed–Solomon RS(n, k)
code over GF(251) that corrects errors and erasures while
`2·errors + erasures ≤ n − k`. As an outer block code it pairs naturally with the
convolutional inner codecs. The frame interfaces and the soft-input block
interface are defined but not yet implemented.

## Build

```sh
cmake -S . -B build
cmake --build build
```

This produces `libdrifty.a` (self-contained) and `libdrifty_bare.a` (the
freestanding core, with the few libc shims left for you to supply). Only the
public API is exported — `dt_cc_code_*`, the shared encoder `dt_cc_encoder_*`, the
per-codec decoder factories `dt_cc_viterbi_*` / `dt_cc_bcjr_*` / `dt_cc_vindel_*` /
`dt_cc_hybrid_*` / `dt_cc_maxir_*`, and the `dt_bc_rs251_block_*` Reed–Solomon block
codec — while the engine internals (including the bundled `rs251` library) are
hidden.

A top-level build defaults to `CMAKE_BUILD_TYPE=Release` (`-O3`) — the decoders
are numeric hot loops, so this matters. Override it explicitly when you need to,
e.g. `-DCMAKE_BUILD_TYPE=RelWithDebInfo` or `=Debug`. To also tune the core for
the building host's CPU (`-march=native`, not portable), configure with
`-DDRIFTY_NATIVE=ON`.

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Metrics

A Monte-Carlo harness measures decoding-mistake rates against flip / insert /
drop / erase channels for each standard code — see
[metrics/hybrid/METRICS.md](metrics/hybrid/METRICS.md),
[metrics/maxir/METRICS.md](metrics/maxir/METRICS.md),
[metrics/vindel/METRICS.md](metrics/vindel/METRICS.md),
[metrics/viterbi/METRICS.md](metrics/viterbi/METRICS.md), and
[metrics/bcjr/METRICS.md](metrics/bcjr/METRICS.md). The no-drift decoders
(`viterbi`, `bcjr`) sweep only the flip and erase channels; the drift-tolerant ones
(`hybrid`, `vindel`, `maxir`) sweep insert and drop as well. Each METRICS.md embeds
the full-resolution sweep (30 trials × 4000 info bits per point) with plots.

On the insert/drop (drift) channels the sweeps give a clear pick of code and
decoder for indel recovery:

- **Code — use `K5_R1_5`.** Indel tolerance tracks redundancy (code rate), not
  constraint length: the rate-1/5 `K5_R1_5` holds out far longer than the rate-1/3
  `K7_R1_3`, which in turn beats the two rate-1/2 codes (`K7_R1_2`, `K3_R1_2`). At a
  10% insert/drop rate `K5_R1_5` still decodes almost cleanly (under ~1% edit rate)
  while the weakest code `K3_R1_2` sits near 20%. The price is its 5× coded-bit
  bandwidth.
- **Decoder — use `maxir` (or `hybrid`).** The two soft forward-backward decoders
  beat hard-decision `vindel` on indels everywhere — often 2–8× lower edit rate on
  insertions — because marginalising over alignments recovers a sync slip better
  than committing to a single survivor path. `maxir` edges `hybrid`, and the gap
  widens when the indel rate is *not* anticipated, so it degrades most gracefully on
  an uncertain channel; `hybrid` is an even match when the rate is known. `vindel`'s
  hard-decision traceback is the wrong tool here — keep it for cheaper substitution
  / erasure work.

## Install

```sh
cmake --install build --prefix /your/prefix
```

Installs `libdrifty.a`, `libdrifty_bare.a`, and the public headers under
`include/drifty/`.

## License

MIT — see [LICENSE](LICENSE).
