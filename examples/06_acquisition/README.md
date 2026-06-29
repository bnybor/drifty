# 06 — acquisition

The lock signal: drifty's lock-tracking decoders acquire sync **blindly** — they
lock on whether you start at the head of a stream or tap into one mid-flight — and
report, per bit, how confident they are via `c_locked`. When they are not tracking
a valid stream of the code, they emit `DT_ABSENT` rather than guessing.

## What it shows

- **Part A:** `c_locked` climbing from ~0 (acquiring) to ~1 (locked) over the
  warm-up — that ramp *is* blind acquisition.
- **Part B:** decoding a stream encoded with a **different** code; the decoder never
  locks (`c_locked` stays low) and the hard decoder emits `DT_ABSENT`.

## Run

```sh
./build/examples/06_acquisition/06_acquisition
```

## Expected output

```
Part A - acquiring lock on our own stream (c_locked over time):
  bit   0:  c_locked=0.93  -> 1
  bit  16:  c_locked=1.00  -> 1
  ...
Part B - decoding a SIBLING code's stream with the wrong decoder:
  mean c_locked = 0.03 (low: never locks)
  304 of 304 hard outputs read DT_ABSENT - the decoder reports it is not
  tracking this code rather than emitting confident wrong bits.
```

## Reading it

- A rate-1/5 K5 code is used because its lock is sharply code-specific, which makes
  Part B vivid: a sibling code (same family, different generators) drives lock to
  ~0 and the whole output reads `DT_ABSENT`.
- `DT_ABSENT` is the decoder's "I cannot place this position" — the honest answer
  when it has lost (or never had) lock. It surfaces in the **hard** decision when
  `c_locked` drops below the lock floor; the soft `c_locked` carries the same signal
  continuously.

## See also

- [Soft decoding](../../doc/soft_decoding.md) and [Symbols (`bit`)](../../doc/bit.md)
  for `c_locked` / `DT_ABSENT`.
- [03 decoders](../03_decoders) — `viterbi` starts from a known state and does not
  blind-acquire; the other four do.
