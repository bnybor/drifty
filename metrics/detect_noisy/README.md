# detect_noisy metrics

Monte-Carlo characterization of the **detect_noisy** blind code-presence detector
([doc](../../doc/cc/detect_noisy.md)) — parity-check **bias** scored by a fast
Walsh–Hadamard transform — across the channel's **flip / insert / delete / erase /
invalid** axes, for each standard code. It is the noise-tolerant sibling of
[detect_clean](../detect_clean/METRICS.md): the **same** metrics, plots, and CLI, but
with the flip and erasure knees pushed substantially further out (indels comparable), at
a ~64 KB / heavier-compute cost. detect_noisy does not recover bits, so the metrics are
**detection consistency** — the **present** (`c_erasure`) and **absent** (`c_absent`)
reads, each averaged over a coded stream and a pure-random baseline. Two
[variations](METRICS.md) are shipped: `tuned` (matched model) and `untuned` (pegged
model).

## Build & run

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_detect_noisy_metrics
# dt_detect_noisy_metrics <trials> <info_bits> <seed> <variation> [rate_grids_file]
#   variation = matched | pegged   (full committed sweep: 40 6000 0xC0FFEE)
build/metrics/detect_noisy/dt_detect_noisy_metrics 40 6000 0xC0FFEE matched > metrics/detect_noisy/tuned/metrics.csv
build/metrics/detect_noisy/dt_detect_noisy_metrics 40 6000 0xC0FFEE pegged  > metrics/detect_noisy/untuned/metrics.csv

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/detect_noisy/plot_metrics.py metrics/detect_noisy/tuned/metrics.csv   -o metrics/detect_noisy/tuned/plots/
.venv/bin/python metrics/detect_noisy/plot_metrics.py metrics/detect_noisy/untuned/metrics.csv -o metrics/detect_noisy/untuned/plots/
```

A coarse pass (e.g. `4 2500`) regenerates everything in seconds for iterating on the grids.

## In this directory

- `dt_detect_noisy_metrics.c` — the harness (links the public `drifty::drifty` archive).
- `rate_grids.txt` — the swept rates, per variation/read/axis (edit and re-run, no recompile).
- `plot_metrics.py` — renders the two consistency reads (with the random baseline) into per-axis PNGs.
- `tuned/`, `untuned/` — each a `metrics.csv` and a `plots/` (`present_vs_*` / `absent_vs_*`).
- `METRICS.md` — the full analysis and embedded plots.

Full analysis and plots: **[METRICS.md](METRICS.md)**. The cheap / low-noise sibling is
[detect_clean](../detect_clean/METRICS.md).
