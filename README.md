# drifty

A small C library for **forward error correction on a bit stream**. You add
redundancy when you encode; on the other end you decode and recover your bits with
errors corrected — including bits that were **flipped, inserted, dropped, or
lost**. Inserted and dropped bits normally knock an error-correcting code out of
sync; drifty's drift-tolerant codecs stay aligned through them.

It works on a continuous stream: feed received bits in, read corrected bits out at
a fixed delay, with no message lengths or frame boundaries to manage.

## Concepts

A **code** (`dt_ccode`) is the redundancy scheme — pick a ready-made one or define
your own; the sender and receiver must use the same code. Encoding and decoding go
through small **interfaces** you call by function pointer:

- `dt_encoder` — turn input bits into coded bits.
- `dt_decoder` — turn received bits back into input bits (a hard decision).
- `dt_soft_decoder` — the same, but report per-bit *consistencies* rather than a
  hard decision.

drifty ships **three codecs** that implement these interfaces over a `dt_ccode`,
differing in what channel damage they correct and how much you pay for it:

- **`viterbi`** — a plain Viterbi hard-decision decoder. Corrects flipped and
  erased bits. Simplest and fastest, and takes no settings.
- **`vindel`** — adds drift tolerance: it stays aligned even when bits are
  inserted or dropped. Hard decision; a small channel model to set.
- **`hybrid`** — drift-tolerant like `vindel`, and additionally offers a **soft
  decoder** (per-bit consistencies) and the most expressive channel model.

