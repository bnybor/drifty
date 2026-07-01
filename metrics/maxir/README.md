# maxir metrics

Monte-Carlo characterization of the **maxir** codec — a drift-tolerant max-log-MAP
(forward-backward) **soft** decoder with re-acquisition — across all four channel
impairments (**flip**, **insert**, **delete**, **erase**), for the four standard codes.
maxir runs over a (state × drift) super-trellis, so like [hybrid](../hybrid/METRICS.md)
it sweeps the drift axes and carries a channel model, with three
[variations](METRICS.md) (`tuned` matched / `untuned` pegged / `overmatched`). It
reports the normalized **edit distance** and a **lock** probability; the plotter also
renders **run length** (`1 / edit`).

## Build & run

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_maxir_metrics
# dt_maxir_metrics <trials> <info_bits> <seed> <variation> [rate_grids_file]
#   variation = matched | pegged | overmatched   (defaults 50 1000 0xC0FFEE matched)
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE matched     > metrics/maxir/tuned/metrics.csv
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE pegged      > metrics/maxir/untuned/metrics.csv
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE overmatched > metrics/maxir/overmatched/metrics.csv

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/maxir/plot_metrics.py metrics/maxir/tuned/metrics.csv -o metrics/maxir/tuned/plots/ --match metrics/maxir/untuned/metrics.csv
# ...and likewise for untuned/ and overmatched/ (see METRICS.md for all three).
```

## In this directory

- `dt_maxir_metrics.c` — the harness (links the public `drifty::drifty` archive).
- `rate_grids.txt` — the swept rates, per variation/metric/axis (edit and re-run, no recompile).
- `plot_metrics.py` — renders a CSV into per-axis PNGs (`--match` shares a y-axis across a pair).
- `tuned/`, `untuned/`, `overmatched/` — each a `metrics.csv` and a `plots/` for one variation.
- `METRICS.md` — the full analysis and embedded plots.

Full analysis and plots: **[METRICS.md](METRICS.md)**.
