#!/usr/bin/env python3
"""
analyze_perf_delta.py -- cycle-delta analysis between a baseline CSV and an
after-run glob. Dedicated to pre/post perf comparisons where both sides have
the same config_name.

Outputs:
  - Global phase median table (before, after, delta, %change, rows-faster).
  - Per-instance LER+Concentration combined delta (the main Phase 4 signal).
  - Grid-hash probe/hit rate (Phase 4 cache health metric).

Usage:
  python tests/analyze_perf_delta.py \\
      --before tests/results/profile_baseline.csv \\
      --after "tests/results/phase4_perf/*.csv" \\
      [--out tests/results/phase4_perf/report.md]
"""
import argparse
import glob
import os
import sys

try:
    import pandas as pd
except ImportError:
    print("ERROR: pandas not installed.", file=sys.stderr)
    sys.exit(1)


JOIN_KEYS = ["config_name", "instance_name", "seed", "target", "rotate_all"]

PHASES = [
    ("fp_cycles_move_gen",           "MoveGen"),
    ("fp_cycles_skyline_pack",       "SkylinePack"),
    ("fp_cycles_skyline_waste_map",  "  Skyline.WasteMap"),
    ("fp_cycles_skyline_candidate",  "  Skyline.Candidate"),
    ("fp_cycles_skyline_adjacency",  "  Skyline.Adjacency"),
    ("fp_cycles_skyline_commit",     "  Skyline.Commit"),
    ("fp_cycles_ler",                "LER"),
    ("fp_cycles_ler_histogram",      "  LER.Histogram"),
    ("fp_cycles_ler_stack",          "  LER.Stack"),
    ("fp_cycles_concentration",      "Concentration"),
    ("fp_cycles_grouping",           "Grouping"),
    ("fp_cycles_score",              "Score"),
    ("fp_cycles_accept",             "Accept"),
]


def _fmt(c):
    if pd.isna(c):
        return "-"
    if abs(c) >= 1e9:
        return "{:.2f}G".format(c / 1e9)
    if abs(c) >= 1e6:
        return "{:.1f}M".format(c / 1e6)
    if abs(c) >= 1e3:
        return "{:.1f}K".format(c / 1e3)
    return "{:.0f}".format(c)


