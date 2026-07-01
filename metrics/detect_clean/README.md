# detect_clean metrics

Monte-Carlo characterization of the **detect_clean** blind code-presence detector
([doc](../../doc/cc/detect_clean.md)) — exact GF(2) sliding-window rank deficiency —
across the channel's **flip / insert / delete / erase / invalid** axes, for each
standard code. detect_clean does not recover bits, so the metrics are **detection
consistency**, not edit distance: at each point the harness runs a **coded** stream and
a same-length **pure-random** stream through the channel and averages the detector's two
reads over the interior —

- **present** (`c_erasure`) — consistency with a code being present, and
- **absent** (`c_absent`) — consistency with random —

so every plot carries a random baseline (detection works to the extent the coded curves
stand clear of it). Two [variations](METRICS.md) are shipped: `tuned` (matched model)
and `untuned` (pegged model).

## Build & run

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_detect_clean_metrics
# dt_detect_clean_metrics <trials> <info_bits> <seed> <variation> [rate_grids_file]
#   variation = matched | pegged   (full committed sweep: 40 6000 0xC0FFEE)
build/metrics/detect_clean/dt_detect_clean_metrics 40 6000 0xC0FFEE matched > metrics/detect_clean/tuned/metrics.csv
build/metrics/detect_clean/dt_detect_clean_metrics 40 6000 0xC0FFEE pegged  > metrics/detect_clean/untuned/metrics.csv

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/detect_clean/plot_metrics.py metrics/detect_clean/tuned/metrics.csv   -o metrics/detect_clean/tuned/plots/
.venv/bin/python metrics/detect_clean/plot_metrics.py metrics/detect_clean/untuned/metrics.csv -o metrics/detect_clean/untuned/plots/
```

A coarse pass (e.g. `4 2500`) regenerates everything in seconds for iterating on the grids.

## In this directory

- `dt_detect_clean_metrics.c` — the harness (links the public `drifty::drifty` archive).
- `rate_grids.txt` — the swept rates, per variation/read/axis (edit and re-run, no recompile).
- `plot_metrics.py` — renders the two consistency reads (with the random baseline) into per-axis PNGs.
- `tuned/`, `untuned/` — each a `metrics.csv` and a `plots/` (`present_vs_*` / `absent_vs_*`).
- `METRICS.md` — the full analysis and embedded plots.

Full analysis and plots: **[METRICS.md](METRICS.md)**. The noise-tolerant sibling is
[detect_noisy](../detect_noisy/METRICS.md).