Build one with its `dt_<codec>_*_create` factories — `dt_viterbi_*`,
`dt_vindel_*`, or `dt_hybrid_*` — and include its single header
(`<drifty/viterbi.h>`, `<drifty/vindel.h>`, or `<drifty/hybrid.h>`). They share
the code type and the encoder, so you can swap codecs without re-encoding. See
[Choosing a codec](#choosing-a-codec).

Bits are carried one per byte as `dt_t` symbols: `DT_FALSE`, `DT_TRUE`, or
`DT_ERASURE` to mark a received bit as lost (defined in `<drifty/bit.h>`).

## Choosing a codec

All three share the same code (`dt_ccode`) and the same encoder; they differ only
in the decoder. Pick the least capable one that covers your channel — it is the
simplest and fastest.

|                                        | `viterbi`            | `vindel`           | `hybrid`          |
|----------------------------------------|----------------------|--------------------|-------------------|
| Corrects flips & erasures              | ✓                    | ✓                  | ✓                 |
| Tracks drift (inserted / dropped bits) | —                    | ✓                  | ✓                 |
| Blind acquisition (join mid-stream)    | —                    | ✓                  | ✓                 |
| Soft output (per-bit consistencies)    | —                    | —                  | ✓                 |
| Settings to tune                       | none                 | channel rates      | channel rates (richer) |
| Header                                 | `<drifty/viterbi.h>` | `<drifty/vindel.h>`| `<drifty/hybrid.h>` |

- Use **`viterbi`** when the received stream stays bit-aligned — the channel only
  flips or erases bits, never inserts or drops them (most wired links, framed
  packets, anything with its own clock recovery). It is a standard convolutional
  FEC: fastest, with nothing to configure.
- Use **`vindel`** when the stream can lose sync — bits get inserted or dropped,
  so position drifts — and a hard 0/1 per bit is all you need. It tracks the
  drift and re-anchors; you tell it roughly how often each impairment happens.
- Use **`hybrid`** when you also need **soft** output — per-bit consistencies to
  feed an outer code or a downstream decision — or the most expressive channel
  model (asymmetric flips, value-specific insertions, stuck/overwritten bits). It
  is the most capable, and the default when in doubt.

Drift tolerance is not free: `vindel` and `hybrid` do proportionally more work as
their drift window widens, while `viterbi` is a plain trellis with none of that
machinery. The examples below use the `hybrid` codec; `viterbi` and `vindel`
follow the same vtable pattern, differing only in their `_create` signatures
(`viterbi` takes just the code, `vindel` a simpler parameter set).

## Quick start

```c
/* 1. Pick a code (sender and receiver must agree). */
dt_ccode *code = dt_ccode_create_standard(DT_CODE_K7_RATE_1_2);
/* or roll your own: dt_ccode_create(K, generators, num_generators) */

/* 2. Encode. Each interface is a vtable you call through: begin writes any
 *    preamble, encode appends coded bits, finalize flushes the tail. */
dt_encoder *enc = dt_hybrid_encoder_create(code);
dt_t coded[CAP];                  /* size ~ (n_bits + K) * dt_ccode_n(code) */
int clen  = enc->begin(enc, coded, CAP);
clen     += enc->encode(enc, coded + clen, CAP - clen, bits, n_bits);
clen     += enc->finalize(enc, coded + clen, CAP - clen);
dt_hybrid_encoder_destroy(enc);

/* 3. Decode the received bits. */
dt_decoder *dec = dt_hybrid_decoder_create(code, &(dt_hybrid_stream_params){
    .decision_depth = 40,   /* output delay; try ~6 * the code's K */
    .max_drift      = 4,    /* set 0 to correct flips only         */
    .p_flip = 0.01, .p_ins_true = 0.005, .p_ins_false = 0.005, .p_del = 0.01,
});
dt_t out[OUT];
int n  = dec->begin(dec, out, OUT);
n     += dec->decode(dec, out + n, OUT - n, received, n_received);
/* ... call decode again as more bits arrive ... */
n     += dec->finalize(dec, out + n, OUT - n);   /* drain the tail at end-of-stream */
dt_hybrid_decoder_destroy(dec);

dt_ccode_destroy(code);   /* the code must outlive everything built from it */
```

`dt_ccode`, `dt_encoder`, and `dt_decoder` are made with `_create` and freed with
`_destroy`. Every `begin` / `encode` / `decode` / `finalize` call returns the
number of bits it wrote into the output buffer, or a negative value on a bad
argument (such as too little room). `dt_ccode_n()` gives output-bits-per-input-bit
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

These are the **`hybrid`** decoder's settings, set in `dt_hybrid_stream_params`;
anything you leave out defaults to 0. **`vindel`** uses a simpler set
(`dt_vindel_stream_params`: `decision_depth`, `max_drift`, and the rates `p_sub`,
`p_ins`, `p_del`, `p_erase`), and **`viterbi`** has none — its decoder is built
from the code alone.

| Field            | What it does |
|------------------|--------------|
| `decision_depth` | Output delay in bits. Larger = more reliable, more latency. Try ~6× the code's K. Required. |
| `p_flip`         | How often a coded bit is flipped (e.g. 0.01 = 1%). Required. |
| `max_drift`      | How far alignment may slip from inserted/dropped bits. 0 (default) corrects flips only; 4–8 also handles insertions and deletions. |
| `p_ins_true`, `p_ins_false`, `p_ins_erase` | How often a spurious `DT_TRUE` / `DT_FALSE` / `DT_ERASURE` bit is inserted (per bit, at any position). Needed when `max_drift > 0`. |
| `p_del`          | How often a coded bit is dropped (per bit, at any position). Needed when `max_drift > 0`. |
| `p_ovr_true`, `p_ovr_false`, `p_ovr_erase` | How often a coded bit is overwritten with a fixed `DT_TRUE` / `DT_FALSE` / `DT_ERASURE`, regardless of what was sent. `p_ovr_erase` is the plain erasure rate. |

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
of each hypothesis, not a probability split (they need not sum to 1). Of the three
codecs, only **`hybrid`** offers a soft decoder.

```c
dt_soft_decoder *sd = dt_hybrid_soft_decoder_create(code, &params);
dt_soft_decoder_out soft[OUT];
int n  = sd->begin(sd, NULL, 0);     /* the hybrid codec emits no preamble */
n     += sd->decode(sd, soft + n, OUT - n, received, n_received);
n     += sd->finalize(sd, soft + n, OUT - n);
/* soft[i].c_true / c_false - consistency the bit was true / false
 * soft[i].c_erasure        - consistency its value is unrecoverable
 * soft[i].c_locked         - consistency the decoder is tracking this code   */
dt_hybrid_soft_decoder_destroy(sd);
```

`dt_soft_decoder_out` also carries `c_invalid` and `c_absent`; the hybrid codec
does not model those and leaves them 0.

## Build

```sh
cmake -S . -B build
cmake --build build
```

This produces `libdrifty.a` (self-contained) and `libdrifty_bare.a` (the
freestanding core, with the few libc shims left for you to supply). Only the
public API — `dt_ccode_*` and the codec factories `dt_viterbi_*`, `dt_vindel_*`,
and `dt_hybrid_*` — is exported; the engine internals are hidden.

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Metrics

A Monte-Carlo harness measures decoding-mistake rates against flip / insert /
drop / erase channels for each standard code, one per codec — see
[metrics/hybrid/METRICS.md](metrics/hybrid/METRICS.md),
[metrics/vindel/METRICS.md](metrics/vindel/METRICS.md), and
[metrics/viterbi/METRICS.md](metrics/viterbi/METRICS.md) (`viterbi`, which does
not track drift, sweeps only the flip and erase channels).

## Install

```sh
cmake --install build --prefix /your/prefix
```

Installs `libdrifty.a`, `libdrifty_bare.a`, and the public headers under
`include/drifty/`.

## License

MIT — see [LICENSE](LICENSE).
