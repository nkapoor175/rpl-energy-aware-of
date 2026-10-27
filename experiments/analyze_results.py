#!/usr/bin/env python3
"""Aggregate the per-run metrics into comparison tables.

Usage:
    analyze_results.py RESULTS_DIR [--ref mrhof] [--ref of0] [--out analysis]

RESULTS_DIR is a folder such as results/main that contains run folders (each already
parsed by parse_run.py, i.e. each has run_metrics.csv). Written next to it:

    <out>.md                     readable report (also printed)
    <out>_by_variant.csv         per (nodes, variant): n, mean, std, min, max of every metric
    <out>_paired_vs_<ref>.csv    per (nodes, variant) vs the reference variant, PAIRED BY SEED

Why paired: for a given (nodes, seed) every variant runs on the identical topology and
traffic, so the right comparison is the per-seed difference, not two independent means.
With 5 seeds the confidence interval is wide: the report states the mean difference, its
95% CI (Student t) and in how many seeds the variant was better / worse, and it does NOT
declare significance. A difference whose CI contains 0 is not evidence of an effect.

Metrics (higher is better unless noted): FND_s, HND_s, LND_s (lifetimes), pdr_request,
pdr_roundtrip, latency_mean_ms (lower is better), latency_p95_ms (lower),
consumed_max_uj (lower: worst-case node at FND), consumed_std_uj (lower: more even drain),
parent_switches (lower; only when RPL logging was enabled).
"""
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

METRICS = [  # (column, higher_is_better)
    ("FND_s", True), ("HND_s", True), ("LND_s", True),
    ("pdr_request", True), ("pdr_roundtrip", True),
    ("latency_mean_ms", False), ("latency_p95_ms", False),
    ("consumed_max_uj", False), ("consumed_std_uj", False),
    ("parent_switches", False),
]
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
       8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145}
ORDER = ["of0", "mrhof", "base", "fixedw", "gating", "depletion", "adaptive", "full", "energy"]


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load(root):
    rows = []
    for f in sorted(Path(root).rglob("run_metrics.csv")):
        with f.open() as fh:
            for r in csv.DictReader(fh):
                r["_dir"] = str(f.parent)
                rows.append(r)
    if not rows:
        sys.exit("no run_metrics.csv under %s (run parse_run.py --all first)" % root)
    return rows


