# 13 — funnel

A **streaming, detection-routed receiver over a full concatenated stack** — the
realistic shape of a drifty receiver, built with the
[`pipe/`](../../doc/pipe/README.md) API. It ties together almost everything the
other examples introduce: an outer block code, a frame codec, two inner
convolutional decoders, the blind detectors, and soft information carried end to
end.

## The stack

Each transmitted signal is protected layer by layer — every codec guards the one
inside it:

```
payload bytes -> rs251 (outer FEC) -> marker (framing) -> cc (inner FEC) -> channel
```

The receiver watches **one continuous channel whose condition changes over time**: a
clean signal, then a battered and drifting one, then dead air, then corrupt noise.
It cannot afford the strongest inner decoder on every bit, so the **funnel** streams
the channel and escalates only as far as each stretch needs, gating with drifty's
two blind [code-presence detectors](../../doc/cc/README.md):

```
detect_clean (cheap):  c_absent - c_erasure > 0.85  -> DROP        (clearly no code)
                       c_erasure - c_absent > 0.85  -> bcjr        (clearly a code)
                       else (uncertain) -> detect_noisy (expensive):
                            c_absent - c_erasure > 0.85 -> DROP     (no signal)
                            else                        -> maxir    (noisy signal)
```

`detect_clean`'s exact parity settles the easy calls but can't tell a *battered*
signal from random, so it defers those to `detect_noisy`, which asks "is there a
signal at all?" — routing a real one to `maxir` (which alone tracks inserted/dropped
bits) and dropping pure noise. The inner decoders are **soft**, so per-bit
reliabilities survive outward: the marker **soft** frame decoder reframes the
recovered stream (its markers re-synchronise past any boundary junk), and the rs251
**soft** block decoder mops up the inner decoder's residual errors via the erasure
bridge.

## What it shows

- **Escalation as resource control.** The clean signal is settled by the two *cheap*
  stages (`detect_clean` + `bcjr`) and never pays for `detect_noisy` or `maxir`;
  only ambiguous stretches escalate. The per-block trace prints which detectors ran.
- **Adaptive routing over time.** One stream, four conditions; the funnel re-routes
  live as the channel changes — clean → bcjr, noisy+drift → maxir, dead air /
  corrupt junk → dropped.
- **The full soft concatenated stack.** `maxir` tracks a **5 % deletion** drift
  channel (which `bcjr` could not follow at all); the marker layer reframes; and
  `rs251` soft-decodes each frame, **mopping up the residual bit errors** the inner
  decoder left — including a deliberate erasure burst.
- **Partial recovery is normal.** A couple of frames the drift shakes loose are not
  recovered here; in a real system a **fountain code upstream of rs251** reconstructs
  the whole message from the frames that survive, so the receiver is built to lose a
  few and keep going.

## Run

```sh
./build/examples/13_funnel/13_funnel
```

## Expected output

