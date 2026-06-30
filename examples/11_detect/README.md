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
- **Part C:** the same detection through an insert/delete channel — detect tolerates
  the drift and still finds the code, degrading gracefully with the indel rate.

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
  bits  1536.. 1920 (coded)  [########################] 1.00
  ...
  bits  3840.. 4224 (coded)  [########################] 1.00
  bits  4224.. 4608 (random) [##########              ] 0.43   <- edge block straddles the boundary

Part C - detection through an insert/delete channel...
  0.0% deletions (4012 bits): code-present [########################] 1.00
  0.5% deletions (3998 bits): code-present [####################### ] 0.97
  1.0% deletions (3966 bits): code-present [###################     ] 0.78
  2.0% deletions (3945 bits): code-present [###########             ] 0.47
```

## Reading it

- **Part B:** ~0 in the random regions, ~1 across the coded segment. detect finds
  the code's location without being told it exists — the "no coordination" property.
  Blocks that straddle the code/random boundary read a partial value (the analysis
  windows fully inside the code fire; those crossing the edge do not).
- Part A's `0.96` (not `1.00`) is the unanalyzable final tail: the last positions
  have too few following bits for a full analysis window, so detect abstains there
  (reads 0), pulling the mean down slightly.
- **Part C:** indels shift the bit phase — which desyncs an ordinary decoder — but
  detect's analysis windows slide, so it only needs one indel-free aligned run to
  fire. Detection fades smoothly as the indel rate climbs.

## Method & limits (see [the page](../../doc/cc/detect.md))

A convolutional code is linear, so windows of coded bits are GF(2) **rank-deficient**
(they satisfy parity checks) while random bits are full-rank; detect slides short
windows over candidate block sizes and takes each position's max deficiency. Sliding
is what buys indel tolerance — only a *locally* clean aligned run is needed. It is
still brittle to **flips** (a flip is an independent row in every covering window),
so it works in the **clean / very-low-noise** regime: it holds to ~1 % flips and to
~2–3 % indels.

## See also

- [`detect` reference](../../doc/cc/detect.md) and [Soft decoding](../../doc/soft_decoding.md).
- [04 soft_output](../04_soft_output) — the normal soft decoders, whose
  `dt_soft_bit` fields detect repurposes.
