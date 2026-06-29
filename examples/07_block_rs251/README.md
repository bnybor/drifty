# 07 — block_rs251

The `rs251` Reed–Solomon **block** code — drifty's outer code. A systematic RS(n, k)
over GF(251): k message symbols become an n-symbol codeword with n−k parity
symbols, correcting errors and **erasures** while `2·errors + erasures ≤ n−k`.
Unlike the streaming convolutional codecs, the block interface is buffer-based.

## What it shows

- The block vtable: fill the encoder's `decoded_buf`, call `encode()`, read its
  `encoded_buf`; mirror for the decoder. `dt_result` status (`DT_OK` / `DT_AGAIN` /
  `DT_ERR_DECODE`).
- `dt_bc_rs251_block_encoder_create` / `_decoder_create` / `_soft_decoder_create`.
- Correction at the exact error+erasure budget, a failure just past it, and the
  soft-input decoder.

## Run

```sh
./build/examples/07_block_rs251/07_block_rs251
```

## Expected output

```
RS(40, 24): 184 message bits -> 320 codeword bits, budget n-k=16

  clean                        decode=OK      message=recovered
  4 errors + 8 erasures        decode=OK      message=recovered
  5 errors + 8 erasures        decode=DECODE-ERR message=lost
  soft input (8 erasures)      decode=OK      message=recovered
```

## Reading it

- The message is `(k-1)*8 = 184` bits (GF(251) byte packing); the codeword is
  `n*8 = 320` bits, i.e. 40 symbols of 8 bits each.
- `2·errors + erasures` must stay within `n−k = 16`: 4 errors + 8 erasures = 16
  decodes; 5 errors + 8 erasures = 18 is rejected as `DT_ERR_DECODE`.
- The **soft** decoder takes a `dt_soft_bit` codeword and reads each symbol by
  per-bit argmax, iteratively erasing its least-reliable symbols on a failed
  decode. This is the outer half of the [concatenated stack](../10_concatenated).

## See also

- [`rs251` block codec](../../doc/bc/rs251.md) — the code, the erasure bridge, the
  spare-symbol guard `s`, and the soft decoder.
- [Block interface](../../doc/block.md).
