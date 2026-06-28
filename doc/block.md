# drifty — block interface

The block codec interface: fixed-size `(k, n)` blocks rather than a flowing
stream. Each codec owns its input and output buffers; you fill one, run, and read
the other. Three function-pointer vtables over the `dt_bit` alphabet
([symbols](bit.md)):

- `dt_block_encoder` ([`block_encoder.h`](../include/drifty/block_encoder.h))
- `dt_block_decoder` ([`block_decoder.h`](../include/drifty/block_decoder.h))
- `dt_block_soft_decoder`
  ([`block_soft_decoder.h`](../include/drifty/block_soft_decoder.h)) — soft-input:
  its received buffer holds `dt_soft_bit` consistencies rather than hard `dt_bit`s,
  and it still produces a hard decoded output.

The Reed–Solomon [`rs251`](rs/rs251.md) codec implements the block encoder and
decoder.

## Buffers and lengths

Every block codec exposes its two buffers and their fixed bit-lengths:

| Accessor | Returns |
|----------|---------|
| `decoded_len()` / `encoded_len()` | the block's decoded / encoded size, in bits (`size_t`) |
| `decoded_buf()` / `encoded_buf()` | the codec-owned `dt_bit` buffer (the soft decoder's `encoded_buf()` is `dt_soft_bit`) |

The buffers are owned by the codec and fixed for its life — unlike the streaming
interface there is no per-call length. The *decoded* side is the information block,
the *encoded* side the coded block, on both the encoder and the decoder.

## Driving a block codec

```c
/* encoder: fill decoded, run, read encoded */
dt_bit *in = enc->decoded_buf(enc);        /* decoded_len() bits */
/* ... fill in ... */
while (enc->encode(enc) == DT_AGAIN) { }   /* run to completion */
dt_bit *out = enc->encoded_buf(enc);       /* encoded_len() bits */

/* decoder: fill encoded, run, read decoded */
dt_bit *rx = dec->encoded_buf(dec);        /* encoded_len() bits */
/* ... fill rx ... */
dt_result r = dec->decode(dec);
if (r == DT_OK) {
    dt_bit *msg = dec->decoded_buf(dec);   /* decoded_len() bits */
}
```

`encode()` / `decode()` return a [`dt_result`](../include/drifty/result.h): `DT_OK`
when done, `DT_AGAIN` if the operation is incremental and has more work (call
again), or a negative `DT_ERR_*` on failure (e.g. `DT_ERR_DECODE` for an
unrecoverable block). `reset()` clears in-progress state so the codec can be reused
on a fresh block — overwrite the input buffer, `reset()`, run again.

> A given codec may complete in a single call — `rs251`, for instance, never
> returns `DT_AGAIN` — but driving `encode` / `decode` in the
> `while (… == DT_AGAIN)` loop is always correct.

## See also

- [`rs/rs251.md`](rs/rs251.md) — the Reed–Solomon block codec that implements this
  interface.
- [Symbols](bit.md) — `dt_bit` and the soft `dt_soft_bit`.
- [Streaming interface](stream.md) — the continuous-stream alternative.
