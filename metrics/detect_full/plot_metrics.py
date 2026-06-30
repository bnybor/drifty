#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Robyn Kirkman
"""Plot blind code-presence detection metrics vs channel rate.

Reads the CSV produced by a dt_detect_{lean,full}_metrics harness and renders, for
each channel axis (flip / insert / delete / erase), two figures - one per soft
confidence the detector emits:

  present (c_erasure) - confidence a convolutional code IS present.
  absent  (c_absent)  - confidence a code is NOT present.

Each figure draws one solid curve per code (the value on a CODED stream) plus a
dashed BASELINE - what the detector reports on a pure-RANDOM stream through the same
channel, averaged over the codes (random detection is code-independent). Detection
works to the extent the coded curves stand clear of that baseline: for `present`,
coded should ride high above a near-zero random floor; for `absent`, coded should
sit low under a high random baseline. Both axes are confidences in [0, 1].

The CSV is a single variation (pegged or matched); run the plotter once per
variation CSV into that variation's plots/ directory.

Usage:
    build/metrics/detect_full/dt_detect_full_metrics 16 4000 0xC0FFEE matched \
        > metrics/detect_full/tuned/metrics.csv
    python3 metrics/detect_full/plot_metrics.py metrics/detect_full/tuned/metrics.csv \
        -o metrics/detect_full/tuned/plots/
"""
import argparse
import csv
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")  # render to files; no display needed
import matplotlib.pyplot as plt

# The two confidences, each its own figure: (csv coded column, csv random column,
# y-axis label, title fragment).
METRICS = {
    "present": ("coded_present", "random_present",
                "mean c_erasure (code-present)", "code-present confidence"),
    "absent": ("coded_absent", "random_absent",
               "mean c_absent (no-code)", "no-code confidence"),
}


def load(path):
    """series[axis][code] -> sorted [(rate, coded_present, coded_absent,
    random_present, random_absent)]; plus the variation name."""
    series = defaultdict(lambda: defaultdict(list))
    variation = None
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            variation = row.get("variation") or variation
            series[row["axis"]][row["code"]].append((
                float(row["rate"]),
                float(row["coded_present"]), float(row["coded_absent"]),
                float(row["random_present"]), float(row["random_absent"]),
            ))
    for by_code in series.values():
        for pts in by_code.values():
            pts.sort()
    return series, variation


def baseline(by_code, idx):
    """Mean over codes of column `idx` at each rate: the random baseline curve."""
    acc = defaultdict(lambda: [0.0, 0])
    for pts in by_code.values():
        for row in pts:
            acc[row[0]][0] += row[idx]
            acc[row[0]][1] += 1
    rates = sorted(acc)
    return rates, [acc[r][0] / acc[r][1] for r in rates]


# csv-column index within a series row for each metric's coded / random value.
COL = {"coded_present": 1, "coded_absent": 2,
       "random_present": 3, "random_absent": 4}


def plot_axis_metric(axis, by_code, outdir, metric, variation):
    coded_col, random_col, ylabel, title = METRICS[metric]
    fig, ax = plt.subplots(figsize=(7, 5))
    ci = COL[coded_col]
    for code, pts in sorted(by_code.items()):
        ax.plot([p[0] for p in pts], [p[ci] for p in pts], marker="o", label=code)
    rates, base = baseline(by_code, COL[random_col])
    ax.plot(rates, base, color="0.4", linestyle="--", linewidth=2,
            marker="x", markersize=4, label="random (baseline)")
    ax.set_xlabel(f"channel {axis} rate per coded bit")
    ax.set_ylabel(ylabel)
    var = f" [{variation}]" if variation else ""
    ax.set_title(f"{title} vs {axis} rate{var}")
    ax.set_ylim(-0.02, 1.02)
    # Pin the origin at 0; the grids are kept identical between detect_full and
    # detect_full, so corresponding plots end up with the same x-range too.
    ax.set_xlim(left=0)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    out = os.path.join(outdir, f"{metric}_vs_{axis}.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="CSV from a dt_detect_*_metrics run")
    ap.add_argument("-o", "--outdir", default=".", help="output directory")
    ap.add_argument("--metric", choices=["present", "absent", "all"],
                    default="all", help="c_erasure (present), c_absent (absent), "
                    "or both (default)")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    series, variation = load(args.csv)
    if not series:
        raise SystemExit("no rows found in CSV")
    metrics = ["present", "absent"] if args.metric == "all" else [args.metric]
    for axis, by_code in series.items():
        for metric in metrics:
            plot_axis_metric(axis, by_code, args.outdir, metric, variation)


if __name__ == "__main__":
    main()
