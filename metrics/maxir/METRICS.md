# maxir metrics

`metrics/maxir/dt_maxir_metrics.c` measures how well the **maxir** codec — a
drift-tolerant max-log-MAP (forward-backward) decoder with soft output and
re-acquisition — recovers a message across the four channel impairments it is built
to survive. It is the maxir counterpart of [../hybrid/METRICS.md](../hybrid/METRICS.md):
same Monte-Carlo framework (random message → encode → channel → decode), same axes,
same two metrics, and the same `[trials] [info_bits] [seed] [variation] [grids]`
CLI. maxir runs the decoder over a (state × drift) super-trellis, so unlike
[../bcjr/METRICS.md](../bcjr/METRICS.md) it sweeps **insert** and **delete** in
addition to **flip** and **erase**.

Two metrics are reported per point:

- **edit** — normalized edit (Levenshtein) distance between decoded and original
  message bits, divided by message bits. The recovery-quality metric.
- **lock** — mean of the decoder's soft lock probability `c_locked` over the
  stream. How confidently it believes it is tracking, independent of whether the
  bits are right.

Like the hybrid harness, the decoder takes a rich channel model (`decision_depth`,
`max_drift`, and per-symptom probabilities `p_flip`, `p_ins_*`, `p_del`,
`p_ovr_*`). The CLI selects a **variation** that fixes how the decoder's model
relates to the channel:

- `matched` (the **tuned** model, used here) — the decoder's rate for the swept
  impairment tracks the channel's rate; the others stay at small floors.
- `pegged` / `overmatched` — model fixed low / fixed high, to show sensitivity to
  a mismatched model.

> [!NOTE]
> This is a **coarse first pass**: the committed CSV in `tuned/metrics.csv` is the
> `matched` variation only, on a sparse grid (~10 rates per axis) at 6 trials per
> point and a short message — enough to confirm the curve shapes. The maxir
> super-trellis is slow, so a full sweep (the shipped `rate_grids.txt`, all
> variations, more trials) is left for a dedicated run; use the commands below.
> Plots are not committed yet (matplotlib was unavailable); generate them after the
> full sweep.

```sh
# Build the harness (off by default) and run a sweep to a CSV.
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_maxir_metrics
# dt_maxir_metrics <trials> <info_bits> <seed> <variation> <rate_grids_file>
#   defaults: 50 1000 0xC0FFEE matched, grids = metrics/maxir/rate_grids.txt
#   (run from the repo root; the grid file holds one block per variation)
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE matched > metrics/maxir/tuned/metrics.csv

# Plot the edit and lock metrics (one curve per code). Needs matplotlib:
python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/maxir/plot_metrics.py metrics/maxir/tuned/metrics.csv -o metrics/maxir/tuned/plots/
```

Every run is reproducible from its `seed`: the sweep is fanned out across cores
with OpenMP when available, and each point owns a seeded PRNG stream, so a seed
reproduces every row exactly regardless of thread count. The rate grids are read at
startup from `metrics/maxir/rate_grids.txt` (or a 5th-argument path); each line is
`<variation> <metric> <axis>  <rate> <rate> ...` (`#` begins a comment), so the
sweep retunes without recompiling.

The CSV columns are `code, metric, axis, rate, dec_p_flip, dec_p_ins, dec_p_del,
dec_p_ovr_erase, decision_depth, max_drift, trials, ref_bits, edit_distance,
edit_rate, lock_bits, mean_lock` — the decoder-model columns record the model each
point ran with, and the `edit` / `lock` rows leave the other metric's columns
blank. The plotter reads by column name and is shared with the other codecs.

## Coarse first-pass shapes

In the committed `matched` run the expected shapes hold: on every axis each code
holds near-zero edit rate up to a per-code knee, then climbs, with tolerance
scaling with redundancy. The drift axes (insert / delete) are the point of maxir —
the no-drift codecs cannot sweep them at all — and it tracks them well below their
knees. The **lock** metric stays high at low impairment, dips through the
mid-range where tracking is hardest, and recovers toward 1.0 at extreme insert /
delete rates, where a near-constant impairment is itself a consistent (and so
trackable) drift. The 6-trial counts are noisy at the high-rate tail (e.g. a point
or two that dips back); read shapes, not individual points, until the full sweep.
