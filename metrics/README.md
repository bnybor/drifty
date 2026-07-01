# drifty — metrics

Monte-Carlo harnesses that characterize each decoder and detector against the channel
impairments it is meant to survive — bit **flips**, **insertions**, **deletions**,
**erasures**, and (for the detectors) **invalid** symbols. Each harness sweeps one
standard code family and one impairment axis at a time, streams results to a CSV, and a
companion Python script renders the CSV into plots. They are **not** part of the
library or its tests — they are a research/benchmarking aid, built only when you ask
for them.

Every subdirectory here measures one implementation and carries its own analysis in a
`METRICS.md` (with the committed sweep and rendered plots). This page is the index and
the shared how-to.

## The harnesses

| Directory | Implementation | Channel axes | Metrics | Datasets |
|-----------|----------------|--------------|---------|----------|
| **[viterbi](viterbi/METRICS.md)** | Viterbi hard-decision (no channel model) | flip, erase | edit | one (`metrics.csv`) |
| **[bcjr](bcjr/METRICS.md)** | max-log-MAP hard-decision, blind-acquiring | flip, erase | edit | one (`metrics.csv`) |
| **[vindel](vindel/METRICS.md)** | `drift_viterbi` — hard-decision, drift-tracking | flip, insert, delete, erase | edit, lock | tuned / untuned / overmatched |
| **[hybrid](hybrid/METRICS.md)** | drift-tracking **soft** decoder | flip, insert, delete, erase | edit, lock | tuned / untuned / overmatched |
| **[maxir](maxir/METRICS.md)** | drift-tolerant **soft** max-log-MAP + re-acquisition | flip, insert, delete, erase | edit, lock | tuned / untuned / overmatched |
| **[detect_clean](detect_clean/METRICS.md)** | blind detector — exact GF(2) rank deficiency | flip, insert, delete, erase, invalid | present / absent consistency | tuned / untuned |
| **[detect_noisy](detect_noisy/METRICS.md)** | blind detector — FWHT parity-check bias | flip, insert, delete, erase, invalid | present / absent consistency | tuned / untuned |

Two families, two kinds of metric:

- **FEC decoders** (`viterbi`, `bcjr`, `vindel`, `hybrid`, `maxir`) recover bits, so
  their metric is the **normalized edit (Levenshtein) distance** between decoded and
  sent bits (mistakes per info bit — the right measure for a channel that inserts and
  drops, where one sync slip costs one edit). The soft/drift decoders also report a
  **lock** probability (the decoder's own running confidence). The plotter additionally
  renders **run length** = `1 / edit` (bits between mistakes). The no-drift pair
  (`viterbi`, `bcjr`) sweep only flip and erase; the drift-tolerant trio add insert and
  delete.
- **Detectors** (`detect_clean`, `detect_noisy`) do not recover bits — they answer "is
  a code present?", so their metrics are the two detection consistency reads
  (**present** `c_erasure`, **absent** `c_absent`), each run on a coded stream and a
  pure-random baseline.

## Layout of a metrics directory

```
<codec>/
  dt_<codec>_metrics.c   the harness (C, OpenMP-parallel)
  CMakeLists.txt         its build target (built under DRIFTY_BUILD_BENCH)
  rate_grids.txt         the swept rates, per variation/metric/axis - edit and re-run,
                         no recompile (each line: <variation> <metric> <axis> <rate>...)
  plot_metrics.py        renders a CSV into per-axis PNGs (needs matplotlib)
  METRICS.md             the analysis, with the committed sweep and its plots
  metrics.csv + plots/   the results (directly, or under tuned/ untuned/ overmatched/)
```

## Building and running

The harnesses are off by default; enable the bench build, then build one target:

```sh
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_maxir_metrics   # or dt_metrics (hybrid), dt_vindel_metrics, ...
```

Each program takes `<trials> <info_bits> <seed> [variation] [rate_grids_file]` (the
no-variation `viterbi`/`bcjr` drop the `variation` argument) and streams a CSV to
stdout; the plotter turns a CSV into PNGs. See each directory's `METRICS.md` for its
exact commands, and the notes there on reproducibility.

- **Reproducible.** A given `seed` fixes every point's value regardless of thread
  count — each sweep point owns its own seeded PRNG stream. The corrupted-channel
  variations see the *same* channel realizations at a given (code, axis, rate).
- **Parallel.** The independent points fan out across cores with OpenMP when available;
  without it the harness still builds and runs single-threaded. Rows stream out as each
  point finishes, so CSV order follows completion — sort if you want a stable order (the
  plotter is order-independent).
- **Retunable.** The rate grids are read from `rate_grids.txt` at startup, not compiled
  in, so you can densify a knee or extend a tail by editing that file and re-running.

## Variations (the tuned / untuned / overmatched split)

The drift decoders and detectors take a channel model, so the same sweep is run under
three relationships between what the decoder *believes* and what the channel *does*:

- **tuned** (`matched`) — the model matches the channel at every rate (the decoder's
  best case).
- **untuned** (`pegged`) — the model is fixed low (e.g. 1%) regardless of the swept
  rate, so the decoder meets an increasingly unanticipated channel.
- **overmatched** — the matched model over a **clean** channel: the x-axis is the
  corruption the decoder *expects*, and the curve is the penalty of over-bracing for
  trouble that never comes. (FEC decoders only; the detectors ship tuned / untuned.)

`viterbi` and `bcjr` take no tunable model over these axes, so they have a single
dataset.

## See also

- [README → Metrics](../README.md#metrics) — the headline findings (code and decoder
  picks for indel recovery).
- [Convolutional coding (`doc/cc/`)](../doc/cc/README.md) — the decoders these harnesses
  measure and a guide to choosing one.
