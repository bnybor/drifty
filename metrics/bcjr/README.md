# bcjr metrics

Monte-Carlo characterization of the **bcjr** codec — a max-log-MAP / forward-backward
hard-decision decoder — across the channel's **flip** and **erase** rates, for the four
standard codes. Like [viterbi](../viterbi/METRICS.md), bcjr does **not** track inserted
or dropped bits, so only the flip and erase axes are swept. Unlike viterbi it takes a
channel model (`decision_depth`, `p_flip`, `p_erase`) and **blind-acquires** (so the
first `~decision_depth` bits are an acquisition transient, trimmed before comparison);
this harness runs the **matched** model, so there is a single dataset. The reported
metric is the normalized **edit distance** (bcjr's soft lock probability is not swept
here).

## Build & run

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_bcjr_metrics
# dt_bcjr_metrics <trials> <info_bits> <seed> [rate_grids_file]   (defaults 50 1000 0xC0FFEE)
build/metrics/bcjr/dt_bcjr_metrics 30 4000 0xC0FFEE > metrics/bcjr/metrics.csv

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/bcjr/plot_metrics.py metrics/bcjr/metrics.csv -o metrics/bcjr/plots/
```

## In this directory

- `dt_bcjr_metrics.c` — the harness (links the public `drifty::drifty` archive).
- `rate_grids.txt` — the swept flip/erase rates (edit and re-run, no recompile).
- `plot_metrics.py` — renders the CSV into per-axis PNGs.
- `metrics.csv`, `plots/` — the committed sweep and its figures.
- `METRICS.md` — the full analysis and embedded plots.

Full analysis and plots: **[METRICS.md](METRICS.md)**.
