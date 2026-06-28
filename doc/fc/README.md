# drifty — Frame coding (`fc`)

The frame codecs. Each splits a continuous stream into delimited **frames** —
protected payload framed, unprotected bits between frames passing through — over
the [frame interface](../frame.md) (`dt_frame_encoder` / `dt_frame_decoder` /
`dt_frame_soft_decoder`). A frame codec carries **no error correction** of its own:
it manages boundaries, so you frame it around an inner [`cc/`](../cc/README.md) or
outer [`bc/`](../bc/rs251.md) codec that does the protecting.

The symbol alphabet these components speak is defined in
[Data-flow semantics](../data_flow_semantics.md); read it first.

## Codecs

| Codec | Frames | Delimiting | Soft decoder |
|-------|--------|-----------|:------------:|
| **[naive](naive.md)** | fixed length (`len` symbols) | none — both sides agree on `len` | ✅ |
| **[marker](marker.md)** | variable length | 18-bit escape sequences (escaped payload) | ✅ |

Both expose the same six factories — an encoder, a hard decoder, and a fully soft
decoder — and report frame boundaries through the decoder's `get_state()` machine,
`OUTSIDE → (BEGIN → INSIDE → END)*`.

## Choosing a codec

- **Every frame the same known size** → [`naive`](naive.md). No markers, no
  escaping, no overhead — just the fixed split and the frame-state machine.
- **Variable-length frames, or boundaries that must be self-describing in the
  stream** → [`marker`](marker.md). Begin/end markers delimit each frame and the
  payload is escaped so any bit pattern stays transparent.

## Driving any codec

Every codec is driven through the frame vtables — the encoder
`begin → [ encode | begin_frame → encode* → end_frame ]* → finalize`, the decoder
`begin → decode (repeat) → finalize` with `get_state()` checked after every
`decode()`. See the [frame interface](../frame.md) for the full contract and each
codec's page for the specifics (including how each handles buffering).
