# 11 — detect

Blind **code-presence detection** with the [`detect`](../../doc/cc/detect.md)
meta-codec. Every other example encodes or decodes a *known* code; `detect` is the
odd one out — given an arbitrary bit stream with **no prior knowledge or
coordination** (no code, rate, generators, or alignment), it reports per position
how confident it is that a convolutional code is present. It does not recover bit
values — it answers "is there a code here?".

## What it shows

- `dt_cc_detect_soft_decoder_create(&params)` — it takes the same rich channel
  model as `hybrid`/`maxir` (here a clean channel) — driven through the normal
  soft-decoder vtable.
- The repurposed output fields: `c_erasure` = confidence a code **is** present,
  `c_absent` = confidence it is **not** (they need not sum to 1).
- **Part A:** a whole coded stream reads as "present", a random stream as "absent".
- **Part B:** a coded segment spliced into a random stream — detect **localizes**
  the code per-position, with no hint of where (or whether) it is.

## Run

```sh
./build/examples/11_detect/11_detect
```

## Expected output

```
Part A - a whole stream, coded vs random:
  coded  (4012 bits): code-present [####################### ] 0.96   no-code 0.00
  random (4012 bits): code-present [                        ] 0.00   no-code 0.96

Part B - a coded segment spliced into a random stream...
  true coded region: bits [1536, 4348)
  bits  1152.. 1536 (random) [                        ] 0.00
  bits  1536.. 1920 (coded) [########################] 1.00
  ...
  bits  3840.. 4224 (coded) [########################] 1.00
  bits  4224.. 4608 (random) [                        ] 0.00
```

## Reading it

- The bar is the mean code-present confidence over a 384-bit detection block: ~0 in
  the random regions, ~1 across the coded segment. detect finds the code's location
  without being told it exists — the "no coordination" property.
- Part A's `0.96` (not `1.00`) is the unanalyzable final tail: the last sub-block
  has too few bits to determine the code's structure, so detect honestly abstains
  there (those positions read 0), pulling the mean down slightly.
- Detection has **block granularity** (one verdict per 384-bit block), so the edges
  of the coded segment land in a transition block.

## Method & limits (see [the page](../../doc/cc/detect.md))

A convolutional code is linear, so windows of coded bits are GF(2) **rank-deficient**
(they satisfy parity checks) while random bits are full-rank; detect sweeps
candidate block sizes and measures the deficiency. It is brittle to noise (a flip
breaks exact parity) — it works in the **clean / very-low-noise** regime, holding to
~0.5 % bit flips and reading "absent" past ~1 %.

## See also

- [`detect` reference](../../doc/cc/detect.md) and [Soft decoding](../../doc/soft_decoding.md).
- [04 soft_output](../04_soft_output) — the normal soft decoders, whose
  `dt_soft_bit` fields detect repurposes.