```
Streaming one channel whose condition changes over time. Stack per signal:
  payload -> rs251 (outer) -> marker (frames) -> cc (inner) -> channel
Each row is a 2048-bit block - the funnel's live routing as the stream flows.
(c_erasure, c_absent) = (code-present, no-code); B=bcjr M=maxir .=drop(clean) x=drop(noisy).

  block   channel        detect_clean      detect_noisy     -> route
    0   clean signal    (0.99, 0.07)     (skipped)     ->  B
    1   clean signal    (1.00, 0.00)     (skipped)     ->  B
    2   clean signal    (1.00, 0.00)     (skipped)     ->  B
    3   clean signal    (1.00, 0.00)     (skipped)     ->  B
    4   clean signal    (1.00, 0.00)     (skipped)     ->  B
    5   clean signal    (1.00, 0.00)     (skipped)     ->  B
    6   dead air        (0.87, 0.96)     (0.03, 0.95)  ->  x
    7   dead air        (0.86, 1.00)     (0.04, 0.91)  ->  x
    8   noisy+drift     (0.96, 0.30)     (0.75, 0.19)  ->  M
    9   noisy+drift     (0.89, 0.87)     (0.61, 0.24)  ->  M
   10   noisy+drift     (0.86, 1.00)     (0.12, 0.75)  ->  M
   11   noisy+drift     (0.86, 1.00)     (0.15, 0.70)  ->  M
   12   noisy+drift     (0.86, 1.00)     (0.09, 0.81)  ->  M
   13   noisy+drift     (0.86, 1.00)     (0.14, 0.72)  ->  M
   14   dead air        (0.86, 1.00)     (0.09, 0.83)  ->  M
   15   dead air        (0.86, 1.00)     (0.04, 0.92)  ->  x
   16   corrupt junk    (0.01, 1.00)     (skipped)     ->  .
   17   corrupt junk    (0.00, 1.00)     (skipped)     ->  .

Outer recovery (marker reframing + rs251 soft decode) of the decoded signal streams:
  clean  -> bcjr   6/6 frames delimited, 6 full-length; rs251 recovered 6/6 payloads, mopping up 0 inner-decoder bit errors
  noisy  -> maxir  6/6 frames delimited, 4 full-length; rs251 recovered 4/6 payloads, mopping up 62 inner-decoder bit errors
  dead air + junk : dropped (no decoder, no frames)

detect_noisy ran on 10 of 18 blocks - only the ambiguous stretches paid for it. bcjr
carried the clean signal; maxir tracked the 5% drift and rs251 mopped up its
residual errors (including an erasure burst). A few frames the drift shook loose
are not recovered here - upstream of rs251 a fountain code reconstructs the whole
message from the frames that do survive.
```

## Reading it

- **Blocks 0–5 (clean signal).** `detect_clean` reads `(≈1.0, ≈0.0)` — code present,
  wide margin — and commits to `bcjr`. `detect_noisy` and `maxir` never run; the
  cheapest path. All six frames come back exact (0 residual).
- **Blocks 6–7, 15 (dead air).** `detect_clean` is unsure (`≈0.87, ≈1.0`), so it
  escalates; `detect_noisy` finds no code bias and **drops** the blocks. The gaps are
  padded so each signal begins on a block boundary — its sync preamble then lands
  wholly inside one routed block, which `maxir` needs intact to acquire lock.
- **Blocks 8–13 (noisy + drift).** `detect_clean` uncertain → `detect_noisy` confirms
  → `maxir`, which tracks the 5 % deletion drift the `bcjr` path could never follow.
  Four of six frames reframe and decode; `rs251` mops up **62** residual inner-decoder
  bit errors (the erasure burst) to deliver clean payloads.
- **Blocks 16–17 (corrupt junk).** Lone `DT_INVALID` symbols are un-encodable —
  two-sided evidence `detect_clean` rejects outright at the cheap stage.
- **Block 14** shows the detector's lag: one dead-air block still routes to `maxir`
  before `detect_noisy`'s window fills with noise. Its garbage lands *after* the real
  frames, so the reframer ignores it — the framing makes the pipeline robust to exactly
  this kind of boundary slop.

The two lost frames are expected: `maxir` decodes a drift channel near-perfectly or
slips hard, with little in between, and 5 % deletions occasionally shake a frame loose.
That is what the fountain layer above `rs251` is for.

## See also

- [Pipe API (`doc/pipe/`)](../../doc/pipe/README.md) and [12 pipe](../12_pipe) — the
  toolkit this is built from.
- [10 concatenated](../10_concatenated) — the rs251-over-cc soft stack and the
  erasure bridge, without the streaming router.
- [11 detect](../11_detect) — the `detect_clean` / `detect_noisy` detectors alone.
- [09 frames_marker](../09_frames_marker) and [07 block_rs251](../07_block_rs251) —
  the marker frame codec and the rs251 block code on their own.
