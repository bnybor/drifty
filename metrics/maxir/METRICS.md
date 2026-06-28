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
> The committed CSVs are the full sweep — all three variations (`matched` →
> `tuned/`, `pegged` → `untuned/`, `overmatched` → `overmatched/`), 30 trials per
> point over the shipped `rate_grids.txt`, seed `0xC0FFEE`. Plots are rendered into
> each variation's `plots/` (regenerate them with the plotting commands below).

```sh
# Build the harness (off by default) and run a sweep to a CSV.
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_maxir_metrics
# dt_maxir_metrics <trials> <info_bits> <seed> <variation> <rate_grids_file>
#   defaults: 50 1000 0xC0FFEE matched, grids = metrics/maxir/rate_grids.txt
#   (run from the repo root; the grid file holds one block per variation)
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE matched     > metrics/maxir/tuned/metrics.csv
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE pegged      > metrics/maxir/untuned/metrics.csv
build/metrics/maxir/dt_maxir_metrics 30 4000 0xC0FFEE overmatched > metrics/maxir/overmatched/metrics.csv

# Plot the edit and lock metrics (one curve per code). Needs matplotlib:
python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python metrics/maxir/plot_metrics.py metrics/maxir/tuned/metrics.csv       -o metrics/maxir/tuned/plots/       --match metrics/maxir/untuned/metrics.csv
.venv/bin/python metrics/maxir/plot_metrics.py metrics/maxir/untuned/metrics.csv     -o metrics/maxir/untuned/plots/     --match metrics/maxir/tuned/metrics.csv
.venv/bin/python metrics/maxir/plot_metrics.py metrics/maxir/overmatched/metrics.csv -o metrics/maxir/overmatched/plots/
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

## Generated plots

The figures come from the full sweep described above (30 trials × 4000 info bits,
seed `0xC0FFEE`). In every plot the x-axis is the channel impairment rate per coded
bit and the four curves are the standard codes — `K3_R1_2`, `K7_R1_2` (rate 1/2),
`K7_R1_3` (rate 1/3) and `K5_R1_5` (rate 1/5), in order of increasing redundancy.
The plots are grouped by metric, each showing all four channel impairments — flip,
insert, delete, erase.

Each decoding metric below is shown with the **untuned** (`pegged` model, the
decoder pegged at a flat 1% and never told what the channel is doing) on the left
and its **tuned** (`matched` model, the decoder anticipating the swept impairment)
counterpart on the right. The `--match` overlay shares each pair's y-axis so they
compare by eye; see the variation bullets above for what each model does.

### Edit distance (decoding mistakes per bit)

Normalized edit distance between decoded and sent bits — the core correctness
metric. Every code holds near zero up to a per-code knee, then climbs; tolerance
scales with redundancy, so `K5_R1_5` holds out longest (flip knee ~19% vs ~5% for
`K3_R1_2`). The drift axes — insert and delete — are the point of maxir; the
no-drift codecs cannot sweep them at all. Because the untuned model is pegged
rather than matched, the codes degrade harder past their knees than the tuned
column, where each knee falls later.

| Untuned — unanticipated channel | Tuned — anticipated channel |
|---|---|
| <img src="untuned/plots/edit_vs_flip_per_info_bit.png" alt="edit/info vs flip, untuned" width="420"> | <img src="tuned/plots/edit_vs_flip_per_info_bit.png" alt="edit/info vs flip, tuned" width="420"> |
| <img src="untuned/plots/edit_vs_insert_per_info_bit.png" alt="edit/info vs insert, untuned" width="420"> | <img src="tuned/plots/edit_vs_insert_per_info_bit.png" alt="edit/info vs insert, tuned" width="420"> |
| <img src="untuned/plots/edit_vs_delete_per_info_bit.png" alt="edit/info vs delete, untuned" width="420"> | <img src="tuned/plots/edit_vs_delete_per_info_bit.png" alt="edit/info vs delete, tuned" width="420"> |
| <img src="untuned/plots/edit_vs_erase_per_info_bit.png" alt="edit/info vs erase, untuned" width="420"> | <img src="tuned/plots/edit_vs_erase_per_info_bit.png" alt="edit/info vs erase, tuned" width="420"> |

### Run length between edits

The reciprocal of the edit rate (`1 / edit_rate`): the average bits that get
through between mistakes — how long a transmission you can expect to push through
cleanly. Effectively unbounded below each code's knee (those zero-edit points are
dropped) and drops off a cliff at the knee, the cliffs marching rightward with
redundancy. Plotted linearly with an adaptive y-cap.

| Untuned — unanticipated channel | Tuned — anticipated channel |
|---|---|
| <img src="untuned/plots/runlen_vs_flip_per_info_bit.png" alt="runlen/info vs flip, untuned" width="420"> | <img src="tuned/plots/runlen_vs_flip_per_info_bit.png" alt="runlen/info vs flip, tuned" width="420"> |
| <img src="untuned/plots/runlen_vs_insert_per_info_bit.png" alt="runlen/info vs insert, untuned" width="420"> | <img src="tuned/plots/runlen_vs_insert_per_info_bit.png" alt="runlen/info vs insert, tuned" width="420"> |
| <img src="untuned/plots/runlen_vs_delete_per_info_bit.png" alt="runlen/info vs delete, untuned" width="420"> | <img src="tuned/plots/runlen_vs_delete_per_info_bit.png" alt="runlen/info vs delete, tuned" width="420"> |
| <img src="untuned/plots/runlen_vs_erase_per_info_bit.png" alt="runlen/info vs erase, untuned" width="420"> | <img src="tuned/plots/runlen_vs_erase_per_info_bit.png" alt="runlen/info vs erase, tuned" width="420"> |

### Lock probability

The decoder's own running confidence (0–1) that it is tracking a valid coded
stream, averaged over the kept bits. It stays near 1.0 at low impairment and dips
through the mid-range where tracking is hardest; on the insert / delete axes it
then **recovers toward 1.0 at extreme rates** (e.g. matched insert-lock for
`K7_R1_2` falls to ~0.82 around 15–25% then climbs back past ~0.95), because a
near-constant impairment is itself a consistent — and so trackable — drift. Lock is
the decoder's confidence, not a correctness measure.

| Untuned — unanticipated channel | Tuned — anticipated channel |
|---|---|
| <img src="untuned/plots/lock_vs_flip.png" alt="lock vs flip, untuned" width="420"> | <img src="tuned/plots/lock_vs_flip.png" alt="lock vs flip, tuned" width="420"> |
| <img src="untuned/plots/lock_vs_insert.png" alt="lock vs insert, untuned" width="420"> | <img src="tuned/plots/lock_vs_insert.png" alt="lock vs insert, tuned" width="420"> |
| <img src="untuned/plots/lock_vs_delete.png" alt="lock vs delete, untuned" width="420"> | <img src="tuned/plots/lock_vs_delete.png" alt="lock vs delete, tuned" width="420"> |
| <img src="untuned/plots/lock_vs_erase.png" alt="lock vs erase, untuned" width="420"> | <img src="tuned/plots/lock_vs_erase.png" alt="lock vs erase, tuned" width="420"> |

### Overmatched

The decoder is built with the matched model (it *expects* the swept rate of
corruption) but the channel is **clean**, so the x-axis is what the decoder
anticipates, not what it meets, and the curve is the cost of over-bracing. Only the
**insert** axis stays flat at 0 (expecting absent insertions costs nothing on a
clean stream); flip, delete and erase all break down once the decoder over-expects.
On flip and delete the model is singular at an expected rate of **0.5** (a flipped
bit and a kept bit then cost the same) — perfect below it, inverting above it as the
decoder "corrects" a clean stream into garbage (a confidently-wrong plateau ~0.30
edit/info for the rate-1/2 codes, more for the others). Erasure breaks down as the
decoder expects more erasures than real bits. The full set is in
`metrics/maxir/overmatched/plots/`.

| Edit distance per info bit | Lock probability |
|---|---|
| <img src="overmatched/plots/edit_vs_flip_per_info_bit.png" alt="overmatched edit vs expected flip rate" width="420"> | <img src="overmatched/plots/lock_vs_flip.png" alt="overmatched lock vs expected flip rate" width="420"> |
| <img src="overmatched/plots/edit_vs_delete_per_info_bit.png" alt="overmatched edit vs expected delete rate" width="420"> | <img src="overmatched/plots/lock_vs_erase.png" alt="overmatched lock vs expected erase rate" width="420"> |
