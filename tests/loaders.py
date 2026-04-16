import glob as _glob
import sys

import pandas as pd


def load_csvs(paths_or_patterns):
    """Read CSVs from a mixed list of file paths and glob patterns.

    Returns (DataFrame, max_schema_version). Derives score_no_grouping
    when scoring_grouping_weight + grouping_bonus columns exist.

    Uses int() (truncation toward zero) for score_no_grouping to match
    C++ (long long) cast in ComputeScore; do NOT use round().
    """
    frames = []
    max_schema = 1
    for entry in paths_or_patterns:
        expanded = _glob.glob(entry)
        if not expanded and not any(ch in entry for ch in "*?["):
            expanded = [entry]
        for f in expanded:
            try:
                df = pd.read_csv(f, skip_blank_lines=True)
            except Exception as e:
                print("WARN: failed to read {}: {}".format(f, e), file=sys.stderr)
                continue
            if "schema_version" in df.columns and len(df) > 0:
                v = int(df["schema_version"].iloc[0])
                if v > max_schema:
                    max_schema = v
            frames.append(df)
    if not frames:
        return None, max_schema
    df = pd.concat(frames, ignore_index=True)
    if "scoring_grouping_weight" in df.columns and "grouping_bonus" in df.columns:
        contrib = (df["grouping_bonus"] * df["concentration"]
                   * df["scoring_grouping_weight"])
        df["score_no_grouping"] = df["score"] - contrib.apply(
            lambda x: int(x) if pd.notna(x) else 0)
        if "first_pass_score" in df.columns and "first_pass_grouping_bonus" in df.columns:
            fp_contrib = (df["first_pass_grouping_bonus"]
                          * df["first_pass_concentration"]
                          * df["scoring_grouping_weight"])
            df["first_pass_score_no_grouping"] = (
                df["first_pass_score"] - fp_contrib.apply(
                    lambda x: int(x) if pd.notna(x) else 0))
    return df, max_schema
