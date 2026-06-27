# drifty metrics

`metrics/hybrid/dt_metrics.c` measures the decoding-mistake rate as a function of the
channel's flip / insert / delete / erase rates, for all four standard codes. It
runs a Monte-Carlo sweep — random message → encode → channel → stream-decode —
and reports the **normalized edit (Levenshtein) distance** between the decoded
bits and the original message, divided by the number of message bits (after a
warm-up to skip the acquisition transient). Each axis is swept independently with
the other rates at zero, so each curve isolates one impairment.

> Edit distance is the right metric for a channel that inserts and deletes bits:
> a single uncorrected sync slip costs *one* edit, rather than misaligning the
> whole remaining stream the way a position-by-position bit comparison would. A
> clean decode scores 0; total failure approaches ~1 edit per info bit.

The harness runs in three variations (see [Variations](#variations) below), all
measuring decoding performance — `matched`, where the decoder anticipates the
channel, `pegged`, where it does not, and `overmatched`, where the decoder braces
for corruption that never comes (a clean channel). They write to `metrics/hybrid/tuned/`,
`metrics/hybrid/untuned/`, and `metrics/hybrid/overmatched/`.

```sh
# Build the harness (off by default) and run each variation to its own CSV.
cmake -S . -B build -DDRIFTY_BUILD_BENCH=ON
cmake --build build --target dt_metrics
# dt_metrics <trials> <info_bits> <seed> <variation> <rate_grids_file>
#   variation = pegged|matched|overmatched   (defaults: 50 1000 0xC0FFEE pegged)
#   rate_grids_file defaults to metrics/hybrid/rate_grids.txt (so run from the repo root)
build/metrics/hybrid/dt_metrics 30 4000 0xC0FFEE matched     > metrics/hybrid/tuned/metrics.csv
build/metrics/hybrid/dt_metrics 30 4000 0xC0FFEE pegged      > metrics/hybrid/untuned/metrics.csv
build/metrics/hybrid/dt_metrics 30 4000 0xC0FFEE overmatched > metrics/hybrid/overmatched/metrics.csv

# Plot the metrics each CSV carries (one curve per code). Needs matplotlib:
python3 -m venv .venv && .venv/bin/pip install matplotlib
# --match shares each edit/runlen plot's y-axis across the pair (the larger
# range wins) so the side-by-side untuned/tuned plots compare by eye directly.
.venv/bin/python metrics/hybrid/plot_metrics.py metrics/hybrid/tuned/metrics.csv   -o metrics/hybrid/tuned/plots/   --match metrics/hybrid/untuned/metrics.csv
.venv/bin/python metrics/hybrid/plot_metrics.py metrics/hybrid/untuned/metrics.csv -o metrics/hybrid/untuned/plots/ --match metrics/hybrid/tuned/metrics.csv
.venv/bin/python metrics/hybrid/plot_metrics.py metrics/hybrid/overmatched/metrics.csv -o metrics/hybrid/overmatched/plots/
```

All share a `seed`, so every run is reproducible. The corrupted-channel
variations (`pegged`, `matched`) also see the *same* channel
realizations at a given (code, axis, rate), since the seed drives the impairments
identically; `overmatched`'s channel is clean, so there its seed only drives the
message. The decoding variations sample different rate grids — each tuned to its
own differently-shaped curves (and `overmatched`'s swept rate is the decoder's
expectation, not a channel rate) — so they line up only where their grids share a
rate.

The sweep's rate grids are not compiled in: `dt_metrics` reads them at startup
from a plain-text file (`metrics/hybrid/rate_grids.txt` by default, or a path passed as
the 5th argument). Each line is `<variation> <metric> <axis>  <rate> <rate> ...`
(`#` begins a comment), so you can retune which rates a curve samples — densify a
knee, extend a tail, add points — by editing that file and re-running, with no
rebuild. The shipped file reproduces the grids these plots were made with.

The default sweep takes a few minutes (the drift-tracking axes dominate); pass
smaller `trials`/`info_bits` for a faster, coarser run. The sweep is fanned out
across cores with OpenMP when available, and each point owns a seeded PRNG
stream, so a given `seed` reproduces every row's values exactly regardless of
thread count. Rows are streamed to stdout as each point finishes (so the CSV
grows as it runs), which means their order follows completion, not the code /
axis / rate grid — sort the file if you want a stable order; the plotter is
order-independent.

By default the plotter writes, per axis, the three metrics below. The first two
are per info bit —
`tuned/plots/{edit,runlen}_vs_{flip,insert,delete,erase}_per_info_bit.png` —
and the probability is unitless, one plot per axis —
`tuned/plots/lock_vs_{flip,insert,delete,erase}.png`:

- **edit** — edit distance per bit (mistakes per bit).
- **runlen** — mean run length between edits (`1 / edit_rate`): the average bits
  that get through between mistakes, i.e. how long a transmission you can expect
  to push through cleanly. Points where no edits were observed are dropped (the
  value there is only a lower bound, not infinity).
- **lock** — mean lock probability (0–1): the decoder's own running confidence
  that it is tracking a valid coded stream, averaged over the kept bits.

Plots are per info bit (the payload delivered, which is what matters). Run-length
plots are linear with an adaptive y-cap (the low-rate spikes run off the top);
pass `--logy` for a log axis or `--ymax N` to set the cap. Pass `--metric
edit|runlength|lock` to emit just one, or `--unit coded` for the per-coded-bit
view (each edit/runlen curve ÷ its rate `n`, for comparing codes of different
rates on the wire). Pass `--match OTHER.csv` to size each edit/runlen plot's
y-axis to whichever of the two CSVs has the larger range, so the side-by-side
untuned/tuned plots below share a vertical scale and compare by eye (the lock
plots already do, fixed to `[0, 1]`).

## Variations

The decoding metrics — edit distance, run length, lock — come in three variations
that differ in the decoder's channel model: what the decoder believes about the
impairment it is facing. The codes and seed are the same; the `dt_cc_hybrid_stream_params`
probabilities the decoder is built with change (and, for `overmatched`, the
channel goes clean), and because that reshapes the curves, each variation samples
its **own rate grid** tuned to where its curves actually move.

- **untuned** (`metrics/hybrid/untuned/`, the `pegged` model) — every impairment
  probability is pegged at a flat 1%, regardless of the rate actually being
  swept, with drift tracking always on. The decoder is never told what the
  channel is doing, so as a rate climbs it meets something increasingly
  unexpected. This shows how the decoder handles **channels it cannot
  anticipate**.
- **tuned** (`metrics/hybrid/tuned/`, the `matched` model) — the active impairment's
  probability is set to the swept rate, and drift is tracked only when that
  impairment is an insertion or deletion. The decoder's model matches the
  channel at every point. This shows how the decoder handles **channels it can
  anticipate** — its best case.
- **overmatched** (`metrics/hybrid/overmatched/`, the `overmatched` model) — the decoder
  is built with the *matched* model (it expects the swept rate of corruption) but
  the channel is **clean**. So the x-axis here is what the decoder *expects*, not
  what it meets, and the curve is the **penalty of over-bracing** for trouble that
  never comes. On clean data the decoder copes until its expectation gets extreme,
  so these grids run almost to 1 (see [Overmatched](#overmatched)).

For a corrupted channel the matched decoder always does at least as well as the
pegged one; the gap is widest where a pegged probability is most wrong (high
flip/indel rates, where the matched edit knees fall much later and matched lock
barely dips) and narrowest for erasures, which carry no wrong information for
either model to misjudge. Overmatched is the opposite stress — the same matched
model, but nothing wrong to correct.

## Generated plots

The figures come from a `dt_metrics` sweep (the published set uses 30 trials ×
4000 info bits, default seed). In every plot the x-axis is the channel impairment
rate per coded bit and the four curves are the standard codes — `K3_R1_2`,
`K7_R1_2` (rate 1/2), `K7_R1_3` (rate 1/3) and `K5_R1_5` (rate 1/5), in order of
increasing redundancy.

The plots are grouped **by metric**, and each metric shows all four channel
impairments — flip, insert, delete, erase — side by side, so you read one metric
across the channels rather than one channel across the metrics. The swept rate
range is tuned per metric and per impairment to the band where that curve
actually moves, so the x-axes differ between plots: for the untuned set, edit and
run length run to ~20% (to ~90% on erase, which is far more correctable) and lock
runs the full range to 100%. The tuned set uses its own grids — edit knees fall
later, so it runs higher (~32%), and lock barely dips.

Each decoding metric below is shown as a table with the **untuned** (pegged-model)
figure on the left and its **tuned** (matched-model) counterpart on the right —
the same metric with the decoder unable to anticipate the channel (left) versus
anticipating it (right). The captions describe the untuned (left) case, the
harder one; see [Variations](#variations) for how the tuned column
differs.

### Edit distance (decoding mistakes per bit)

Normalized edit distance between the decoded and sent bits — the core
correctness metric. Every code holds near zero up to a per-code knee, then
climbs; tolerance scales with redundancy, so `K5_R1_5` holds out the longest and
the rate-1/2 codes turn up first. On the flip axis the knee (first rate past 1%
edit distance) sits near 1% for `K3_R1_2`, ~1.3% for `K7_R1_2`, ~2% for `K7_R1_3`
and ~3.5% for `K5_R1_5`. Because the decoder's model is pegged rather than matched
to the channel, the codes degrade harder past their knees than a channel-aware
decoder would: by 25% flips every code is up around 0.29–0.32 edits per info bit.
Erasures, carrying no wrong information, are tolerated to far higher rates — knees
near each code's `1 - rate` capacity limit (~20% for `K3_R1_2`, rising to ~48% for
`K5_R1_5`).

| Untuned — unanticipated channel | Tuned — anticipated channel |
|---|---|
| <img src="untuned/plots/edit_vs_flip_per_info_bit.png" alt="edit/info vs flip, untuned" width="420"> | <img src="tuned/plots/edit_vs_flip_per_info_bit.png" alt="edit/info vs flip, tuned" width="420"> |
| <img src="untuned/plots/edit_vs_insert_per_info_bit.png" alt="edit/info vs insert, untuned" width="420"> | <img src="tuned/plots/edit_vs_insert_per_info_bit.png" alt="edit/info vs insert, tuned" width="420"> |
| <img src="untuned/plots/edit_vs_delete_per_info_bit.png" alt="edit/info vs delete, untuned" width="420"> | <img src="tuned/plots/edit_vs_delete_per_info_bit.png" alt="edit/info vs delete, tuned" width="420"> |
| <img src="untuned/plots/edit_vs_erase_per_info_bit.png" alt="edit/info vs erase, untuned" width="420"> | <img src="tuned/plots/edit_vs_erase_per_info_bit.png" alt="edit/info vs erase, tuned" width="420"> |

### Run length between edits

The reciprocal of the edit rate (`1 / edit_rate`): the average number of bits
that get through between mistakes — how long a transmission you can expect to
push through cleanly. It is effectively unbounded below each code's knee (those
points, where no edits were seen, are dropped — the value there is only a lower
bound) and drops off a cliff at the knee, with the cliffs marching rightward with
redundancy. Plotted linearly with an adaptive y-cap so the low-rate spikes don't
flatten the readable range. Same knee positions as edit distance, since it is
derived from it.

| Untuned — unanticipated channel | Tuned — anticipated channel |
|---|---|
| <img src="untuned/plots/runlen_vs_flip_per_info_bit.png" alt="runlen/info vs flip, untuned" width="420"> | <img src="tuned/plots/runlen_vs_flip_per_info_bit.png" alt="runlen/info vs flip, tuned" width="420"> |
| <img src="untuned/plots/runlen_vs_insert_per_info_bit.png" alt="runlen/info vs insert, untuned" width="420"> | <img src="tuned/plots/runlen_vs_insert_per_info_bit.png" alt="runlen/info vs insert, tuned" width="420"> |
| <img src="untuned/plots/runlen_vs_delete_per_info_bit.png" alt="runlen/info vs delete, untuned" width="420"> | <img src="tuned/plots/runlen_vs_delete_per_info_bit.png" alt="runlen/info vs delete, tuned" width="420"> |
| <img src="untuned/plots/runlen_vs_erase_per_info_bit.png" alt="runlen/info vs erase, untuned" width="420"> | <img src="tuned/plots/runlen_vs_erase_per_info_bit.png" alt="runlen/info vs erase, tuned" width="420"> |

### Lock probability

The decoder's own running confidence that it is tracking a valid coded stream,
averaged over the kept bits. Because the model is pegged at 1% rather than
matched to the channel, lock genuinely tracks the decoder being surprised: it
falls from ~1.0 along a descent and settles onto a plateau as each impairment
ramps, reflecting real loss of confidence rather than false comfort. The plateau
sits by redundancy — on the flip/insert/delete axes around 0.69 for `K7_R1_2`,
0.61 for `K3_R1_2`, 0.52 for `K7_R1_3` and 0.33 for `K5_R1_5` (the strongest code
spreads its few information bits thinnest, so a surprise dents its confidence
most). The descent is where the structure lives (the plots sample it densely);
past it the plateau is flat. Two endpoints are special: under deletion the stream
empties out near 100% and lock collapses to zero, and under flips a rate near
100% is a *deterministic* inversion — structured again — so lock partly recovers.
Erasures are a *known* unknown, so lock holds higher and longer before sliding to
zero as the rate nears capacity. Lock is the decoder's confidence, not a
correctness measure.

| Untuned — unanticipated channel | Tuned — anticipated channel |
|---|---|
| <img src="untuned/plots/lock_vs_flip.png" alt="lock vs flip, untuned" width="420"> | <img src="tuned/plots/lock_vs_flip.png" alt="lock vs flip, tuned" width="420"> |
| <img src="untuned/plots/lock_vs_insert.png" alt="lock vs insert, untuned" width="420"> | <img src="tuned/plots/lock_vs_insert.png" alt="lock vs insert, tuned" width="420"> |
| <img src="untuned/plots/lock_vs_delete.png" alt="lock vs delete, untuned" width="420"> | <img src="tuned/plots/lock_vs_delete.png" alt="lock vs delete, tuned" width="420"> |
| <img src="untuned/plots/lock_vs_erase.png" alt="lock vs erase, untuned" width="420"> | <img src="tuned/plots/lock_vs_erase.png" alt="lock vs erase, tuned" width="420"> |

### Overmatched

The mirror image of the two variations above: the decoder is built with the
matched model (it *expects* the swept rate of corruption) but the channel is
**clean**, so the x-axis is what the decoder anticipates, not what it meets, and
the curve is the cost of over-bracing. A correct decoder should ignore corruption
that never arrives — and it does, right up to a hard threshold. Below an expected
rate of **0.5** every metric is perfect: edit distance is 0 and lock is 1. At
exactly 0.5 the model is singular (a flipped bit and a kept bit cost the same, an
inserted/deleted bit and a real one cost the same), so the decoder is momentarily
blind — a one-point spike in edit and dip in lock. **Above 0.5 the model
inverts**: believing corruption is more likely than not, the decoder starts
"correcting" a clean stream into garbage, settling onto a confidently-wrong
plateau (~0.30 edit/info for the rate-1/2 codes, less for the more redundant
ones). Insertions and erasures have no such threshold — expecting absent
insertions or erasures costs nothing on a clean stream, so those edit curves stay
flat at 0 (and insert/delete lock stay flat at 1); only erasure *lock* moves,
collapsing to 0 across ~0.55–0.70 as the decoder expects more erasures than real
bits. The x-axes run to ~0.97 because nothing happens until the threshold.

| Edit distance per info bit | Lock probability |
|---|---|
| <img src="overmatched/plots/edit_vs_flip_per_info_bit.png" alt="overmatched edit vs expected flip rate" width="420"> | <img src="overmatched/plots/lock_vs_flip.png" alt="overmatched lock vs expected flip rate" width="420"> |
| <img src="overmatched/plots/edit_vs_delete_per_info_bit.png" alt="overmatched edit vs expected delete rate" width="420"> | <img src="overmatched/plots/lock_vs_erase.png" alt="overmatched lock vs expected erase rate" width="420"> |

(Left: the flip and delete edit thresholds at 0.5. Right: the flip-lock dip at the
0.5 singularity with the high-redundancy `K5_R1_5` tail, and the erasure-lock
collapse. The insert/erase edit and insert/delete lock plots are flat and omitted
here; the full set is in `metrics/hybrid/overmatched/plots/`.)
