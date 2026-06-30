# detect_noisy metrics

`metrics/detect_noisy/dt_detect_noisy_metrics.c` measures the **detect_noisy** blind
code-presence detector ([doc](../../doc/cc/detect_noisy.md)) — parity-check **bias**
scored by a fast Walsh–Hadamard transform — across the four channel impairments, for
each standard code. It shares the framework and CLI of the FEC harnesses, but
detect_noisy does not recover bits, so the metrics are detection confidence rather
than edit distance. It is the noise-tolerant sibling of
[detect_clean](../detect_clean/METRICS.md): same metrics and plots, with the flip
and erasure knees pushed substantially further out (indels comparable) at a ~64 KB /
heavier-compute cost.

> [!NOTE]
> The committed CSVs and plots are the **full sweep** (`40 6000 0xC0FFEE`) over the
> shipped `rate_grids.txt`. For fast iteration on the grids, a coarse pass (e.g.
> `4 2500`) regenerates everything in seconds.

## What is measured

detect_noisy answers "is a convolutional code present?". For each point we run **two**
streams through the channel — a **coded** one and a same-length **pure random** one —
and average each of the detector's two soft confidences over the stream interior
(the head/tail abstain transient is trimmed):

- **present** (`c_erasure`) — confidence a code **is** present. High on coded, ~0 on
  random.
- **absent** (`c_absent`) — confidence a code is **not** present. ~0 on coded, high
  on random.

Emitting the random stream's means alongside the coded ones gives every plot a
pure-random **baseline**: detection works to the extent the coded curves stand clear
of it. Each axis (flip / insert / delete / erase) is swept independently.

## Variations (the decoder's channel model)

The detector takes a channel model, selected by a variation — the **parameterization
axis**:

- **pegged** (`untuned/`) — model fixed at a flat 1% on every impairment, whatever
  the channel does.
- **matched** (`tuned/`) — the swept impairment's model rate tracks the channel; the
  others stay at the 1% floor.

The model only calibrates **`c_absent`**: detect_noisy damps its no-code confidence by
a detectability factor `(1 − 2p)^W_ref` when it expects flips/overwrites (heavy
expected flips could have pushed a real code's bias down into the random floor). So
**matched FLIP and ERASE** sweeps pull the `c_absent` random baseline down as the
rate climbs, while **`c_erasure` is identical across variations** (an observed bias
is real regardless of what you expected) and the **INSERT/DELETE** axes barely move
(indels are tolerated, not a reason to doubt "no code").

## Running

```sh
# Build the harness (off by default).
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_detect_noisy_metrics

# Full sweep (what is committed):
build/metrics/detect_noisy/dt_detect_noisy_metrics 40 6000 0xC0FFEE pegged  > metrics/detect_noisy/untuned/metrics.csv
build/metrics/detect_noisy/dt_detect_noisy_metrics 40 6000 0xC0FFEE matched > metrics/detect_noisy/tuned/metrics.csv

# Coarse pass (fast, for iterating on the grids):
build/metrics/detect_noisy/dt_detect_noisy_metrics 4 2500 0xC0FFEE pegged  > metrics/detect_noisy/untuned/metrics.csv
build/metrics/detect_noisy/dt_detect_noisy_metrics 4 2500 0xC0FFEE matched > metrics/detect_noisy/tuned/metrics.csv

# Plot both confidences (with the random baseline) into each variation's plots/.
python3 -m venv .venv && .venv/bin/pip install matplotlib   # once
.venv/bin/python metrics/detect_noisy/plot_metrics.py metrics/detect_noisy/untuned/metrics.csv -o metrics/detect_noisy/untuned/plots/
.venv/bin/python metrics/detect_noisy/plot_metrics.py metrics/detect_noisy/tuned/metrics.csv   -o metrics/detect_noisy/tuned/plots/
```

Every run is reproducible from its `seed` (each point owns a derived PRNG stream, so
the sweep fans out over OpenMP without changing the numbers). The per-axis rate grids
are read from `rate_grids.txt` (or a 5th-argument path), so a sweep retunes without
recompiling. CSV columns: `code, variation, axis, rate, dec_p_flip, dec_p_ins,
dec_p_del, dec_p_ovr_erase, trials, coded_present, coded_absent, random_present,
random_absent` (the `dec_*` columns record the model each point ran with).

## Reading the plots

One figure per (metric, axis): a solid curve per code (the coded value) and a dashed
**random baseline**. Unlike detect_clean, the random `present` baseline sits at a
small positive floor (~0.03–0.05 — the max bias over `2^14` candidates on random
data), so the coded curves clear it by less at the clean end but stay separated far
longer: detect_noisy holds to ~5–8 % flips, ~2–3 % indels, ~16 % erasures. (On the
erase axis that floor is *not* flat — past ~10 % erasure the forced-zero bits are
themselves biased toward 0, so the random baseline climbs, reaching ~0.35 by 26 %;
see `present_vs_erase`.)

### Code-present confidence (`c_erasure`)

Identical between variations (the model does not affect it). Coded confidence decays
**gracefully** toward the random floor; shorter-constraint codes (lighter parity
checks, less affected by `(1−2p)^w`) hold longer.

| axis | pegged | matched |
|---|---|---|
| flip   | <img src="untuned/plots/present_vs_flip.png"   width="420"> | <img src="tuned/plots/present_vs_flip.png"   width="420"> |
| insert | <img src="untuned/plots/present_vs_insert.png" width="420"> | <img src="tuned/plots/present_vs_insert.png" width="420"> |
| delete | <img src="untuned/plots/present_vs_delete.png" width="420"> | <img src="tuned/plots/present_vs_delete.png" width="420"> |
| erase  | <img src="untuned/plots/present_vs_erase.png"  width="420"> | <img src="tuned/plots/present_vs_erase.png"  width="420"> |

### No-code confidence (`c_absent`)

Where the variation bites. Under **pegged** the random baseline is flat (fixed
model); under **matched** the flip and erase baselines **decay** as the model expects
more corruption — the calibration that keeps a noisy channel from being wrongly
declared code-free. Insert/delete baselines stay flat in both (indels don't damp).

| axis | pegged | matched |
|---|---|---|
| flip   | <img src="untuned/plots/absent_vs_flip.png"   width="420"> | <img src="tuned/plots/absent_vs_flip.png"   width="420"> |
| insert | <img src="untuned/plots/absent_vs_insert.png" width="420"> | <img src="tuned/plots/absent_vs_insert.png" width="420"> |
| delete | <img src="untuned/plots/absent_vs_delete.png" width="420"> | <img src="tuned/plots/absent_vs_delete.png" width="420"> |
| erase  | <img src="untuned/plots/absent_vs_erase.png"  width="420"> | <img src="tuned/plots/absent_vs_erase.png"  width="420"> |

## Iterating

The engine constants worth sweeping live in `src/cc/detect_noisy/decode.c`
(`DET_LC` — transform order / histogram size, `DET_L` — window, `DET_STEP`,
`DET_K_LOST` — the c_erasure calibration, `DET_WREF` — the c_absent detectability
weight). Edit, rebuild the target, re-run — the `present` curves move with
`DET_K_LOST`/`DET_LC`/`DET_L`, the `absent` baselines with `DET_WREF`. Retune the
channel sweep in `rate_grids.txt` (read at startup) to zoom a knee without
recompiling.
