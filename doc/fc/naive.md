# drifty — `naive` frame codec

The simplest frame codec: **fixed-length** frames with no delimiters at all,
presented through the frame codec interfaces
([`frame_encoder.h`](../../include/drifty/frame_encoder.h),
[`frame_decoder.h`](../../include/drifty/frame_decoder.h),
[`frame_soft_decoder.h`](../../include/drifty/frame_soft_decoder.h)). Every frame
is `len` symbols; both sides agree on `len` up front, so the stream needs no
preamble, trailer, or marker. The encoder is a pure pass-through and the decoder
re-splits the stream back into `len`-symbol frames, reporting the boundaries
through `get_state()`. It carries no error correction — pair it with an inner
[`cc/`](../cc/README.md) or outer [`bc/`](../bc/rs251.md) codec for protection.

For the symbol alphabet and what each interface means, see
[Data-flow semantics](../data_flow_semantics.md); for the interface contract the
codec implements, see the [frame interface](../frame.md).

## How it frames

There is no wire-level delimiter. The encoder copies bits through unchanged; the
frame structure is purely the agreed `len`. The decoder counts `len` symbols per
frame and reports each boundary through its state machine, which it walks one
transition per `decode()`:

```
OUTSIDE -> (BEGIN -> INSIDE -> END)*
```

Because the preamble and trailer are zero-length, `BEGIN` and `END` are 0-bit
transitions: a `decode()` makes a single state move, consuming the frame's `len`
payload symbols while `INSIDE`. Once the first frame opens the decoder never
returns to `OUTSIDE` — frames run back to back — so the caller must drive
`decode()` + `get_state()` to observe each `OUTSIDE → BEGIN → INSIDE → END → BEGIN
→ …` step.

## API

```c
#include <drifty/fc/naive.h>

dt_frame_encoder      *dt_fc_naive_frame_encoder_create(size_t len);
void                   dt_fc_naive_frame_encoder_destroy(dt_frame_encoder *enc);

dt_frame_decoder      *dt_fc_naive_frame_decoder_create(size_t len);
void                   dt_fc_naive_frame_decoder_destroy(dt_frame_decoder *dec);

dt_frame_soft_decoder *dt_fc_naive_frame_soft_decoder_create(size_t len);
void                   dt_fc_naive_frame_soft_decoder_destroy(dt_frame_soft_decoder *dec);
```

Each factory takes the frame length `len`, in symbols. Factories return NULL on out
of memory; free with the matching `_destroy()` (NULL is fine).

## No buffering

A delay-free pass-through buffers nothing: every call **consumes exactly the bits
it writes**, so the caller advances `src` by the return value. This is a deliberate
departure from the general "a `decode()` that returns `dst_len` has more buffered,
drain with `src_len` 0" rule on the [frame interface](../frame.md) — `naive` has
nothing to buffer, so draining always returns 0.

## Driving the encoder

The encoder writes no preamble, marker, or trailer — `begin`, `begin_frame`,
`end_frame`, and `finalize` all emit nothing; `encode` copies `src` to `dst`
verbatim. Encode `len` symbols per frame so the decoder's fixed split lines up:

```c
dt_frame_encoder *enc = dt_fc_naive_frame_encoder_create(LEN);
int n = 0;
n += enc->begin(enc, out + n, cap - n);                 /* 0 */
for (int f = 0; f < nframes; ++f) {
    n += enc->begin_frame(enc, out + n, cap - n);       /* 0 */
    n += enc->encode(enc, out + n, cap - n, msg + f * LEN, LEN);  /* copies LEN */
    n += enc->end_frame(enc, out + n, cap - n);         /* 0 */
}
n += enc->finalize(enc, out + n, cap - n);              /* 0 */
dt_fc_naive_frame_encoder_destroy(enc);
```

## Driving the decoder

`decode()` advances one state transition per call (or stops when `dst` fills);
`get_state()` reports where it landed. The zero-length `BEGIN`/`END` transitions
return 0 bits, so the caller keeps going until a call makes no progress (0 bits and
no state change) — then `finalize`.

```c
dt_frame_decoder *dec = dt_fc_naive_frame_decoder_create(LEN);
dec->begin(dec, NULL, 0);                                /* no preamble */
dt_frame_decoder_state prev = dec->get_state(dec);       /* OUTSIDE */
for (;;) {
    int n = dec->decode(dec, dst + dpos, cap - dpos, src + spos, src_len - spos);
    dt_frame_decoder_state state = dec->get_state(dec);  /* check after every decode */
    spos += n;                                           /* consumed == written */
    dpos += n;
    if (n == 0 && state == prev) {
        break;                                           /* no progress: settled */
    }
    /* state == INSIDE: dst[dpos-n .. dpos) is this frame's payload */
    prev = state;
}
dec->finalize(dec, dst + dpos, cap - dpos);
dt_fc_naive_frame_decoder_destroy(dec);
```

A partial final frame (fewer than `len` symbols before the stream ends) stays
`INSIDE` rather than reaching `END` — it never completed.

## Soft decoding

`dt_fc_naive_frame_soft_decoder_create(len)` builds a fully soft decoder (the
[frame soft-decoder interface](../frame.md)): its `src` and `dst` are `dt_soft_bit`
([symbols](../bit.md)) throughout. It runs the identical `len`-counting state
machine and passes the payload soft records through unchanged, so graded soft
values survive a frame intact.

## Choosing `naive`

Pick `naive` when every frame is the **same known size** — it adds no overhead and
no escaping, just the fixed split and the frame-state machine. When frames are
**variable-length** or the boundaries must be self-describing in the stream, use
the [`marker`](marker.md) codec, which delimits frames with escape sequences
instead. Neither codec corrects errors; put the protection in the
[`cc/`](../cc/README.md) / [`bc/`](../bc/rs251.md) codec you frame around.
