# 04 — soft_output

Per-bit *consistencies* instead of a hard 0/1. A soft decoder reports, for every
recovered position, a `dt_soft_bit`: a graded value in `[0,1]` for each hypothesis
(`c_true` / `c_false` / `c_erasure` / `c_invalid` / `c_absent`) plus `c_locked`.
These are goodness-of-fit values, **not** a probability split — they need not sum
to 1. Soft output keeps the value information a hard decision throws away.

## What it shows

- The soft decoder (`dt_cc_hybrid_soft_decoder_create`) and the `dt_soft_bit`
  fields, on confident bits and on bits inside an erasure burst.
- That the same soft interface is offered by `bcjr` and `maxir`
  (`dt_cc_bcjr_soft_decoder_create`, `dt_cc_maxir_soft_decoder_create`).

## Run

```sh
./build/examples/04_soft_output/04_soft_output
```

## Expected output

```
confident bits on a clean stretch (winner ~1, loser ~0, locked ~1):
  bit  60:  true=1.00 false=0.00 erasure=0.00 invalid=0.00 absent=0.00  locked=1.00  -> 1
  ...
bits inside the erasure burst [150,168) - the value evidence is gone,
so true/false both stay high (c_erasure leads) or lock dips:
  bit 156:  true=1.00 false=1.00 erasure=1.00 invalid=0.00 absent=0.25  locked=0.75  -> E
  ...
bcjr and maxir expose the identical soft interface; settled bit 60:
  bcjr : -> 1 (locked=1.00)
  maxir: -> 1 (locked=1.00)
```

## Reading it

- **Confident bit:** the winning value reads ~1, the loser ~0, `c_locked` ~1.
- **Inside the burst:** the value evidence is gone, so `c_true` and `c_false` both
  stay high and their agreement `c_erasure` leads — the record honestly says "I
  can't tell," rather than committing to a value.
- The `-> ` column is the hard symbol the decoder would emit (the
  recoverability-first projection of these fields, mirrored by `ex_hard_of` in
  [util.h](../util.h)). An outer code instead consumes the graded values directly
  (see [10 concatenated](../10_concatenated)).

## See also

- [Soft decoding](../../doc/soft_decoding.md) — what the consistencies mean and how
  `c_absent` differs across decoders.
- [Symbols (`bit`)](../../doc/bit.md) — the `dt_soft_bit` field reference.
