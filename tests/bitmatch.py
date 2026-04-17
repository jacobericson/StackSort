#!/usr/bin/env python3
"""
bitmatch.py -- strict bit-match of scoring columns: baseline vs new run.

Joins on (config_name, instance_name, seed, target, rotate_all) and the
refinement-bucket key, then asserts every scoring metric is identical.

Usage:
  python tests/bitmatch.py \\
      --before tests/results/profile_baseline.csv \\
      --after "tests/results/profile_new/*.csv"

Output: byte-match / drift summary, pinpointed mismatched columns + first
5 offending rows per column if any.
"""
import argparse
import os
import sys
import glob
import pandas as pd


# These must match byte-for-byte.
EXACT_COLS = [
    "all_placed", "num_placed", "score",
    "ler_area", "ler_width", "ler_height", "ler_x", "ler_y",
    "concentration", "stranded_cells", "grouping_bonus", "num_rotated",
]

# Join keys -- every row in BEFORE should have exactly one match in AFTER.
JOIN_KEYS = [
    "config_name", "instance_name", "seed", "target", "rotate_all",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--before", required=True,
                    help="Baseline CSV path")
    ap.add_argument("--after", required=True,
                    help="Glob for new-run CSV(s)")
    args = ap.parse_args()

    if not os.path.exists(args.before):
        print("ERROR: baseline CSV missing: {}".format(args.before), file=sys.stderr)
        return 1
    after_files = glob.glob(args.after)
    if not after_files:
        print("ERROR: new-run CSVs missing: {}".format(args.after), file=sys.stderr)
        return 1

    before = pd.read_csv(args.before)
    after = pd.concat([pd.read_csv(f) for f in after_files], ignore_index=True)

    # Harness may emit an auxiliary 'refine' flag column. Include it in join
    # so first-pass and refined rows aren't cross-matched.
    refine_col = None
    for cand in ("refine", "did_refine", "refined"):
        if cand in before.columns and cand in after.columns:
            refine_col = cand
            break
    keys = list(JOIN_KEYS)
    if refine_col:
        keys.append(refine_col)

    # Scope before to only baseline config so cross-config CSVs don't fight.
    before = before[before["config_name"] == "baseline"].copy()

    print("BEFORE rows: {}   AFTER rows: {}".format(len(before), len(after)))
    print("Join keys: {}".format(keys))

    merged = before.merge(after, on=keys, how="inner", suffixes=("_b", "_a"))
    print("Joined rows: {}".format(len(merged)))
    if len(merged) != len(before):
        print("WARN: {} BEFORE rows had no AFTER match".format(
            len(before) - len(merged)))

    all_ok = True
    for col in EXACT_COLS:
        if col + "_b" not in merged.columns:
            continue
        b = merged[col + "_b"]
        a = merged[col + "_a"]
        diff = (b != a) & ~(b.isna() & a.isna())
        n_diff = int(diff.sum())
        if n_diff == 0:
            print("  OK   {:20s} identical ({} rows)".format(col, len(merged)))
        else:
            all_ok = False
            print("  FAIL {:20s} {} rows differ".format(col, n_diff))
            sample = merged.loc[diff, keys + [col + "_b", col + "_a"]].head(5)
            for _, row in sample.iterrows():
                k = " ".join("{}={}".format(k, row[k]) for k in keys)
                print("       {} | before={} after={}".format(
                    k, row[col + "_b"], row[col + "_a"]))

    if all_ok:
        print("")
        print("BIT-MATCH: PASS")
        return 0
    else:
        print("")
        print("BIT-MATCH: FAIL")
        return 1


if __name__ == "__main__":
    sys.exit(main())
