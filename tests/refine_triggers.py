#!/usr/bin/env python3
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

from loaders import load_csvs as _load_csvs


def load_csvs(patterns):
    df, _ = _load_csvs(patterns)
    return df


def bin_rate(df, col, bins, label=None):
    """Bin a column and show refine_replaced rate per bin."""
    if label is None:
        label = col
    df = df.copy()
    df["_bin"] = pd.cut(df[col], bins=bins, include_lowest=True)
    grouped = df.groupby("_bin").agg(
        n=("refine_replaced", "count"),
        replaced=("refine_replaced", "sum"),
    )
    grouped["rate"] = grouped["replaced"] / grouped["n"]
    print("\n{}:".format(label))
    print("{:<24s} {:>6s} {:>8s} {:>8s}".format("bin", "n", "replaced", "rate"))
    for idx, row in grouped.iterrows():
        if row["n"] == 0:
            continue
        print("{:<24s} {:>6.0f} {:>8.0f} {:>7.1%}".format(
            str(idx), row["n"], row["replaced"], row["rate"]))
    return grouped


def threshold_scan(df, col, thresholds, direction="above"):
    """For each threshold, show what trigger rate and replace rate would be
    if we only refined rows where col is above (or below) the threshold."""
    print("\n{} threshold scan (refine when {} threshold):".format(
        col, direction))
    print("{:>10s} {:>8s} {:>10s} {:>10s} {:>10s}".format(
        "threshold", "trigger%", "triggered", "replaced", "replace%"))
    for t in thresholds:
        if direction == "above":
            triggered = df[df[col] > t]
        else:
            triggered = df[df[col] < t]
        n_triggered = len(triggered)
        n_replaced = int(triggered["refine_replaced"].sum())
        trigger_pct = n_triggered / len(df) if len(df) > 0 else 0
        replace_pct = n_replaced / n_triggered if n_triggered > 0 else 0
        print("{:>10} {:>7.1%} {:>10} {:>10} {:>9.1%}".format(
            t, trigger_pct, n_triggered, n_replaced, replace_pct))


def correlation_summary(df, predictors):
    """Point-biserial correlation of each predictor with refine_replaced."""
    print("\nCorrelation with refine_replaced:")
    print("{:<36s} {:>8s} {:>8s}".format("predictor", "corr", "abs"))
    corrs = []
    for col in predictors:
        if col not in df.columns:
            continue
        c = df[col].corr(df["refine_replaced"])
        if pd.notna(c):
            corrs.append((col, c))
    corrs.sort(key=lambda x: -abs(x[1]))
    for col, c in corrs:
        print("{:<36s} {:>+8.4f} {:>8.4f}".format(col, c, abs(c)))


def main():
    ap = argparse.ArgumentParser(
        description="Analyze refine_always results to find refinement trigger predictors.")
    ap.add_argument("csv", nargs="+", help="CSV glob patterns (use refine_always data)")
    ap.add_argument("--config", default=None,
                    help="Filter to a specific config_name (default: all)")
    args = ap.parse_args()

    df = load_csvs(args.csv)
    if df is None or df.empty:
        print("ERROR: no data loaded", file=sys.stderr)
        return 1

    if args.config:
        df = df[df["config_name"] == args.config]

    # Only look at rows where refinement actually ran.
    if "refine_triggered" in df.columns:
        df = df[df["refine_triggered"] == 1]
    if df.empty:
        print("ERROR: no refine_triggered rows", file=sys.stderr)
        return 1

    total = len(df)
    replaced = int(df["refine_replaced"].sum())
    print("=" * 70)
    print("Refinement trigger analysis")
    print("=" * 70)
    print("Rows with refinement: {}".format(total))
    print("Replaced (improved):  {} ({:.1%})".format(replaced, replaced / total))
    print("")

    # Derive density if not present
    if "density" not in df.columns and "grid_w" in df.columns:
        df["density"] = df.apply(
            lambda r: 1.0 - (r["grid_w"] * r["grid_h"]
                             - r["num_items"]) / (r["grid_w"] * r["grid_h"])
            if r["grid_w"] * r["grid_h"] > 0 else 0, axis=1)

    # Use the actual density column from the CSV if available
    # (it's total_item_area / grid_area, more accurate than num_items / grid_area)

    # First pass quality metrics that might predict refinement value
    predictors = [
        "density",
        "num_items",
        "num_types",
        "grid_w",
        "grid_h",
        "target",
        "first_pass_score",
        "first_pass_concentration",
        "first_pass_stranded",
        "first_pass_grouping_bonus",
        "first_pass_ler_area",
        "first_pass_ler_width",
        "first_pass_ler_height",
        "first_pass_num_rotated",
        "first_pass_wall_ms",
        "greedy_seed_score",
        "greedy_seed_ler_area",
    ]

    available = [p for p in predictors if p in df.columns]
    correlation_summary(df, available)

    # Density bins
    if "density" in df.columns:
        bin_rate(df, "density",
                 [0, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0],
                 "Density (item_area / grid_area)")
        threshold_scan(df, "density",
                       [0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
                       direction="above")

    # First-pass concentration bins
    if "first_pass_concentration" in df.columns:
        bin_rate(df, "first_pass_concentration",
                 [0, 0.5, 0.7, 0.8, 0.9, 0.95, 0.98, 1.0],
                 "First-pass concentration")
        threshold_scan(df, "first_pass_concentration",
                       [0.80, 0.85, 0.90, 0.95, 0.98, 0.99, 1.0],
                       direction="below")

    # First-pass stranded cells
    if "first_pass_stranded" in df.columns:
        bin_rate(df, "first_pass_stranded",
                 [-0.5, 0.5, 1.5, 2.5, 4.5, 8.5, 100],
                 "First-pass stranded cells")
        threshold_scan(df, "first_pass_stranded",
                       [0, 1, 2, 3, 5, 8],
                       direction="above")

    # Num items
    if "num_items" in df.columns:
        bin_rate(df, "num_items",
                 [0, 5, 10, 15, 20, 30, 50],
                 "Num items")

    # Score delta (how much refinement found)
    if "first_pass_score" in df.columns and "score" in df.columns:
        df["score_delta"] = df["score"] - df["first_pass_score"]
        replaced_df = df[df["refine_replaced"] == 1]
        if not replaced_df.empty:
            print("\nScore delta when replaced (refinement improvement):")
            print("  median: {:+.0f}".format(replaced_df["score_delta"].median()))
            print("  mean:   {:+.0f}".format(replaced_df["score_delta"].mean()))
            print("  p10:    {:+.0f}".format(replaced_df["score_delta"].quantile(0.10)))
            print("  p90:    {:+.0f}".format(replaced_df["score_delta"].quantile(0.90)))

            # Score delta by density
            if "density" in replaced_df.columns:
                print("\nScore delta by density (replaced rows only):")
                replaced_df = replaced_df.copy()
                replaced_df["_dbin"] = pd.cut(replaced_df["density"],
                    bins=[0, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0],
                    include_lowest=True)
                for dbin, grp in replaced_df.groupby("_dbin"):
                    if len(grp) == 0:
                        continue
                    print("  {}: n={}, median delta={:+.0f}".format(
                        dbin, len(grp), grp["score_delta"].median()))

    return 0


if __name__ == "__main__":
    sys.exit(main())
