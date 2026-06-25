# drifty

A small C library for **error correction on a bit stream**. You add redundancy
when you encode; on the other end you decode and get your bits back with errors
corrected — including bits that were **flipped, inserted, dropped, or lost**.
Inserted and dropped bits normally knock an error-correcting code out of sync;
this one stays aligned through them.

It works on a continuous stream: feed received bits in, read corrected bits out
at a fixed delay, with no message lengths or frame boundaries to manage.

## API

Pick a code, encode your bits, then stream-decode the received bits.

```c
#include "drifty/drifty.h"

/* 1. Pick a code (sender and receiver must use the same one). */
dt_code *code = dt_code_create_standard(DT_CODE_K7_RATE_1_2);
/* or define your own: dt_code_create(K, generators, num_generators) */

/* 2. Encode, in as many chunks as you like; carry `state` across calls. */
int state = 0, len = 0;
len += dt_code_encode(code, chunk1, n1, &state, out + len);
len += dt_code_encode(code, chunk2, n2, &state, out + len);
len += dt_code_encode_flush(code, &state, out + len);   /* finish the stream */

/* 3. Decode the received bits. */
dt_stream_decoder *d = dt_stream_decoder_create(code, &(dt_stream_params){
    .decision_depth = 40,   /* output delay; try ~6 * the code's K       */
    .max_drift      = 4,    /* set 0 to correct flips only                */
    .p_flip = 0.01, .p_ins = 0.01, .p_del = 0.01,
});

uint8_t decoded[OUT];
int n = dt_stream_decode(d, in, n_in, decoded, NULL, OUT); /* feed + collect bits */
/* ... repeat as more bits arrive ... */
while (dt_stream_decode_flush(d, decoded, OUT) > 0)    /* drain at the end */
    /* use the decoded bits */ ;

dt_stream_decoder_destroy(d);
dt_code_destroy(code);
```

`dt_code` and `dt_stream_decoder` are opaque handles (`_create` / `_destroy`).
Functions return `DT_OK` (0) or a count on success, or a negative `DT_ERR_*`
code. Each bit is `DT_FALSE` or `DT_TRUE`, or `DT_ERASURE` to mark a received bit
as lost. `dt_code_n()` gives output-bits-per-input-bit for sizing encode buffers.

The decoder starts producing output after a short delay (`decision_depth` bits),
and it locks on whether you decode from the start of a stream or join one already
in progress — so discard the first ~`decision_depth` decoded bits (or send a
known preamble you can skip).

## Decoder settings

Set these in `dt_stream_params`; anything you leave out defaults to 0.

| Field            | What it does |
|------------------|--------------|
| `decision_depth` | Output delay in bits. Larger = more reliable, more latency. Try ~6× the code's K. Required. |
| `p_flip`         | How often a coded bit is flipped (e.g. 0.01 = 1%). Required. |
| `max_drift`      | How far alignment may slip from inserted/dropped bits. 0 (default) corrects flips only; 4–8 also handles insertions and deletions. |
| `p_ins`, `p_del` | How often a coded bit is inserted / dropped (per bit, at any position). Needed when `max_drift > 0`. |
| `p_erase`        | How often a received bit is marked `DT_ERASURE` (lost). |
| `p_ovr`          | How often a coded bit is replaced with a fixed value (`DT_TRUE`/`DT_FALSE`), regardless of what was sent. |

You don't need exact probabilities — rough, order-of-magnitude values are fine;
only their relative sizes matter. They mainly control how readily the decoder
assumes an inserted/dropped bit versus a plain flipped one:

- **Insert/drop probability too low:** the decoder resists realigning and tries
  to explain a real inserted/dropped bit as a run of flipped bits. A badly missed
  one can throw it out of sync for a stretch. Lean this way only if inserts and
  drops are genuinely rare.
- **Insert/drop probability too high:** the decoder over-corrects, imagining
  inserts/drops to explain ordinary noise and adding a few extra errors. This
  degrades gently.

When unsure, tune on representative data rather than chasing exact numbers.

## Decoding against several codes at once

When you don't know which of a few candidate codes produced a stream — they
share a rate and constraint length but differ in their generator polynomials —
decode against all of them at once. For each output bit the multi-decoder
combines the codes' decoded bits weighted by how well each code fits the stream,
emitting the value the weight favours, and marks the bit `DT_ERASURE` when no
code is locked or the weighted vote is too close to call.

```c
const dt_code *codes[] = { code_a, code_b, code_c };   /* same rate */

dt_multi_decoder *m = dt_multi_create(&(dt_multi_params){
    .codes = codes, .codes_len = 3,
    .stream = { .decision_depth = 40, .max_drift = 4,
                .p_flip = 0.01, .p_ins = 0.01, .p_del = 0.01 },
});

uint8_t decoded[OUT];
int which[OUT];                                /* winning code per bit, -1 = erased */
int n = dt_multi_decode(m, in, n_in, decoded, which, OUT);  /* feed + collect bits */
/* ... repeat as more bits arrive ... */
while (dt_multi_decode_flush(m, decoded, OUT) > 0)        /* drain at the end */
    /* use the decoded bits */ ;

dt_multi_destroy(m);   /* frees the decoders it built; you still own the codes */
```

`dt_multi_decoder` is an opaque handle. It builds one stream decoder per code from
the shared `stream` settings, so the codes must share a rate (`dt_code_n`) and a
constraint length (`dt_code_k`), and must outlive the multi-decoder. The optional
`locked_decoder` array (here `which`) reports, per output bit, the index of the
likeliest code or `-1` where the bit was erased. `lock_floor` / `lock_margin` set
how confident the combiner must be — in the best code's absolute lock probability,
and in the winning bit value's lead over the other as a share of the total
likelihood weight — to commit a bit rather than abstain (defaults 0.6 and 0.2).

## Encoding with a selectable code

The sender's mirror of the multi-decoder. Hold a family of codes that share a
rate and constraint length, and on each call encode with the one you pick by
index. The encoder keeps a single state shared across the codes — which is well
defined because a convolutional encoder's state depends only on the constraint
length and the input bits, not the generator polynomials — so you can even switch
codes partway through one message and the stream stays continuous.

```c
const dt_code *codes[] = { code_a, code_b, code_c };   /* same rate and K */

dt_multi_encoder *e = dt_multi_encode_create(&(dt_multi_encode_params){
    .codes = codes, .codes_len = 3,
});

uint8_t coded[OUT];
int len  = dt_multi_encode(e, idx, bits, n_bits, coded, OUT);  /* emit codes[idx] */
/* ... more chunks, optionally with a different idx ... */
len += dt_multi_encode_flush(e, idx, coded + len, OUT - len);  /* finish the stream */

dt_multi_encode_destroy(e);   /* you still own the codes */
```

`dt_multi_encoder` is an opaque handle. The codes must share a rate (`dt_code_n`)
and a constraint length (`dt_code_k`) and must outlive the encoder, which borrows
them (it copies the array, frees neither). Each call writes `n_bits * dt_code_n`
bits for the selected code; `max_out` caps the write. Feed the resulting stream to
a `dt_multi_decoder` over the same family to decode it without knowing the index.

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Metrics

A Monte-Carlo harness measures decoding-mistake rates against flip / insert /
drop channels for each standard code — see [metrics/METRICS.md](metrics/METRICS.md).

## Install

```sh
cmake --install build --prefix /your/prefix
```

## License

MIT — see [LICENSE](LICENSE).
