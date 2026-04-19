#!/usr/bin/env python3
"""
profile_analysis.py -- phase-level cycle attribution across configs.

Reads CSVs produced by the STACKSORT_PROFILE build (fp_cycles_* columns)
and produces a per-phase breakdown, delta-vs-baseline table, and share
percentages per config. Used to:

  1. Decide where optimization effort pays off.
  2. Measure the cost of new heuristics in isolation from everything else.
  3. Catch silent perf regressions introduced alongside quality wins.

Assumes CSVs came from a single-threaded profile run (STACKSORT_PROFILE=1
build, taskset/affinity pinned for low noise). Multi-worker tune runs
will have contaminated rdtsc values -- don't feed those here.

Usage:
  tests/harness/build.bat       # needs STACKSORT_PROFILE=1 in env
  python tests/run_matrix.py --seeds 20 --tag profile_baseline ...
  python tests/profile_analysis.py "tests/results/profile_baseline/*.csv" \
      --baseline baseline --out tests/results/profile_baseline/report.md
"""
import argparse
import os
import sys

try:
    import pandas as pd
except ImportError:
    print("ERROR: pandas not installed.", file=sys.stderr)
    sys.exit(1)

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass

from loaders import load_csvs


# All profile columns in emit order. Subset is detected per-CSV since a
# non-profile build has none of them.
INNER_LOOP_PHASES = [
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
    ("fp_cycles_stranded",           "Stranded (post-fusion=0)"),
    ("fp_cycles_score",              "Score"),
    ("fp_cycles_accept",             "Accept"),
]

PER_RUN_PHASES = [
    ("fp_cycles_pre_reservation",       "PreReservation"),
    ("fp_cycles_greedy_seed",           "GreedySeed (BSSF+BAF)"),
    ("fp_cycles_unconstrained_fallback", "UnconstrainedFallback"),
    ("fp_cycles_optimize_grouping",     "OptimizeGrouping"),
    ("fp_cycles_borders_raw",           "BordersRaw (final)"),
]

ALL_PHASES = INNER_LOOP_PHASES + PER_RUN_PHASES


def _fmt_cycles(c):
    if pd.isna(c):
        return "-"
    if c >= 1e9:
        return "{:.2f}G".format(c / 1e9)
    if c >= 1e6:
        return "{:.1f}M".format(c / 1e6)
    if c >= 1e3:
        return "{:.1f}K".format(c / 1e3)
    return "{:.0f}".format(c)


def _fmt_delta(d):
    if pd.isna(d):
        return "-"
    sign = "+" if d >= 0 else ""
    if abs(d) >= 1e9:
        return "{}{:.2f}G".format(sign, d / 1e9)
    if abs(d) >= 1e6:
        return "{}{:.1f}M".format(sign, d / 1e6)
    if abs(d) >= 1e3:
        return "{}{:.1f}K".format(sign, d / 1e3)
    return "{}{:.0f}".format(sign, d)


def emit_header(out, df, baseline, available):
    out.append("# StackSort profile report")
    out.append("")
    out.append("Baseline: `{}`".format(baseline))
    out.append("Total rows: {}".format(len(df)))
    out.append("Configs: " + ", ".join(sorted(df["config_name"].unique())))
    out.append("Instances: {}".format(df["instance_name"].nunique()))
    missing = [col for col, _ in ALL_PHASES if col not in available]
    if missing:
        out.append("")
        out.append("**WARN:** missing phase columns (non-profile build CSVs?): "
                   + ", ".join(missing))
    out.append("")
    out.append("## How to read this report")
    out.append("")
    out.append("Cycles are CPU timestamps (rdtsc) accumulated across the LAHC "
               "inner loop (MoveGen..Accept) or once per PackAnnealed call "
               "(per-run phases). Values are **medians across all rows** — "
               "per-instance median would undercount long-running phases on "
               "harder instances. Use single-threaded runs only (run_matrix "
               "--workers 1) because TSC is contaminated by worker contention.")
    out.append("")
    out.append("- `Grouping` wraps ComputeGroupingBonus — Pass 1 (union-find on "
               "same-type SharedBorder edges) + Pass 2 (border accumulation).")
    out.append("- `Stranded` is held at 0 post-fusion — concentration and "
               "stranded share the same flood-fill pass now.")
    out.append("")


def emit_absolute_medians(out, df, available):
    out.append("## Absolute phase medians (cycles)")
    out.append("")
    configs = sorted(df["config_name"].unique())
    header = "| Phase | " + " | ".join(configs) + " |"
    out.append(header)
    out.append("|---" + "|---:" * len(configs) + "|")
    for col, label in ALL_PHASES:
        if col not in available:
            continue
        medians = {}
        for cfg in configs:
            sub = df[df["config_name"] == cfg]
            medians[cfg] = sub[col].median() if len(sub) else float("nan")
        if label.startswith("  ") and all(m == 0 for m in medians.values()):
            continue
        row = "| {} |".format(label)
        for cfg in configs:
            row += " {} |".format(_fmt_cycles(medians[cfg]))
        out.append(row)
    out.append("")


