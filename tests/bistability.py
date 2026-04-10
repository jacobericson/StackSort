#!/usr/bin/env python3
import argparse
import glob
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

from basins import stratified_bimodal_cells, basin_rates, detect_bimodal


def load_csvs(paths):
    frames = []
    for p in paths:
        try:
            df = pd.read_csv(p)
        except Exception as e:
            print("WARN: {}: {}".format(p, e), file=sys.stderr)
            continue
        frames.append(df)
    if not frames:
        return None
    df = pd.concat(frames, ignore_index=True)
    # v3: derive score_no_grouping for cross-weight drilldowns.
    if "scoring_grouping_weight" in df.columns and "grouping_bonus" in df.columns:
        contrib = (df["grouping_bonus"] * df["concentration"]
                   * df["scoring_grouping_weight"])
        df["score_no_grouping"] = df["score"] - contrib.apply(
            lambda x: int(x) if pd.notna(x) else 0)
    return df


def print_bimodal_summary(df, baseline_name):
    base = df[df["config_name"] == baseline_name]
    if base.empty:
        print("No baseline rows found.")
        return

    cells = stratified_bimodal_cells(base, score_col="score")
    print("=" * 78)
    print("Bimodal cells detected on baseline (stratified by num_placed)")
    print("=" * 78)
    print("{:>4} {:<28} {:>4} {:>10} {:>5} {:>5} {:>6} {:>12}".format(
        "rot", "instance", "tgt", "num_placed", "low", "high", "n", "threshold"))
    for c in cells:
        mixed = " (mixed)" if c["mixed_placement"] else ""
        print("{:>4} {:<28} {:>4} {:>10}{:<8} {:>5} {:>5} {:>6} {:>12.0f}".format(
            c["rot"], c["inst"], c["tgt"], c["num_placed"], mixed,
            c["low_count"], c["high_count"], c["n"], c["thresh"]))
    print("Total: {}".format(len(cells)))
    print()

    if not cells:
        return

    has_v2 = "first_pass_score" in df.columns
    rates_final = basin_rates(df, cells, score_col="score")
    if has_v2:
        rates_first = basin_rates(df, cells, score_col="first_pass_score")

    sorted_rates = sorted(rates_final.items(), key=lambda kv: -kv[1][2])
    base_final = rates_final.get(baseline_name, (0, 0, 0.0))[2]

    print("=" * 78)
    print("Per-config high-basin rate")
    print("=" * 78)
    if has_v2:
        base_first = rates_first.get(baseline_name, (0, 0, 0.0))[2]
        print("{:<28} {:>12} {:>10} {:>10} {:>10}".format(
            "config", "first pass", "delta", "final", "delta"))
        for cfg, (hits, total, rate) in sorted_rates:
            f_rate = rates_first.get(cfg, (0, 0, 0.0))[2]
            tag = "  <-- baseline" if cfg == baseline_name else ""
            print("{:<28} {:>11.1%} {:>+9.1f} {:>9.1%} {:>+9.1f}{}".format(
                cfg, f_rate, (f_rate - base_first) * 100,
                rate, (rate - base_final) * 100, tag))
    else:
        print("{:<28} {:>10} {:>10}".format("config", "rate", "delta vs baseline"))
        for cfg, (hits, total, rate) in sorted_rates:
            tag = "  <-- baseline" if cfg == baseline_name else ""
            print("{:<28} {:>9.1%} {:>+9.1f}{}".format(
                cfg, rate, (rate - base_final) * 100, tag))


