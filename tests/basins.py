import pandas as pd


def detect_bimodal(scores, min_gap=500):
    """Returns (True, midpoint_threshold) if sorted scores have a gap >= min_gap,
    else (False, None)."""
    s = sorted(scores)
    if len(s) < 4:
        return False, None
    best_gap = 0
    best_i = -1
    for i in range(1, len(s)):
        g = s[i] - s[i - 1]
        if g > best_gap:
            best_gap = g
            best_i = i
    if best_gap >= min_gap:
        return True, (s[best_i - 1] + s[best_i]) / 2.0
    return False, None


def stratified_bimodal_cells(df, score_col="score", min_gap=500):
    """Groups by (rotate_all, instance_name, target, num_placed) before
    running detect_bimodal — seeds that placed different numbers of items
    span the ~1M-point tier-1 gap, so detecting basins across that boundary
    would be spurious.

    Returns list of dicts: {rot, inst, tgt, num_placed, thresh, low_count,
    high_count, n, mixed_placement}."""
    cells = []
    for (rot, inst, tgt), grp in df.groupby(
        ["rotate_all", "instance_name", "target"]
    ):
        placement_values = grp["num_placed"].unique()
        mixed = len(placement_values) > 1
        for np_val in sorted(placement_values):
            sub = grp[grp["num_placed"] == np_val]
            if len(sub) < 4:
                continue
            is_bi, thresh = detect_bimodal(sub[score_col].tolist(), min_gap)
            if not is_bi:
                continue
            cnt = len(sub)
            low = int((sub[score_col] < thresh).sum())
            cells.append({
                "rot": int(rot),
                "inst": inst,
                "tgt": int(tgt),
                "num_placed": int(np_val),
                "thresh": float(thresh),
                "low_count": low,
                "high_count": cnt - low,
                "n": cnt,
                "mixed_placement": mixed,
            })
    return cells


def basin_rates(df, cells, score_col="score"):
    """Returns dict {config_name: (hits, total, rate)}."""
    thresh_map = {
        (c["rot"], c["inst"], c["tgt"], c["num_placed"]): c["thresh"]
        for c in cells
    }
    out = {}
    for cfg, cfg_df in df.groupby("config_name"):
        hits = 0
        total = 0
        for (rot, inst, tgt, np_val), grp in cfg_df.groupby(
            ["rotate_all", "instance_name", "target", "num_placed"]
        ):
            key = (int(rot), inst, int(tgt), int(np_val))
            if key not in thresh_map:
                continue
            t = thresh_map[key]
            total += len(grp)
            hits += int((grp[score_col] > t).sum())
        rate = hits / total if total > 0 else 0.0
        out[cfg] = (hits, total, rate)
    return out