def emit_delta_vs_baseline(out, df, baseline, available):
    out.append("## Delta vs baseline (median cycles)")
    out.append("")
    out.append("Positive = slower than baseline. Reported as absolute cycle "
               "delta; combine with absolute medians above to gauge relative "
               "magnitude.")
    out.append("")
    configs = sorted([c for c in df["config_name"].unique() if c != baseline])
    if not configs:
        out.append("Only baseline present — no deltas to report.")
        out.append("")
        return
    base = df[df["config_name"] == baseline]
    base_med = {col: base[col].median() for col, _ in ALL_PHASES if col in available}
    header = "| Phase | " + " | ".join("Δ " + c for c in configs) + " |"
    out.append(header)
    out.append("|---" + "|---:" * len(configs) + "|")
    for col, label in ALL_PHASES:
        if col not in available:
            continue
        deltas = {}
        base_ok = base_med.get(col, 0)
        for cfg in configs:
            sub = df[df["config_name"] == cfg]
            med = sub[col].median() if len(sub) else float("nan")
            deltas[cfg] = med - base_med.get(col, float("nan"))
        if label.startswith("  ") and base_ok == 0 and all(
                (d == 0 or pd.isna(d)) for d in deltas.values()):
            continue
        row = "| {} |".format(label)
        for cfg in configs:
            row += " {} |".format(_fmt_delta(deltas[cfg]))
        out.append(row)
    out.append("")


def emit_share_percent(out, df, available):
    """Share percent = each phase / sum of (inner-loop phases) per config.
    Per-run phases are shown separately since they're one-shot and don't
    dominate when multiplied by 64k iters."""
    out.append("## Share of inner-loop cycles per config")
    out.append("")
    out.append("Share of each inner-loop phase as a fraction of the "
               "inner-loop-total (excludes once-per-run phases). Shows where "
               "time is actually going in the hot path.")
    out.append("")
    configs = sorted(df["config_name"].unique())
    # Sub-phases (indented labels) are slices of their parent — exclude from
    # the total so SkylinePack + Skyline.* don't double-count.
    inner_cols = [c for c, lbl in INNER_LOOP_PHASES
                  if c in available and not lbl.startswith("  ")]
    header = "| Phase | " + " | ".join(configs) + " |"
    out.append(header)
    out.append("|---" + "|---:" * len(configs) + "|")
    config_totals = {}
    for cfg in configs:
        sub = df[df["config_name"] == cfg]
        config_totals[cfg] = sum(sub[col].median() for col in inner_cols
                                 if col in sub.columns)
    subphase_skipped = False
    for col, label in INNER_LOOP_PHASES:
        if col not in available:
            continue
        medians = {}
        for cfg in configs:
            sub = df[df["config_name"] == cfg]
            medians[cfg] = sub[col].median() if len(sub) else float("nan")
        # Sub-phase rows only populate when STACKSORT_PROFILE_SUBPHASE=1; if
        # every config reports zero the flag was off — skip to keep noise out.
        if label.startswith("  ") and all(m == 0 for m in medians.values()):
            subphase_skipped = True
            continue
        row = "| {} |".format(label)
        for cfg in configs:
            med = medians[cfg]
            total = config_totals.get(cfg, 0)
            if total > 0 and not pd.isna(med):
                pct = 100.0 * med / total
                row += " {:.1f}% |".format(pct)
            else:
                row += " - |"
        out.append(row)
    out.append("")
    if subphase_skipped:
        out.append("_Sub-phase rows (indented) hidden — build with "
                   "`STACKSORT_PROFILE_SUBPHASE=1` for per-candidate breakdown._")
        out.append("")

    out.append("## Per-run phase medians (cycles per PackAnnealed call)")
    out.append("")
    out.append("Per-run phases fire once per top-level call, not per LAHC iter. "
               "On long runs (16 restarts × 4000 iters first-pass) their "
               "relative share is tiny, but they matter for cold-click latency.")
    out.append("")
    out.append("| Phase | " + " | ".join(configs) + " |")
    out.append("|---" + "|---:" * len(configs) + "|")
    for col, label in PER_RUN_PHASES:
        if col not in available:
            continue
        row = "| {} |".format(label)
        for cfg in configs:
            sub = df[df["config_name"] == cfg]
            med = sub[col].median() if len(sub) else float("nan")
            row += " {} |".format(_fmt_cycles(med))
        out.append(row)
    out.append("")


def main():
    ap = argparse.ArgumentParser(
        description="Phase-level cycle attribution across configs.")
    ap.add_argument("csv", nargs="+", help="CSV glob patterns (profile build)")
    ap.add_argument("--baseline", default="baseline",
                    help="Baseline config_name (default 'baseline')")
    ap.add_argument("--out", default=None, help="Output markdown path (stdout if omitted)")
    args = ap.parse_args()

    df, _ = load_csvs(args.csv)
    if df is None or df.empty:
        print("ERROR: no data loaded", file=sys.stderr)
        return 1

    available = set(df.columns)
    if not any(col in available for col, _ in ALL_PHASES):
        print("ERROR: no fp_cycles_* columns in CSVs — was the harness built "
              "with STACKSORT_PROFILE=1?", file=sys.stderr)
        return 1

    out = []
    emit_header(out, df, args.baseline, available)
    emit_absolute_medians(out, df, available)
    emit_delta_vs_baseline(out, df, args.baseline, available)
    emit_share_percent(out, df, available)

    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print("Wrote {}".format(args.out))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