def fmt(x, digits=1):
    if x is None:
        return "NA"
    if abs(x) >= 1000:
        return "%.0f" % x
    return ("%." + str(digits) + "f") % x


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    root = argv[1]
    refs = [argv[i + 1] for i, a in enumerate(argv) if a == "--ref" and i + 1 < len(argv)] or ["mrhof", "of0"]
    outbase = Path(root) / (argv[argv.index("--out") + 1] if "--out" in argv else "analysis")
    rows = load(root)

    by = defaultdict(lambda: defaultdict(dict))      # nodes -> variant -> seed -> row
    for r in rows:
        by[int(r["nodes"])][r["of_label"]][int(r["seed"])] = r

    out = []
    out.append("# Results: %s\n" % Path(root).name)
    out.append("%d runs. Cutoff runs (nobody or not everyone died) show NA for the lifetimes they did not reach.\n" % len(rows))

    # ---- per variant summary
    summary_rows = []
    for n in sorted(by):
        out.append("\n## %d nodes\n" % n)
        out.append("| variant | seeds | FND s | HND s | LND s | PDR req | PDR rt | latency ms | worst node at FND (J) | drain std (J) |")
        out.append("|---|---|---|---|---|---|---|---|---|---|")
        for v in sorted(by[n], key=lambda x: ORDER.index(x) if x in ORDER else 99):
            seeds = by[n][v]
            cells = {}
            for col, _ in METRICS:
                vals = [num(r[col]) for r in seeds.values() if num(r[col]) is not None]
                if vals:
                    cells[col] = (statistics.mean(vals),
                                  statistics.stdev(vals) if len(vals) > 1 else 0.0,
                                  min(vals), max(vals), len(vals))
                summary_rows.append({"nodes": n, "variant": v, "metric": col,
                                     "n": len(vals),
                                     "mean": cells[col][0] if col in cells else "NA",
                                     "std": cells[col][1] if col in cells else "NA",
                                     "min": cells[col][2] if col in cells else "NA",
                                     "max": cells[col][3] if col in cells else "NA"})

            def cell(col, scale=1.0, digits=0):
                if col not in cells:
                    return "NA"
                m, sd = cells[col][0] / scale, cells[col][1] / scale
                return "%s ± %s" % (fmt(m, digits), fmt(sd, digits))
            out.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                v, len(seeds), cell("FND_s"), cell("HND_s"), cell("LND_s"),
                cell("pdr_request", 0.01, 2) + " %" if "pdr_request" in cells else "NA",
                cell("pdr_roundtrip", 0.01, 2) + " %" if "pdr_roundtrip" in cells else "NA",
                cell("latency_mean_ms", 1, 1), cell("consumed_max_uj", 1e6, 2), cell("consumed_std_uj", 1e6, 3)))
    with open(str(outbase) + "_by_variant.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["nodes", "variant", "metric", "n", "mean", "std", "min", "max"])
        w.writeheader()
        w.writerows(summary_rows)

    # ---- paired comparisons
    for ref in refs:
        paired = []
        out.append("\n## Paired differences vs `%s` (same topology and traffic, per seed)\n" % ref)
        out.append("Positive = better for the variant. CI is 95% Student t over the seeds; with few seeds it is wide. "
                   "'better/worse' counts seeds. No significance is claimed.\n")
        out.append("| nodes | variant | metric | seeds | mean difference | 95% CI | better / worse / tie |")
        out.append("|---|---|---|---|---|---|---|")
        for n in sorted(by):
            if ref not in by[n]:
                continue
            for v in sorted(by[n], key=lambda x: ORDER.index(x) if x in ORDER else 99):
                if v == ref:
                    continue
                for col, hib in METRICS:
                    diffs = []
                    for seed, r in by[n][v].items():
                        b = by[n][ref].get(seed)
                        x, y = num(r[col]), num(b[col]) if b else None
                        if x is not None and y is not None:
                            d = (x - y) if hib else (y - x)
                            diffs.append(d)
                    if len(diffs) < 2:
                        continue
                    m = statistics.mean(diffs)
                    sd = statistics.stdev(diffs)
                    half = T95.get(len(diffs) - 1, 1.96) * sd / math.sqrt(len(diffs))
                    better = sum(1 for d in diffs if d > 0)
                    worse = sum(1 for d in diffs if d < 0)
                    paired.append({"nodes": n, "variant": v, "ref": ref, "metric": col, "seeds": len(diffs),
                                   "mean_diff": m, "ci95_low": m - half, "ci95_high": m + half,
                                   "better": better, "worse": worse, "tie": len(diffs) - better - worse})
                    if col in ("FND_s", "HND_s", "LND_s", "pdr_roundtrip", "latency_mean_ms", "consumed_max_uj"):
                        out.append("| %d | %s | %s | %d | %s | [%s, %s] | %d / %d / %d |" % (
                            n, v, col, len(diffs), fmt(m, 3 if "pdr" in col else 1),
                            fmt(m - half, 3 if "pdr" in col else 1), fmt(m + half, 3 if "pdr" in col else 1),
                            better, worse, len(diffs) - better - worse))
        with open(str(outbase) + "_paired_vs_%s.csv" % ref, "w", newline="") as fh:
            cols = ["nodes", "variant", "ref", "metric", "seeds", "mean_diff", "ci95_low", "ci95_high",
                    "better", "worse", "tie"]
            w = csv.DictWriter(fh, fieldnames=cols)
            w.writeheader()
            w.writerows(paired)

    text = "\n".join(out) + "\n"
    Path(str(outbase) + ".md").write_text(text, encoding="utf-8")
    print(text)
    print("written:", str(outbase) + ".md", "and the CSVs next to it")


if __name__ == "__main__":
    main(sys.argv)
