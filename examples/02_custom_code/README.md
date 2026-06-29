# 02 — custom_code

Define your own convolutional code from generator polynomials instead of using a
preset. Builds the classic K=7, rate-1/2 code with `dt_cc_code_create`, then
encodes and decodes through it exactly as with a standard code.

## What it shows

- `dt_cc_code_create(K, generators, num_generators)` — the custom-code constructor.
- The generator format: one K-bit tap mask per output bit; an output bit is the
  parity (XOR) of the register bits its mask taps. Two generators → rate 1/2.
- A custom code interoperates with every encoder/decoder just like a preset.

## Run

```sh
./build/examples/02_custom_code/02_custom_code
```

## Expected output

```
custom code: K=7, 2 generators -> rate-1/2 (n=2, k reported as 7)
encoded 200 bits -> 412 coded; decoded 206, 0 errors after 2% flips (recovered)
```

## Reading it

- The generators `{0171, 0133}` (octal) are the standard K7 rate-1/2 taps — using
  them here just shows the mechanism; pass any valid masks for a different code.
- `dt_cc_code_n` reports `2` (output bits per input bit) and `dt_cc_code_k` reports
  the memory length `7`. Sender and receiver must share the same code.

## See also

- [The code (`dt_cc_code`)](../../doc/cc/ccode.md) — presets and the custom format.
- [`bench/`](../../bench) searches for good custom generators.