def _first_present(cols, candidates):
    for c in candidates:
        if c in cols:
            return c
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--before", required=True, help="Baseline CSV path or glob")
    ap.add_argument("--after",  required=True, help="Glob for post-change CSV(s)")
    ap.add_argument("--out", default=None, help="Optional markdown output path")
    args = ap.parse_args()

    before_files = glob.glob(args.before) if any(c in args.before for c in "*?[") \
                   else ([args.before] if os.path.exists(args.before) else [])
    if not before_files:
        print("ERROR: no baseline CSVs matched {}".format(args.before), file=sys.stderr)
        return 1
    after_files = glob.glob(args.after)
    if not after_files:
        print("ERROR: no after-run CSVs matched {}".format(args.after), file=sys.stderr)
        return 1

    before = pd.concat([pd.read_csv(f) for f in before_files], ignore_index=True)
    before = before[before["config_name"] == "baseline"].copy()
    after = pd.concat([pd.read_csv(f) for f in after_files], ignore_index=True)

    merged = before.merge(after, on=JOIN_KEYS, suffixes=("_b", "_a"))
    n = len(merged)
    if n == 0:
        print("ERROR: zero rows joined -- check config_name + join keys", file=sys.stderr)
        return 1

    out = []
    out.append("# Perf delta report")
    out.append("")
    out.append("Before: `{}`".format(args.before))
    out.append("After:  `{}` ({} file(s))".format(args.after, len(after_files)))
    out.append("Joined rows: {}".format(n))
    out.append("")

    # Global phase medians
    out.append("## Phase cycle medians")
    out.append("")
    out.append("| Phase | Before | After | Delta | %chg | rows-faster |")
    out.append("|---|---:|---:|---:|---:|---:|")
    total_b = 0.0
    total_a = 0.0
    subphase_skipped = False
    for col, label in PHASES:
        bc, ac = col + "_b", col + "_a"
        if bc not in merged.columns:
            continue
        b = merged[bc].median()
        a = merged[ac].median()
        # Sub-phase counters (indented labels) are populated only by builds
        # with STACKSORT_PROFILE_SUBPHASE=1. If both sides report zero the
        # measurement was off — skip the row so it doesn't clutter the table.
        if label.startswith("  ") and b == 0 and a == 0:
            subphase_skipped = True
            continue
        delta = a - b
        pct = 100.0 * delta / b if b else 0.0
        faster = int((merged[ac] < merged[bc]).sum())
        # Indented labels are sub-phases of their parent — already counted in parent.
        if not label.startswith("  "):
            total_b += b
            total_a += a
        out.append("| {} | {} | {} | {} | {:+.1f}% | {}/{} |".format(
            label, _fmt(b), _fmt(a), _fmt(delta), pct, faster, n))
    total_delta = total_a - total_b
    total_pct = 100.0 * total_delta / total_b if total_b else 0.0
    out.append("| **Inner loop total** | **{}** | **{}** | **{}** | **{:+.1f}%** | - |".format(
        _fmt(total_b), _fmt(total_a), _fmt(total_delta), total_pct))
    out.append("")
    if subphase_skipped:
        out.append("_Sub-phase rows (indented) hidden — build with "
                   "`STACKSORT_PROFILE_SUBPHASE=1` for per-candidate breakdown._")
        out.append("")

    # Per-instance LER+Conc combined (the Phase 4 signal)
    if "fp_cycles_ler_b" in merged.columns and "fp_cycles_concentration_b" in merged.columns:
        merged = merged.copy()
        merged["lerconc_b"] = merged["fp_cycles_ler_b"] + merged["fp_cycles_concentration_b"]
        merged["lerconc_a"] = merged["fp_cycles_ler_a"] + merged["fp_cycles_concentration_a"]
        density_col = _first_present(merged.columns, ["density_b", "density"])
        group_cols = {"b": ("lerconc_b", "median"), "a": ("lerconc_a", "median")}
        if density_col:
            group_cols["density"] = (density_col, "first")
        per_inst = merged.groupby("instance_name").agg(**group_cols).sort_values("a", ascending=False)
        per_inst["pct"] = 100.0 * (per_inst["a"] - per_inst["b"]) / per_inst["b"]

        out.append("## Per-instance LER+Concentration delta")
        out.append("")
        cols = ["instance", "LER+Conc before", "LER+Conc after", "%chg"]
        if density_col:
            cols.insert(1, "density")
        out.append("| " + " | ".join(cols) + " |")
        out.append("|---" + "|---:" * (len(cols) - 1) + "|")
        for name, row in per_inst.iterrows():
            parts = ["`{}`".format(name)]
            if density_col:
                parts.append("{:.2f}".format(row["density"]))
            parts.append(_fmt(row["b"]))
            parts.append(_fmt(row["a"]))
            parts.append("{:+.1f}%".format(row["pct"]))
            out.append("| " + " | ".join(parts) + " |")
        out.append("")

    # Grid-hash cache health
    probes_col = _first_present(merged.columns, ["fp_grid_hash_probes_a", "fp_grid_hash_probes"])
    hits_col = _first_present(merged.columns, ["fp_grid_hash_hits_a", "fp_grid_hash_hits"])
    if probes_col and hits_col:
        probes = float(merged[probes_col].sum())
        hits = float(merged[hits_col].sum())
        rate = 100.0 * hits / probes if probes else 0.0
        out.append("## Grid-hash cache health")
        out.append("")
        out.append("- Probes: {:,}".format(int(probes)))
        out.append("- Hits:   {:,}".format(int(hits)))
        out.append("- Hit rate: **{:.1f}%**".format(rate))
        out.append("")

    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print("Wrote {}".format(args.out))
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
