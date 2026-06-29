# 10 — concatenated

The full stack, where the layers compose: an [`rs251`](../../doc/bc/rs251.md) outer
block code over a [`hybrid`](../../doc/cc/hybrid.md) **soft** inner code, across a
drifting channel.

```
message -> rs251 encode -> cc encode -> [channel] -> cc decode -> rs251 decode -> message
            (outer)         (inner)                   (inner)       (outer)
```

The inner soft decoder absorbs the drift and most flips and reports, per bit, a
graded consistency rather than a hard symbol. The outer soft decoder consumes that,
turns the positions the inner code could not place into Reed–Solomon erasures (the
*erasure bridge*), and corrects the rest. The **hard** path runs on the identical
channel for contrast.

This is the runnable program from [doc/concatenated.md](../../doc/concatenated.md),
which walks through the output in detail.

## What it shows

- The inner→outer hand-off end to end, and why soft output matters: on this burst
  pattern the soft path recovers where the hard path fails.
- Combining `dt_cc_encoder_create` + `dt_cc_hybrid_soft_decoder_create` with
  `dt_bc_rs251_block_*` on one stream, and the drain-then-finalize decode loop.

## Run

```sh
./build/examples/10_concatenated/10_concatenated
```

## Expected output

```
SOFT  (hybrid soft -> soft rs251):
  inner argmax residue: 0 erased + 2 silently-wrong of 40 symbols
  message recovered exactly: YES

HARD  (hybrid hard -> hard rs251), same channel:
  inner residue: 11 erased + 5 silently-wrong; 2*err+eras = 21, RS budget n-k = 16
  message recovered exactly: NO
```

## Reading it

- Same code, same channel, same outer parity — the only difference is reading the
  soft values instead of a hard decision. The two erasure bursts drop the inner
  decoder's lock, so the hard front end gives up on whole symbols (a load of
  `2·5 + 11 = 21` against an RS budget of `16`, and the block fails). The soft
  front end keeps those burst-region bits as a graded lean, leaving only `0` erased
  + `2` silently-wrong — trivially inside budget.
- [doc/concatenated.md](../../doc/concatenated.md) explains the erasure bridge, the
  warm-up prefix, the drain loop, and when the simpler hard path suffices.

## See also

- [Soft decoding](../../doc/soft_decoding.md), [`rs251`](../../doc/bc/rs251.md),
  [`hybrid`](../../doc/cc/hybrid.md).
- [07 block_rs251](../07_block_rs251) and [04 soft_output](../04_soft_output) cover
  the two halves in isolation.
