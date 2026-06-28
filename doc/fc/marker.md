# drifty — `marker` frame codec

A frame codec that delimits **variable-length** frames with 18-bit escape
sequences, presented through the frame codec interfaces
([`frame_encoder.h`](../../include/drifty/frame_encoder.h),
[`frame_decoder.h`](../../include/drifty/frame_decoder.h),
[`frame_soft_decoder.h`](../../include/drifty/frame_soft_decoder.h)). The encoder
writes a marker to open and close each frame and keeps the stream transparent by
escaping any payload that would look like a marker; the decoder discovers the
boundaries and reports them through `get_state()`. It carries no error
correction — the frame interface's coded/verbatim split collapses to "everything
is escaped payload" — so pair it with an inner [`cc/`](../cc/README.md) or outer
[`bc/`](../bc/rs251.md) codec for protection.

For the symbol alphabet and what each interface means, see
[Data-flow semantics](../data_flow_semantics.md); for the interface contract the
codec implements, see the [frame interface](../frame.md).

## Escape sequences

Every escape sequence is **fifteen 1s** followed by a **3-bit code**:

| Bits | Code | Meaning | Frame-state effect |
|------|------|---------|--------------------|
| `111111111111111 000` | `000` | pure escape (escaped payload) | none |
| `111111111111111 001` | `001` | begin a frame, from outside | `OUTSIDE → BEGIN → INSIDE` |
| `111111111111111 011` | `011` | begin a frame, from inside | `END → BEGIN` (a new frame, back to back) |
| `111111111111111 010` | `010` | end a frame | `INSIDE → END → OUTSIDE` |

Because every code starts with `0`, **"fifteen 1s then a 0" always begins a
sequence**; fifteen 1s then a 1 is just a longer run of 1s. Frames carry no length
field — a frame runs from its begin marker to the matching end (or to the next
begin-from-inside).

## Transparency (escaping)

So that payload can hold any bits — *both inside a frame and outside one* — the
encoder escapes any payload run that would form a sequence. An escaped run is
emitted as a **pure escape (`111111111111111 000`) plus a 2-bit suffix** that names
which sequence the payload contained:

| Suffix | Payload sequence that occurred | Decoder reconstructs |
|--------|-------------------------------|----------------------|
| `00` | the pure-escape sequence (`1^15 000`) | the fifteen 1s; the `000` code rides on as ordinary data |
| `01` | begin-from-outside (`1^15 001`) | the whole 18-bit sequence as data |
| `10` | end (`1^15 010`) | the whole 18-bit sequence as data |
| `11` | begin-from-inside (`1^15 011`) | the whole 18-bit sequence as data |

A real marker (codes `001` / `011` / `010`, with **no** suffix) is therefore never
confused with the same bits sitting in the payload: after fifteen 1s and a `0`, code
`000` means "escaped payload — read the suffix", and the other three codes mean a
frame transition.

The `suffix 00` case reconstructs *only the fifteen 1s* and lets the trailing code
bits ride through as raw data. That is what keeps a payload that ends a frame on a
bare prefix (`1^15 0`, with no room left for a full code before the closing marker)
unambiguous: the run is broken by an escape before the marker, so the boundary still
decodes exactly.

## What it does (and does not) do

| Concern | Behaviour |
|---------|-----------|
| Frame delimiting | ✅ variable-length, via begin/end markers |
| Payload transparency | ✅ any bit pattern survives, inside or outside a frame |
| Back-to-back frames | ✅ begin-from-inside (`011`) ends one frame and opens the next |
| Error correction | ✗ none — markers and payload are passed through, only escaped |

## API

```c
#include <drifty/fc/marker.h>

dt_frame_encoder      *dt_fc_marker_frame_encoder_create(void);
void                   dt_fc_marker_frame_encoder_destroy(dt_frame_encoder *enc);

dt_frame_decoder      *dt_fc_marker_frame_decoder_create(void);
void                   dt_fc_marker_frame_decoder_destroy(dt_frame_decoder *dec);

dt_frame_soft_decoder *dt_fc_marker_frame_soft_decoder_create(void);
void                   dt_fc_marker_frame_soft_decoder_destroy(dt_frame_soft_decoder *dec);
```

