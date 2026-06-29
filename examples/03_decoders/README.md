# 03 — decoders

All five convolutional decoders, side by side, on the two channel types they are
built for. drifty shares one encoder but offers five decoders that differ in the
damage they correct:

- **viterbi, bcjr** — flips and erasures on a bit-**aligned** channel.
- **vindel, hybrid, maxir** — additionally track **drift**: inserted / dropped bits
  that slide the stream out of alignment.

The same message is sent over a flip+erasure channel (no drift) and over an
insert+delete channel (drift); each decoder runs on the channel it suits.

## What it shows

- Every decoder factory: `dt_cc_viterbi_decoder_create` (no params),
  `dt_cc_bcjr_decoder_create`, `dt_cc_vindel_decoder_create`,
  `dt_cc_hybrid_decoder_create`, `dt_cc_maxir_decoder_create`, each driven through
  the same `dt_stream_decoder` vtable.
- The alignment-vs-value distinction: drift tolerance is what separates the trio
  from viterbi/bcjr.

## Run

```sh
./build/examples/03_decoders/03_decoders
```

## Expected output

```
aligned channel  (3% flips, 5% erasures, NO drift):
  viterbi  recovered  506 bits,   0 errors in [84, 500)   OK
  bcjr     recovered  506 bits,   0 errors in [84, 500)   OK

drifting channel (1% flips, ~1% inserts, ~1% deletes):
  (channel length 1518 -> 1518 bits from indels)
  vindel   recovered  506 bits,   0 errors in [84, 500)   OK
  hybrid   recovered  506 bits,   0 errors in [84, 500)   OK
  maxir    recovered  506 bits,   0 errors in [84, 500)   OK
```

## Reading it

- The drift channel's length changes from the inserted/dropped bits; the
  drift-tolerant decoders re-anchor and still recover the message in order.
- This example uses a **rate-1/3** code. Indel tolerance grows with code
  redundancy, so a lower-rate code gives the drift decoders more margin — a
  rate-1/2 code is far weaker on the same indel channel (the metrics docs
  recommend K5 rate-1/5 for heavy drift).
- Picking the least-capable decoder that covers your channel is the cheapest; see
  the [choosing-a-decoder guide](../../doc/cc/README.md#choosing-a-decoder).

## See also

- [Convolutional coding](../../doc/cc/README.md) and the per-codec pages.
- [metrics/](../../metrics) quantifies each decoder vs each channel.
