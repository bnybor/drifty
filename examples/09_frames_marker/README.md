# 09 — frames_marker

The **frame** interface with variable-length frames. The
[`marker`](../../doc/fc/marker.md) codec delimits frames with 18-bit escape
sequences instead of an agreed length, so frames can be any size and need no length
field. It keeps the stream **transparent**: if the payload happens to contain a
pattern that looks like a marker, the encoder escapes it, so any payload survives.

## What it shows

- **Hard:** three differently-sized frames — one carrying a 20-bit run of `1`s that
  mimics a marker prefix — recovered via the `get_state()` machine, proving
  transparency.
- **Soft:** the same stream as `dt_soft_bit` through the soft frame decoder
  (`dt_fc_marker_frame_soft_decoder_create`), the reframing stage of a soft
  pipeline.
- The marker drive rule: the decoder buffers, so feed the input once and then drain
  with `src_len = 0` (different from naive's advance-the-pointer drive).

## Run

```sh
./build/examples/09_frames_marker/09_frames_marker
```

## Expected output

```
encoded frames of 10 + 40 + 7 = 57 payload bits -> 167 coded bits
(the overhead is the begin/end markers plus escaping of the 1-run)

hard decoder: recovered 57 in-frame bits; payloads (incl. 1-run) transparent: yes
soft decoder: recovered 57 in-frame bits; payloads transparent: yes
```

## Reading it

- Unlike naive (08), marker has wire overhead: the begin/end markers per frame plus
  the escaping of any payload run that resembles a marker. That overhead is what
  buys variable-length frames and transparency.
- Both the hard and soft decoders recover all 57 payload bits including the
  adversarial 1-run — the escaping is invisible end to end.

## See also

- [`marker` codec](../../doc/fc/marker.md) — the escape-sequence wire format and the
  drive contract.
- [Adding a frame layer](../../doc/concatenated.md#adding-a-frame-layer) — wrapping
  the inner/outer stack in marker frames.