The codec is unparameterised (frames are variable-length, both sides agree only on
the escape format). Factories return NULL on out of memory; free with the matching
`_destroy()` (NULL is fine).

## Buffering

Escaping changes the bit count, so — unlike [`naive`](naive.md) — the encoder and
decoders are **not** 1:1 and buffer their output internally. They follow the
standard frame buffering rule: a call that returns exactly `dst_len` has more
buffered, so call again with no new input (`src_len` 0) to drain before feeding
more. The encoder also holds a few trailing 1s back while it decides whether a run
is the start of a sequence; those are flushed at the next `begin_frame` /
`end_frame` / `finalize`.

## Driving the encoder

Drive it through the frame-encoder phases (see the
[frame interface](../frame.md)). `begin_frame` opens a frame — from outside it
writes a begin-from-outside marker, and **while already inside a frame it writes a
begin-from-inside marker** (`011`), ending the current frame and opening a new one
back to back. `end_frame` writes the end marker; `encode` escapes and emits payload.

```c
dt_frame_encoder *enc = dt_fc_marker_frame_encoder_create();
int n = 0;
n += enc->begin(enc, out + n, cap - n);            /* no preamble */
n += enc->begin_frame(enc, out + n, cap - n);      /* 1^15 001 */
n += enc->encode(enc, out + n, cap - n, msg, msg_len);
n += enc->end_frame(enc, out + n, cap - n);        /* 1^15 010 */
n += enc->finalize(enc, out + n, cap - n);
dt_fc_marker_frame_encoder_destroy(enc);
```

## Driving the decoder

The decoder is a sliding-window scanner: it tracks the trailing run of 1s and, on
fifteen-or-more 1s followed by a `0`, peels any 1s beyond fifteen back out as
payload, then reads the 3-bit code — a marker changes the frame state, a `000`
escape reconstructs payload. Advance it like any frame decoder: `decode` until the
next transition or `dst` fills, and **check `get_state()` after every call**.

```c
dt_frame_decoder *dec = dt_fc_marker_frame_decoder_create();
dec->begin(dec, NULL, 0);                          /* no preamble */
for (;;) {
    int n = dec->decode(dec, dst, dst_len, src, src_len);
    switch (dec->get_state(dec)) {                 /* check after every decode */
    case DT_FRAME_DECODER_BEGIN:  /* a frame just opened */            break;
    case DT_FRAME_DECODER_INSIDE: /* dst[0..n) is this frame's payload */ break;
    case DT_FRAME_DECODER_END:    /* a frame just closed */            break;
    case DT_FRAME_DECODER_OUTSIDE:/* between frames; dst[0..n) is verbatim */ break;
    }
    src_len = 0;                                   /* drain, then feed more */
    /* ... loop until input is exhausted, then dec->finalize(...) ... */
}
dec->finalize(dec, dst, dst_len);
dt_fc_marker_frame_decoder_destroy(dec);
```

The state walks `OUTSIDE → (BEGIN → INSIDE → END)*`, one transition per `decode()`.
A begin-from-inside surfaces as two transitions — `END` then `BEGIN` — so the
caller sees the old frame close and the new one open without an `OUTSIDE` in
between.

## Soft decoding

`dt_fc_marker_frame_soft_decoder_create()` builds a fully soft decoder
(the [frame soft-decoder interface](../frame.md)): its `src` and `dst` are
`dt_soft_bit` ([symbols](../bit.md)) throughout. It runs the same framing machine
on each record's hard projection (`c_true` vs `c_false`) to find markers and runs,
and **passes the payload soft records through unchanged** — so graded soft values
survive a frame intact. Bits it has to reconstruct from an escape (the fifteen 1s,
and any reconstructed code bits) are emitted as saturated soft records, since their
original soft values were spent forming the escape on the encode side.

## Choosing `marker`

Pick `marker` when frames are **variable-length** and the payload is arbitrary —
the escape sequences delimit frames without a length field and the escaping keeps
any payload transparent. If every frame is the same known size, the simpler
[`naive`](naive.md) codec needs no markers or escaping at all. Neither codec
corrects errors; put the protection in the [`cc/`](../cc/README.md) /
[`bc/`](../bc/rs251.md) codec you frame around.
