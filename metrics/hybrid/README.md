# hybrid metrics

Monte-Carlo characterization of the **hybrid** codec — a drift-tracking **soft**
decoder — across all four channel impairments (**flip**, **insert**, **delete**,
**erase**), for the four standard codes. Because hybrid tracks inserted/dropped bits and
carries a channel model, the harness sweeps the drift axes too and runs three
[variations](METRICS.md#variations): `tuned` (matched model), `untuned` (pegged model),
and `overmatched` (matched model over a clean channel). It reports the normalized
**edit distance** and a **lock** probability; the plotter also renders **run length**
(`1 / edit`).

> Note: this harness's target and source are named `dt_metrics` (not
> `dt_hybrid_metrics`).

## Build & run

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_metrics
# dt_metrics <trials> <info_bits> <seed> <variation> [rate_grids_file]
#   variation = matched | pegged | overmatched   (defaults 50 1000 0xC0FFEE pegged)
build/metrics/hybrid/dt_metrics 30 4000 0xC0FFEE matched     > metrics/hybrid/tuned/metrics.csv
build/metrics/hybrid/dt_metrics 30 4000 0xC0FFEE pegged      > metrics/hybrid/untuned/metrics.csv
build/metrics/hybrid/dt_metrics 30 4000 0xC0FFEE overmatched > metrics/hybrid/overmatched/metrics.csv

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/hybrid/plot_metrics.py metrics/hybrid/tuned/metrics.csv -o metrics/hybrid/tuned/plots/ --match metrics/hybrid/untuned/metrics.csv
# ...and likewise for untuned/ and overmatched/ (see METRICS.md for all three).
```

## In this directory

- `dt_metrics.c` — the harness (links the public `drifty::drifty` archive).
- `rate_grids.txt` — the swept rates, per variation/metric/axis (edit and re-run, no recompile).
- `plot_metrics.py` — renders a CSV into per-axis PNGs (`--match` shares a y-axis across a pair).
- `tuned/`, `untuned/`, `overmatched/` — each a `metrics.csv` and a `plots/` for one variation.
- `METRICS.md` — the full analysis and embedded plots.

Full analysis and plots: **[METRICS.md](METRICS.md)**.
