# viterbi metrics

Monte-Carlo characterization of the **viterbi** codec — a plain Viterbi hard-decision
decoder — across the channel's **flip** and **erase** rates, for the four standard
codes. The simplest harness here: viterbi takes **no channel model** (nothing to tune,
so a single dataset — no tuned/untuned/overmatched variations), emits **no lock
probability**, and does **not** track inserted or dropped bits (flip and erase axes
only). The reported metric is the normalized **edit distance** between decoded and sent
bits.

## Build & run

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_viterbi_metrics
# dt_viterbi_metrics <trials> <info_bits> <seed> [rate_grids_file]   (defaults 50 1000 0xC0FFEE)
build/metrics/viterbi/dt_viterbi_metrics 30 4000 0xC0FFEE > metrics/viterbi/metrics.csv

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/viterbi/plot_metrics.py metrics/viterbi/metrics.csv -o metrics/viterbi/plots/
```

## In this directory

- `dt_viterbi_metrics.c` — the harness (links the public `drifty::drifty` archive).
- `rate_grids.txt` — the swept flip/erase rates (edit and re-run, no recompile).
- `plot_metrics.py` — renders the CSV into per-axis PNGs.
- `metrics.csv`, `plots/` — the committed sweep and its figures.
- `METRICS.md` — the full analysis and embedded plots.

Full analysis and plots: **[METRICS.md](METRICS.md)**.
