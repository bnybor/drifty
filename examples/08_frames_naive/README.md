# 08 — frames_naive

The **frame** interface with fixed-length frames. A frame codec splits a continuous
stream into delimited frames and reports the boundaries through a state machine you
read after every `decode()`:

```
OUTSIDE -> (BEGIN -> INSIDE -> END)*
```

The [`naive`](../../doc/fc/naive.md) codec uses fixed-length frames of `len`
symbols with no markers — both sides simply agree on `len`. A frame codec carries
no error correction of its own; you wrap it around an inner `cc` / outer `bc` codec.

## What it shows

- `dt_fc_naive_frame_encoder_create(len)` driven
  `begin → begin_frame → encode → end_frame → finalize`.
- The decoder's `get_state()` boundary machine, and the naive drive rule: it buffers
  nothing, so each `decode()` consumes exactly what it writes — advance the source
  pointer by the return value.
- The naive **soft** frame decoder (`dt_fc_naive_frame_soft_decoder_create`).

## Run

```sh
./build/examples/08_frames_naive/08_frames_naive
```

## Expected output

```
encoded 3 frames of 16 bits -> 48 coded bits (naive adds no overhead)

decoder saw 3 frame opens (BEGIN) and 3 closes (END)
recovered 48 payload bits (expected 48); payloads match: yes
soft decoder: recovered 48 payload bits; payloads match: yes
```

## Reading it

- naive adds no wire overhead (48 payload bits → 48 coded bits); the frame
  structure is purely the agreed `len`. Frames run back to back.
- The transitions (`BEGIN`, `END`) carry 0 bits; the `INSIDE` state copies the
  payload. The state walk is exactly `(BEGIN, INSIDE, END)` per frame.
- The soft decoder runs the identical `len`-counting machine over `dt_soft_bit`
  records — the reframing middle stage of a soft pipeline.

## See also

- [`naive` codec](../../doc/fc/naive.md) and the [frame interface](../../doc/frame.md).
- [09 frames_marker](../09_frames_marker) — variable-length frames, which buffer and
  are driven differently (feed once, then drain).
