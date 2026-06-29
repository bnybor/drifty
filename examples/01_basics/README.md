# 01 — basics

The "hello world" of drifty: encode a bit stream, send it over a noisy channel,
and decode it back. It picks a ready-made convolutional code, encodes a random
message through the shared encoder, flips ~3% of the coded bits, and recovers the
original with the simplest decoder ([`viterbi`](../../doc/cc/viterbi.md)).

## What it shows

- Choosing a preset code with `dt_cc_code_create_standard`, and `dt_cc_code_n` /
  `dt_cc_code_k`.
- The shared encoder (`dt_cc_encoder_create`) driven `begin → encode → finalize`.
- A hard decoder (`dt_cc_viterbi_decoder_create`) driven `begin → decode → finalize`.
- That a convolutional code corrects flipped bits.

## Run

```sh
./build/examples/01_basics/01_basics
```

## Expected output

```
code: K7 rate-1/2  (2 coded bits per input bit)
encoded 240 info bits -> 492 coded bits

clean channel:   recovered 246 bits, 0 errors in [42, 240)
3% bit flips:    recovered 246 bits, 0 errors in [42, 240)  (all corrected)
```

## Reading it

- `492 = (240 + 6) * 2`: the encoder appends a `K-1 = 6` bit flush tail to drain
  its shift register, and emits `n = 2` coded bits per input bit.
- Output **trails** input by a look-ahead delay, so the first `~6*K = 42` recovered
  bits are warm-up and excluded from the error check; the settled region `[42,
  240)` recovers with zero errors on both the clean and 3%-flip channels.

## See also

- [Convolutional coding](../../doc/cc/README.md) and the
  [shared code](../../doc/cc/ccode.md) / [encoder](../../doc/cc/encoder.md).
- Next: [02 custom_code](../02_custom_code) and [03 decoders](../03_decoders).
