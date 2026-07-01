# 14 — pipeline

Example 13's detection-routed receiver, rebuilt so the **entire funnel is one composed
pipe graph**. Where [13](../13_funnel) drove the routing with imperative code in `main`,
here the caller wires the graph once and then only pushes channel bits and reads
recovered frames from the final frame pipe — the graph detects, switches between the
cheap and robust decoders, and reframes internally. It is the capstone for the
[`pipe/`](../../doc/pipe/README.md) API: pipelines, splitters, two diverters, two
controlling **executor** pipes, a combiner, the [frame pipes](../../doc/pipe/README.md),
and a pipe container, plus the [`dt_container`](../../include/drifty/container.h)
ownership bag for one-shot cleanup.

## The graph

The decode side mirrors [`layout.dot`](layout.dot) (rendered in `layout.dot.png`):

```
channel -> splitter1 ─┬─> detect_clean -> executor1 ──steers──▶ diverter1
                      └─────────────────────────────────────▶  diverter1 ─┬─> bcjr
                                                                          ├─> drain            (no code)
                                                                          └─> splitter2 ─┬─> detect_noisy -> executor2 ──steers──▶ diverter2
                                                                                         └────────────────────────────────────▶  diverter2 ─┬─> maxir
                                                                                                                                             └─> drain  (no signal)
                          bcjr, maxir -> combiner1 -> frame_decoder   (the only pipe the caller sees)
```

- **`splitter1`** tees each block to `detect_clean` and to `diverter1`.
- **`executor1`** reads `detect_clean`'s confidence and steers `diverter1` to **bcjr**
  (clearly a code), **drop** (clearly not), or **`splitter2`** (uncertain — escalate).
- Only the escalate path reaches **`detect_noisy`**, so the expensive detector runs
  only where it's needed. **`executor2`** steers **`diverter2`** to **maxir** or drop.
- **`combiner1`** merges the bcjr and maxir soft streams into the **`frame_decoder`**,
  the marker soft-frame pipe the caller walks (`tick`/`advance`) for frames, then
  rs251-decodes.

The executors also re-acquire their decoder (`begin`) when a signal run restarts after
dead air. The short pipelines and routing pipes live in a `dt_pipe_container` the caller
ticks once per block; ticking it drives one block from channel to frame.

## Files

- **`main.c`** — the driver: builds the time-varying channel and prints the routing
  trace. Never touches the detectors, diverters, or decoders.
- **`funnel.{c,h}`** — the decode graph as one object (`dt_funnel_create` / `_input` /
  `_tick` / `_finalize` / `_frames` / `_destroy`), including the two executor callbacks
  and the `dt_container` that frees every codec and pipe with one call.
- **`stack.{c,h}`** — the outer rs251 + marker stack: transmit through the frame
  **encoder** pipe, receive through the frame **soft-decoder** pipe.

## Run

```sh
./build/examples/14_pipeline/14_pipeline
```

## Expected output

```
The whole funnel is one composed pipe graph (see layout.dot); this driver only
pushes channel blocks and reads frames back. Each row is a 2048-bit block.
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

From the funnel's final frame pipe: 10 frames delimited, 10 of 12 payloads recovered;
rs251 mopped up 62 inner-decoder bit errors. detect_noisy ran on 10 of 18 blocks
(only where detect_clean could not decide). A few frames the drift shook loose are
not recovered here - a fountain code upstream of rs251 rebuilds the whole message.
```

## Reading it

Same channel and same result as [example 13](../13_funnel) — clean signal → bcjr, the
5 %-drift signal → maxir with an rs251 mop-up, dead air and junk dropped — but every
routing decision here is made *inside the graph* by the executors steering the
diverters, and `detect_noisy` is reached only through `diverter1`'s escalate branch (it
ran on 10 of 18 blocks). The caller's view is just: push a block, tick, and later walk
the one frame pipe the graph hands back.

## See also

- [13 funnel](../13_funnel) — the same receiver as an imperative loop; start there.
- [Pipe API (`doc/pipe/`)](../../doc/pipe/README.md) — the pipelines, executors,
  diverters, combiner, and frame pipes this graph is built from.
- [`container.h`](../../include/drifty/container.h) — the ownership bag used for cleanup.