def print_drilldown(df, instance, target, rot, baseline_name):
    sub = df[(df["rotate_all"] == rot) &
             (df["instance_name"] == instance) &
             (df["target"] == target) &
             (df["config_name"] == baseline_name)]
    if sub.empty:
        print("No {} rows for {} target={} rot={}".format(
            baseline_name, instance, target, rot))
        return

    print("=" * 78)
    print("Drilldown: {} target={} rot={} ({})".format(
        instance, target, rot, baseline_name))
    print("=" * 78)

    has_v2 = "first_pass_score" in sub.columns
    has_v3 = "score_no_grouping" in sub.columns
    is_bi, thresh = detect_bimodal(sub["score"].tolist())
    if is_bi:
        print("Bimodal threshold: {:.0f}".format(thresh))
    else:
        print("Unimodal (largest score gap < 500)")
    print("Seeds: {}".format(len(sub)))
    print("Grid: {}x{}, {} items".format(
        sub["grid_w"].iloc[0], sub["grid_h"].iloc[0],
        sub["num_items"].iloc[0]))
    if has_v3:
        gw = int(sub["scoring_grouping_weight"].iloc[0])
        print("Scoring grouping weight: {}".format(gw))
    print()

    sub_sorted = sub.sort_values("score")
    if has_v2:
        if has_v3:
            print("{:>5} {:>12} {:>12} {:>12} {:>8} {:>6} {:>6} {:>8} {:>8}".format(
                "seed", "first_pass", "final", "sng_final", "ler", "grp", "fpgrp",
                "refTrig", "refRepl"))
            for _, r in sub_sorted.iterrows():
                print("{:>5} {:>12} {:>12} {:>12} {:>8} {:>6} {:>6} {:>8} {:>8}".format(
                    int(r["seed"]), int(r["first_pass_score"]), int(r["score"]),
                    int(r["score_no_grouping"]),
                    int(r["ler_area"]), int(r["grouping_bonus"]),
                    int(r["first_pass_grouping_bonus"]),
                    int(r["refine_triggered"]), int(r["refine_replaced"])))
        else:
            print("{:>5} {:>12} {:>12} {:>8} {:>6} {:>6} {:>8} {:>8}".format(
                "seed", "first_pass", "final", "ler", "grp", "fpgrp",
                "refTrig", "refRepl"))
            for _, r in sub_sorted.iterrows():
                print("{:>5} {:>12} {:>12} {:>8} {:>6} {:>6} {:>8} {:>8}".format(
                    int(r["seed"]), int(r["first_pass_score"]), int(r["score"]),
                    int(r["ler_area"]), int(r["grouping_bonus"]),
                    int(r["first_pass_grouping_bonus"]),
                    int(r["refine_triggered"]), int(r["refine_replaced"])))

        # Aggregate lift
        refined = sub[sub["refine_replaced"] == 1]
        if len(refined):
            score_lift = (refined["score"] - refined["first_pass_score"]).median()
            group_lift = (refined["grouping_bonus"]
                          - refined["first_pass_grouping_bonus"]).median()
            print()
            print("Refinement replaced {}/{} seeds".format(len(refined), len(sub)))
            print("  median score lift:    {:+d}".format(int(score_lift)))
            print("  median grouping lift: {:+d}".format(int(group_lift)))
    else:
        print("{:>5} {:>12} {:>8} {:>6} {:>6} {:>6}".format(
            "seed", "score", "ler", "grp", "conc", "strand"))
        for _, r in sub_sorted.iterrows():
            print("{:>5} {:>12} {:>8} {:>6} {:>6.3f} {:>6}".format(
                int(r["seed"]), int(r["score"]), int(r["ler_area"]),
                int(r["grouping_bonus"]), float(r["concentration"]),
                int(r["stranded_cells"])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+", help="CSV file(s) or glob patterns")
    ap.add_argument("--baseline", default="baseline")
    ap.add_argument("--drilldown", nargs=3, metavar=("INST", "TGT", "ROT"),
                    help="Show per-seed details for one (instance, target, rot) cell")
    args = ap.parse_args()

    paths = []
    for p in args.csv:
        expanded = glob.glob(p)
        if expanded:
            paths.extend(expanded)
        elif os.path.exists(p):
            paths.append(p)
    if not paths:
        print("ERROR: no input files", file=sys.stderr)
        return 1

    df = load_csvs(paths)
    if df is None or df.empty:
        print("ERROR: no data loaded", file=sys.stderr)
        return 1

    if args.drilldown:
        inst = args.drilldown[0]
        tgt = int(args.drilldown[1])
        rot = int(args.drilldown[2])
        print_drilldown(df, inst, tgt, rot, args.baseline)
    else:
        print_bimodal_summary(df, args.baseline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
