# 11 — detect

Blind **code-presence detection** with the [`detect_clean`](../../doc/cc/detect_clean.md)
and [`detect_noisy`](../../doc/cc/detect_noisy.md) meta-codecs. Every other example
encodes or decodes a *known* code; the detect codecs are the odd ones out — given an
arbitrary bit stream with **no prior knowledge or coordination** (no code, rate,
generators, or alignment), each reports per position how confident it is that a
convolutional code is present. They do not recover bit values — they answer "is
there a code here?".

There are two, trading footprint for noise tolerance, and this example shows **when
to reach for which**:

- **`detect_clean`** — exact GF(2) rank deficiency. A few KB, no transform; tolerates
  indels and ~1 % flips. The embeddable default for clean channels (Parts A and B).
- **`detect_noisy`** — parity-check *bias* via a Walsh–Hadamard transform. A ~64 KB
  histogram and more compute, but tolerates flips (~5–8 %), indels (~2–3 %), and
  light combinations of the two (Part C).

## What it shows

- `dt_cc_detect_clean_soft_decoder_create(&p)` / `dt_cc_detect_noisy_soft_decoder_create(&p)`
  — both take the same rich channel model as `hybrid`/`maxir` (here a clean channel),
  driven through the normal soft-decoder vtable, with the same repurposed output
  fields: `c_erasure` = confidence a code **is** present, `c_absent` = confidence it
  is **not** (they need not sum to 1).
- **Part A (clean):** a whole coded stream reads as "present", a random stream as
  "absent".
- **Part B (clean):** a coded segment spliced into a random stream — detect_clean
  **localizes** the code per-position, with no hint of where (or whether) it is.
- **Part C (clean vs noisy):** add bit **flips**, which break detect_clean's exact
  parity, and watch detect_noisy hold on where detect_clean collapses — including
  through a combined flip+drift channel, the case that motivates carrying both noise
  types in one codec.

## Run

```sh
./build/examples/11_detect/11_detect
```

## Expected output

```
Part A - a whole stream, coded vs random (detect_clean):
  coded  (4012 bits): code-present [########################] 1.00   no-code 0.00
  random (4012 bits): code-present [                        ] 0.00   no-code 1.00

Part B - a coded segment spliced into a random stream...
  true coded region: bits [1536, 4348)
  bits  1152.. 1536 (random) [##                      ] 0.08
  bits  1536.. 1920 (coded)  [########################] 1.00
  ...
  bits  3840.. 4224 (coded)  [########################] 1.00
  bits  4224.. 4608 (random) [##########              ] 0.43   <- edge block straddles the boundary

Part C - the same coded stream through a bit-FLIP channel...
  0% flips:  clean [########################] 1.00   noisy [########################] 1.00
  1% flips:  clean [###################     ] 0.80   noisy [########################] 1.00
  3% flips:  clean [######                  ] 0.23   noisy [########################] 0.99
  5% flips:  clean [                        ] 0.00   noisy [################        ] 0.67

  combined 3% flip + 0.5% deletion (flips AND drift at once):
  3987 bits:    clean [                        ] 0.00   noisy [########                ] 0.35
```

## Reading it

- **Part B:** ~0 in the random regions, ~1 across the coded segment. detect_clean
  finds the code's location without being told it exists — the "no coordination"
  property. Blocks that straddle the code/random boundary read a partial value (the
  analysis windows fully inside the code fire; those crossing the edge do not).
- **Part C** is the whole reason there are two codecs. detect_clean reads exact
  parity, so a few flips erase the structure it looks for: code-present fades from
  1.00 to 0 by ~5 % flips. detect_noisy scores parity *bias*, which flips only
  weaken — it stays near 1.00 through 3 % and is still firing at 5 %. Under the
  combined flip+deletion channel detect_clean reads a flat 0 while detect_noisy keeps
  measurable evidence (and stays well clear of what it reports on a random stream).
- A mean of `1.00`/`0.00` is the analyzable interior; the very first/last positions
  have too few neighbours for a full window, so a detector abstains there — visible
  as the partial edge blocks in Part B.

## Method & limits

A convolutional code is linear, so it leaves a parity-check signature in the stream.
**detect_clean** ([page](../../doc/cc/detect_clean.md)) measures it *exactly*: windows
of coded bits are GF(2) **rank-deficient** while random bits are full-rank; it slides
short windows over candidate block sizes and takes each position's max deficiency.
Sliding buys indel tolerance — only a *locally* clean aligned run is needed — but a
flip is an independent row in every covering window, so detect_clean is brittle to
flips (~1 %). **detect_noisy** ([page](../../doc/cc/detect_noisy.md)) measures it
*approximately*: the **bias** of low-weight checks, found for all candidates at once
by a Walsh–Hadamard transform of the window's bit-slice histogram. A flip only
shrinks a check's bias rather than destroying it, so detect_noisy degrades gracefully
through flips, indels, and their combination — at a ~64 KB / heavier-compute cost.

## See also

- [`detect_clean`](../../doc/cc/detect_clean.md) and
  [`detect_noisy`](../../doc/cc/detect_noisy.md) references, and
  [Soft decoding](../../doc/soft_decoding.md).
- [04 soft_output](../04_soft_output) — the normal soft decoders, whose
  `dt_soft_bit` fields detect repurposes.
