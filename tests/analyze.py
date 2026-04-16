#!/usr/bin/env python3
import argparse
import glob
import os
import sys

try:
    import pandas as pd
except ImportError:
    print("ERROR: pandas not installed. `pip install pandas`.", file=sys.stderr)
    sys.exit(1)

# tests/basins.py lives next to this script.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from basins import stratified_bimodal_cells, basin_rates
from loaders import load_csvs


def fmt_int(v):
    if pd.isna(v):
        return "-"
    return "{:,.0f}".format(v)


def fmt_delta(v):
    if pd.isna(v):
        return "-"
    if v > 0:
        return "+{:,.0f}".format(v)
    if v < 0:
        return "{:,.0f}".format(v)
    return "0"


def fmt_conc_delta(v):
    # Concentration is HHI (0..1 float). Deltas below 0.01 are numerical noise.
    if pd.isna(v):
        return "-"
    if abs(v) < 0.01:
        return "0"
    return "{:+.3f}".format(v)


def fmt_pct(v):
    if pd.isna(v):
        return "-"
    return "{:.1%}".format(v)


def fmt_ms(v):
    if pd.isna(v):
        return "-"
    return "{:.1f}".format(v)


def fmt_pp_delta(v):
    # Percentage-point delta (already pre-multiplied by 100).
    if pd.isna(v) or abs(v) < 0.05:
        return "0.0"
    if v > 0:
        return "+{:.1f}".format(v)
    return "{:.1f}".format(v)


def load_results(paths):
    return load_csvs(paths)


def comparison_table(df, rotate_all, baseline_name, is_v2):
    sub = df[df["rotate_all"] == rotate_all]
    if sub.empty:
        return "_(no rows for rotate_all={})_".format(rotate_all)

    configs = list(sub["config_name"].unique())
    if baseline_name in configs:
        configs.remove(baseline_name)
        configs.insert(0, baseline_name)

    has_refine_cols = is_v2 and "first_pass_wall_ms" in sub.columns

    headers = ["Config", "n", "Score median", "Score p10", "Score p90",
               "LER median", "Wall ms", "Placed%"]
    if has_refine_cols:
        headers += ["First ms", "Refine ms", "Trigger rate"]

    header_row = "| " + " | ".join(headers) + " |"
    sep_row = "|---" + "|---:" * (len(headers) - 1) + "|"
    lines = [header_row, sep_row]

    for cfg in configs:
        c = sub[sub["config_name"] == cfg]
        cells = [cfg, "{}".format(len(c))]
        cells.append(fmt_int(c["score"].median()))
        cells.append(fmt_int(c["score"].quantile(0.10)))
        cells.append(fmt_int(c["score"].quantile(0.90)))
        cells.append(fmt_int(c["ler_area"].median()))
        cells.append(fmt_ms(c["wall_clock_ms"].median()))
        cells.append(fmt_pct(c["all_placed"].mean()))
        if has_refine_cols:
            cells.append(fmt_ms(c["first_pass_wall_ms"].median()))
            triggered = c[c["refine_triggered"] == 1]
            if len(triggered) > 0:
                cells.append(fmt_ms(triggered["refine_wall_ms"].median()))
            else:
                cells.append("-")
            cells.append(fmt_pct((c["refine_triggered"] == 1).mean()))
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def placement_regression_section(df, rotate_all, baseline_name):
    sub = df[df["rotate_all"] == rotate_all]
    if sub.empty:
        return None

    med = sub.groupby(["config_name", "instance_name", "target"])["num_placed"] \
             .median().reset_index()
    base = med[med["config_name"] == baseline_name]
    if base.empty:
        return None
    base_map = base.set_index(["instance_name", "target"])["num_placed"]

    regressions = []
    for _, r in med.iterrows():
        if r["config_name"] == baseline_name:
            continue
        b = base_map.get((r["instance_name"], r["target"]))
        if b is None or pd.isna(b):
            continue
        if r["num_placed"] < b:
            regressions.append({
                "config": r["config_name"],
                "inst": r["instance_name"],
                "tgt": int(r["target"]),
                "baseline_placed": int(b),
                "ablation_placed": int(r["num_placed"]),
            })
    if not regressions:
        return None

    regressions.sort(key=lambda r: (r["config"], r["inst"], r["tgt"]))
    lines = ["| Config | Instance | Target | Baseline | Ablation |",
             "|---|---|---:|---:|---:|"]
    for r in regressions:
        lines.append("| {} | {} | {} | {} | {} |".format(
            r["config"], r["inst"], r["tgt"],
            r["baseline_placed"], r["ablation_placed"]))
    return "\n".join(lines)


def component_delta_heatmap(df, rotate_all, baseline_name, column, title,
                            fmt=None, filter_placed=True):
    if fmt is None:
        fmt = fmt_delta

    sub = df[df["rotate_all"] == rotate_all]
    if sub.empty or column not in sub.columns:
        return "_(no rows for {} at rotate_all={})_".format(column, rotate_all)
    if filter_placed:
        sub = sub[sub["all_placed"] == 1]
    if sub.empty:
        return "_(no fully-placed rows for {} at rotate_all={})_".format(
            column, rotate_all)

    med = sub.groupby(["config_name", "instance_name", "target"])[column] \
             .median().reset_index()

    base = med[med["config_name"] == baseline_name]
    if base.empty:
        return "_(no baseline '{}' in data)_".format(baseline_name)
    base_map = base.set_index(["instance_name", "target"])[column]

    med["delta"] = med.apply(
        lambda r: r[column] - base_map.get((r["instance_name"], r["target"]),
                                           float("nan")),
        axis=1,
    )

    inst_med = med[med["config_name"] != baseline_name].groupby(
        ["config_name", "instance_name"])["delta"].median().reset_index()

    pivot = inst_med.pivot(index="instance_name", columns="config_name",
                           values="delta").sort_index()
    if pivot.empty:
        return "_(no non-baseline configs in data)_"

    cols = list(pivot.columns)
    lines = ["**{}**".format(title), ""]
    lines.append("| Instance | " + " | ".join(cols) + " |")
    lines.append("|---|" + "---:|" * len(cols))
    for inst in pivot.index:
        row = "| " + inst + " | " + " | ".join(
            fmt(pivot.at[inst, c]) for c in cols) + " |"
        lines.append(row)
    return "\n".join(lines)


def _basin_block(sub, baseline_name, is_v2, score_col, first_pass_col, header_note):
    """Splits weight=1 vs weight=2 configs so formula-offset scores don't
    contaminate bimodality detection or basin rate comparisons."""
    base_sub = sub[sub["config_name"] == baseline_name]
    if base_sub.empty:
        return None
    cells = stratified_bimodal_cells(base_sub, score_col=score_col)
    if not cells:
        return ("_(no bimodal cells detected on baseline using `{}`)_".format(
            score_col))

    rates_final = basin_rates(sub, cells, score_col=score_col)
    sorted_rates = sorted(rates_final.items(), key=lambda kv: -kv[1][2])
    base_rate = rates_final.get(baseline_name, (0, 0, 0.0))[2]

    has_first = first_pass_col and first_pass_col in sub.columns and is_v2

    lines = [header_note, "",
             "{} bimodal cells detected on baseline (stratified by num_placed, score col `{}`).".format(
                 len(cells), score_col),
             ""]
    if has_first:
        rates_first = basin_rates(sub, cells, score_col=first_pass_col)
        first_base = rates_first.get(baseline_name, (0, 0, 0.0))[2]
        lines.append("| Config | First pass | delta vs base | Final | delta vs base | Refinement lift |")
        lines.append("|---|---:|---:|---:|---:|---:|")
        for cfg, (_, _, rate) in sorted_rates:
            f_rate = rates_first.get(cfg, (0, 0, 0.0))[2]
            tag = " **(baseline)**" if cfg == baseline_name else ""
            lines.append("| {}{} | {:.1%} | {} | {:.1%} | {} | {:+.1f}pp |".format(
                cfg, tag,
                f_rate, fmt_pp_delta((f_rate - first_base) * 100),
                rate, fmt_pp_delta((rate - base_rate) * 100),
                (rate - f_rate) * 100,
            ))
    else:
        lines.append("| Config | Rate | delta |")
        lines.append("|---|---:|---:|")
        for cfg, (_, _, rate) in sorted_rates:
            tag = " **(baseline)**" if cfg == baseline_name else ""
            lines.append("| {}{} | {:.1%} | {} |".format(
                cfg, tag, rate, fmt_pp_delta((rate - base_rate) * 100)))
    return "\n".join(lines)


def basin_section(df, rotate_all, baseline_name, is_v2):
    sub = df[df["rotate_all"] == rotate_all]
    if sub.empty:
        return "_(no rows)_"

    base_sub = sub[sub["config_name"] == baseline_name]
    if base_sub.empty:
        return "_(no baseline rows)_"

    # Split configs by whether their scoring formula (weight + power exponent)
    # matches baseline. Formula-matched configs use raw `score`. Formula-mismatch
    # configs use `score_no_grouping` to avoid formula-offset bias.
    # The grouping power exponent (v4) changes raw grouping_bonus scale just
    # like weight does, so configs with a different power must also fall into
    # the cross bucket.
    if "scoring_grouping_weight" in sub.columns:
        base_gw = base_sub["scoring_grouping_weight"].iloc[0]
        formula_match = (sub["scoring_grouping_weight"] == base_gw)
        if "scoring_grouping_power_quarters" in sub.columns:
            base_gpq = base_sub["scoring_grouping_power_quarters"].iloc[0]
            formula_match &= (sub["scoring_grouping_power_quarters"] == base_gpq)
        weight_matched = sub[formula_match]
        weight_other = sub[~formula_match]
    else:
        weight_matched = sub
        weight_other = sub.iloc[0:0]  # empty

    blocks = []
    matched_block = _basin_block(
        weight_matched, baseline_name, is_v2,
        score_col="score", first_pass_col="first_pass_score",
        header_note="**Weight-matched configs** (scored on `score`):")
    if matched_block:
        blocks.append(matched_block)

    if not weight_other.empty and "score_no_grouping" in weight_other.columns:
        # Include baseline rows in the weight_other block so we have a baseline
        # to compute rates against, but use score_no_grouping for fair
        # comparison across the formula offset.
        weight_other_with_base = pd.concat(
            [sub[sub["config_name"] == baseline_name], weight_other],
            ignore_index=True)
        other_block = _basin_block(
            weight_other_with_base, baseline_name, is_v2,
            score_col="score_no_grouping",
            first_pass_col="first_pass_score_no_grouping",
            header_note=("**Cross-weight configs** (scored on "
                         "`score_no_grouping` to remove formula offset):"))
        if other_block:
            blocks.append(other_block)

    if not blocks:
        return "_(no basin rate data)_"
    return "\n\n".join(blocks)


def refinement_section(df, rotate_all, baseline_name):
    sub = df[df["rotate_all"] == rotate_all]
    if sub.empty or "refine_triggered" not in sub.columns:
        return None

    configs = list(sub["config_name"].unique())
    if baseline_name in configs:
        configs.remove(baseline_name)
        configs.insert(0, baseline_name)

    lines = ["| Config | Trigger rate | Replace rate | Median refine ms | Median score lift | Median grouping lift |",
             "|---|---:|---:|---:|---:|---:|"]
    for cfg in configs:
        c = sub[sub["config_name"] == cfg]
        if c.empty:
            continue
        trigger_rate = (c["refine_triggered"] == 1).mean()
        triggered = c[c["refine_triggered"] == 1]
        replaced = c[c["refine_replaced"] == 1]
        if len(triggered) > 0:
            replace_rate = len(replaced) / len(triggered)
            refine_ms = triggered["refine_wall_ms"].median()
        else:
            replace_rate = 0.0
            refine_ms = 0.0
        if len(replaced) > 0:
            score_lift = (replaced["score"]
                          - replaced["first_pass_score"]).median()
            group_lift = (replaced["grouping_bonus"]
                          - replaced["first_pass_grouping_bonus"]).median()
        else:
            score_lift = 0
            group_lift = 0
        lines.append("| {} | {} | {} | {} | {} | {} |".format(
            cfg, fmt_pct(trigger_rate), fmt_pct(replace_rate),
            fmt_ms(refine_ms), fmt_delta(score_lift), fmt_delta(group_lift),
        ))
    return "\n".join(lines)


def scoring_weight_ablation(df, rotate_all, baseline_name):
    sub = df[df["rotate_all"] == rotate_all]
    if sub.empty or "scoring_grouping_weight" not in sub.columns:
        return None
    if sub["scoring_grouping_weight"].nunique() < 2:
        return None
    if "score_no_grouping" not in sub.columns:
        return None

    sub = sub[sub["all_placed"] == 1]
    if sub.empty:
        return None

    base = sub[sub["config_name"] == baseline_name]
    if base.empty:
        return None
    base_gw = base["scoring_grouping_weight"].iloc[0]

    # Grouping-heavy instances per spec section 7.2.
    GROUPING_HEAVY = ["typical_backpack_trader", "typical_backpack_mixed",
                      "tight_backpack_stuffed"]

    # Per-(instance, target) medians for each config.
    med = sub.groupby(["config_name", "scoring_grouping_weight",
                       "instance_name", "target"]).agg(
        grouping_bonus=("grouping_bonus", "median"),
        score_no_grouping=("score_no_grouping", "median"),
        ler_area=("ler_area", "median"),
        stranded_cells=("stranded_cells", "median"),
        num_placed=("num_placed", "median"),
    ).reset_index()

    base_med = med[med["config_name"] == baseline_name].set_index(
        ["instance_name", "target"])

    other_configs = [c for c in sub["config_name"].unique() if c != baseline_name]
    w2_configs = [c for c in other_configs
                  if sub[sub["config_name"] == c]["scoring_grouping_weight"].iloc[0] != base_gw]
    if not w2_configs:
        return None

    lines = []
    lines.append("**Scaling-trap note:** `grouping_bonus` is a raw count "
                 "(weight-independent), but `score` bakes in "
                 "`grouping_bonus * conc * weight`. Never compare raw `score` "
                 "across different scoring weights. Primary axes below are "
                 "**raw grouping_bonus** (direct clustering signal) and "
                 "**score_no_grouping** (LER/stranded/placement quality with "
                 "each config's own grouping contribution removed). At "
                 "identical raw grouping_bonus, weight=2 automatically adds "
                 "`baseline_GB * baseline_conc * 1` to composite score for "
                 "free -- that is not a win, it's matching baseline.")
    lines.append("")

    for cfg in w2_configs:
        cfg_gw = sub[sub["config_name"] == cfg]["scoring_grouping_weight"].iloc[0]
        lines.append("#### `{}` (weight={}) vs `{}` (weight={})".format(
            cfg, cfg_gw, baseline_name, base_gw))
        lines.append("")

        cfg_med = med[med["config_name"] == cfg].set_index(
            ["instance_name", "target"])

        # Verdict gates
        grouping_heavy_deltas = []
        other_deltas = []
        score_no_grouping_deltas = []
        placed_deltas = []
        for key in cfg_med.index:
            if key not in base_med.index:
                continue
            inst = key[0]
            gb_delta = cfg_med.at[key, "grouping_bonus"] - base_med.at[key, "grouping_bonus"]
            sng_delta = cfg_med.at[key, "score_no_grouping"] - base_med.at[key, "score_no_grouping"]
            placed_delta = cfg_med.at[key, "num_placed"] - base_med.at[key, "num_placed"]
            if inst in GROUPING_HEAVY:
                grouping_heavy_deltas.append(gb_delta)
            else:
                other_deltas.append(gb_delta)
            score_no_grouping_deltas.append(sng_delta)
            placed_deltas.append(placed_delta)

        def median(xs):
            if not xs:
                return 0
            s = sorted(xs)
            return s[len(s) // 2]

        gh_med = median(grouping_heavy_deltas)
        other_med = median(other_deltas)
        sng_med = median(score_no_grouping_deltas)
        placed_med = median(placed_deltas)

        # Verdict
        wins = (gh_med >= 10 and other_med >= 0 and sng_med >= -100
                and placed_med == 0)
        neutral = (5 <= gh_med < 10 and sng_med >= -100 and placed_med == 0)
        if wins:
            verdict = "**WIN** -- ship it"
        elif neutral:
            verdict = "**NEUTRAL** -- formula-only gain, marginal search gain"
        else:
            verdict = "**LOSS** -- below threshold or LER regression"
        lines.append(
            "**Verdict:** {}. Median grouping_bonus delta: {} on "
            "grouping-heavy instances, {} elsewhere. Median "
            "score_no_grouping delta: {}. Placement delta: {}.".format(
                verdict, fmt_delta(gh_med), fmt_delta(other_med),
                fmt_delta(sng_med), fmt_delta(placed_med)))
        lines.append("")

        # Per-instance table
        inst_data = []
        for inst in sorted(set(k[0] for k in cfg_med.index if k in base_med.index)):
            base_gb_vals = []
            gb_deltas = []
            sng_deltas = []
            la_deltas = []
            strand_deltas = []
            placed_deltas_i = []
            for key in cfg_med.index:
                if key[0] != inst or key not in base_med.index:
                    continue
                base_gb_vals.append(base_med.at[key, "grouping_bonus"])
                gb_deltas.append(cfg_med.at[key, "grouping_bonus"] - base_med.at[key, "grouping_bonus"])
                sng_deltas.append(cfg_med.at[key, "score_no_grouping"] - base_med.at[key, "score_no_grouping"])
                la_deltas.append(cfg_med.at[key, "ler_area"] - base_med.at[key, "ler_area"])
                strand_deltas.append(cfg_med.at[key, "stranded_cells"] - base_med.at[key, "stranded_cells"])
                placed_deltas_i.append(cfg_med.at[key, "num_placed"] - base_med.at[key, "num_placed"])
            if not gb_deltas:
                continue
            base_gb = median(base_gb_vals)
            gb_d = median(gb_deltas)
            pct = (gb_d / base_gb * 100.0) if base_gb else 0.0
            inst_data.append((inst, base_gb, gb_d, pct,
                              median(sng_deltas), median(la_deltas),
                              median(strand_deltas), median(placed_deltas_i)))

        if inst_data:
            lines.append("| Instance | baseline GB | raw GB Δ | GB Δ (%) | score_no_grp Δ | lerArea Δ | stranded Δ | placed Δ |")
            lines.append("|---|---:|---:|---:|---:|---:|---:|---:|")
            for inst, base_gb, gb_d, pct, sng_d, la_d, st_d, pl_d in inst_data:
                tag = " *(grouping-heavy)*" if inst in GROUPING_HEAVY else ""
                lines.append("| {}{} | {} | {} | {:+.1f}% | {} | {} | {} | {} |".format(
                    inst, tag, fmt_int(base_gb), fmt_delta(gb_d), pct,
                    fmt_delta(sng_d), fmt_delta(la_d), fmt_delta(st_d),
                    fmt_delta(pl_d)))
            lines.append("")

        lines.append("_Tier-magnitude reminder: `lerArea^2` at lerArea~50 is "
                     "~100 points per cell. `score_no_grouping` deltas under "
                     "+-50 are LER noise. Raw grouping_bonus +10 at weight=2 "
                     "= +20 composite points, which is inside LER noise._")
        lines.append("")

    return "\n".join(lines) if lines else None


def repair_diagnostics_section(df, rotate_all, baseline_name):
    sub = df[df["rotate_all"] == rotate_all]
    rolls_col = "repair_rolls" if "repair_rolls" in sub.columns else (
                "repair_attempts" if "repair_attempts" in sub.columns else None)
    if sub.empty or rolls_col is None:
        return None
    has_scans = "repair_scans" in sub.columns
    fp_rolls_col = "first_pass_repair_rolls" if "first_pass_repair_rolls" in sub.columns else (
                   "first_pass_repair_attempts" if "first_pass_repair_attempts" in sub.columns else None)
    fp_has_scans = "first_pass_repair_scans" in sub.columns

    lines = []

    configs = list(sub["config_name"].unique())
    if baseline_name in configs:
        configs.remove(baseline_name)
        configs.insert(0, baseline_name)

    scans_hdr = " | scans/rolls" if has_scans else ""
    lines.append("**Per-config repair counters** (final diag means, first-pass in parens):")
    lines.append("")
    lines.append("| Config | rolls/run{} | hits/scans | accepts/hits |".format(scans_hdr))
    lines.append("|---|---:|{}---:|---:|".format("---:|" if has_scans else ""))
    for cfg in configs:
        c = sub[sub["config_name"] == cfg]
        if c.empty:
            continue
        rolls = c[rolls_col].mean()
        scans = c["repair_scans"].mean() if has_scans else rolls
        hits = c["repair_hits"].mean()
        acc = c["repair_accepts"].mean()
        fp_rolls = c[fp_rolls_col].mean() if fp_rolls_col else None
        fp_scans = c["first_pass_repair_scans"].mean() if fp_has_scans else fp_rolls
        fp_hits = c["first_pass_repair_hits"].mean() if "first_pass_repair_hits" in c.columns else None
        fp_acc = c["first_pass_repair_accepts"].mean() if "first_pass_repair_accepts" in c.columns else None
        hit_rate = (hits / scans) if scans else 0
        accept_rate = (acc / hits) if hits else 0
        scan_rate = (scans / rolls) if rolls else 0
        if fp_rolls is not None:
            rolls_str = "{:.0f} ({:.0f})".format(rolls, fp_rolls)
            fp_hit_rate = (fp_hits / fp_scans) if fp_scans else 0
            hit_str = "{:.1%} ({:.1%})".format(hit_rate, fp_hit_rate)
            acc_str = "{:.1%} ({:.1%})".format(
                accept_rate,
                (fp_acc / fp_hits) if fp_hits else 0)
        else:
            rolls_str = "{:.0f}".format(rolls)
            hit_str = "{:.1%}".format(hit_rate)
            acc_str = "{:.1%}".format(accept_rate)
        if has_scans:
            fp_scan_rate = (fp_scans / fp_rolls) if (fp_rolls and fp_has_scans) else 0
            scan_str = " | {:.1%} ({:.1%})".format(scan_rate, fp_scan_rate) if fp_rolls else " | {:.1%}".format(scan_rate)
            lines.append("| {} | {}{} | {} | {} |".format(cfg, rolls_str, scan_str, hit_str, acc_str))
        else:
            lines.append("| {} | {} | {} | {} |".format(cfg, rolls_str, hit_str, acc_str))
    lines.append("")

    # stranded_heavy outcome view
    sh = sub[sub["instance_name"] == "stranded_heavy"]
    if not sh.empty:
        lines.append("**`stranded_heavy` outcome** (median `stranded_cells` across seeds/targets):")
        lines.append("")
        lines.append("| Config | median stranded | max stranded | rows with stranded>0 |")
        lines.append("|---|---:|---:|---:|")
        for cfg in configs:
            c = sh[sh["config_name"] == cfg]
            if c.empty:
                continue
            med = c["stranded_cells"].median()
            mx = c["stranded_cells"].max()
            pct = (c["stranded_cells"] > 0).mean()
            lines.append("| {} | {:.1f} | {:.0f} | {:.1%} |".format(
                cfg, med, mx, pct))
        lines.append("")

    base = sub[sub["config_name"] == baseline_name]
    if not base.empty:
        rolls_sum = int(base[rolls_col].sum())
        scans_sum = int(base["repair_scans"].sum()) if has_scans else rolls_sum
        hit_sum = int(base["repair_hits"].sum())
        acc_sum = int(base["repair_accepts"].sum())
        if rolls_sum == 0:
            verdict = "dead rolls (repair bucket never fires)"
        elif scans_sum == 0:
            verdict = "rolls never scan (bestStranded==0 on all iters)"
        elif hit_sum == 0:
            verdict = "dead code on corpus (scans but never finds targets)"
        elif acc_sum == 0:
            verdict = "LAHC rejects (finds targets but candidate worse)"
        else:
            verdict = ("live feature: {:,} rolls / {:,} scans / {:,} hits / "
                       "{:,} accepts on baseline".format(
                           rolls_sum, scans_sum, hit_sum, acc_sum))
        lines.append("**Baseline rollup:** {}".format(verdict))
        lines.append("")

    return "\n".join(lines) if lines else None


def variance_floor(df, rotate_all, baseline_name):
    sub_base = df[(df["rotate_all"] == rotate_all) &
                  (df["config_name"] == baseline_name)]
    if sub_base.empty:
        return "_(no baseline rows)_"
    if sub_base["seed"].nunique() < 2:
        return ("_(only 1 seed; variance floor requires seeds >= 2. "
                "Re-run with `--seeds 5` or higher.)_")

    bimodal = stratified_bimodal_cells(sub_base, score_col="score")
    bimodal_keys = set((c["rot"], c["inst"], c["tgt"]) for c in bimodal)

    rows = []
    for inst, grp in sub_base.groupby("instance_name"):
        unimodal_iqrs = []
        for t, tgrp in grp.groupby("target"):
            if (int(rotate_all), inst, int(t)) in bimodal_keys:
                continue
            q25 = tgrp["score"].quantile(0.25)
            q75 = tgrp["score"].quantile(0.75)
            unimodal_iqrs.append(q75 - q25)
        if not unimodal_iqrs:
            continue
        median_iqr = sorted(unimodal_iqrs)[len(unimodal_iqrs) // 2]
        rows.append((inst, median_iqr))

    rows.sort()
    lines = ["| Instance | Baseline IQR (unimodal cells, median across targets) |",
             "|---|---:|"]
    for inst, iqr in rows:
        lines.append("| {} | {} |".format(inst, fmt_int(iqr)))
    return "\n".join(lines)


def report(df, baseline_name, schema_version):
    is_v2 = "refine_triggered" in df.columns

    sections = []
    sections.append("# StackSort tuning report")
    sections.append("")
    sections.append("Baseline: `{}`".format(baseline_name))
    sections.append("Schema version: {}".format(schema_version))
    sections.append("Total rows: {}".format(len(df)))
    sections.append("Configs: {}".format(", ".join(sorted(df["config_name"].unique()))))
    sections.append("Instances: {}".format(df["instance_name"].nunique()))
    sections.append("")

    sections.append("## How to read this report")
    sections.append("")
    sections.append(
        "Scores are decomposed by component so small grouping wins "
        "(e.g. a `+16` score delta from better clustering) aren't buried "
        "under LER noise or the `num_placed * 1,000,000` tier jump. A "
        "consistent `+N` delta on **grouping_bonus** at any magnitude is a "
        "real clustering change independent of LER or concentration; the "
        "player-experience goal is inventories that look hand-arranged, "
        "not just maximally packed, so treat grouping as a separate "
        "quality axis.")
    sections.append("")
    sections.append(
        "For cells where the LAHC lands in one of two distinct basins "
        "(bimodal), IQR-based noise floors miss the signal -- use the "
        "**Basin rate** section instead. The variance floor table below "
        "covers unimodal cells only. Score comparisons in the component "
        "heatmaps are filtered to `all_placed == 1` rows; any ablation "
        "that regressed placement rate is listed separately under "
        "**Placement regressions**.")
    sections.append("")

    for rot in sorted(df["rotate_all"].unique()):
        sections.append("## rotate_all = {}".format(rot))
        sections.append("")
        sections.append("### Config comparison")
        sections.append("")
        sections.append(comparison_table(df, rot, baseline_name, is_v2))
        sections.append("")

        pr = placement_regression_section(df, rot, baseline_name)
        if pr:
            sections.append("### Placement regressions")
            sections.append("")
            sections.append(pr)
            sections.append("")

        sections.append("### Per-component delta heatmaps (vs baseline)")
        sections.append("")
        sections.append(
            "Filtered to rows where `all_placed == 1`. Any mixed-placement "
            "cells appear in the Placement regressions section above.")
        sections.append("")
        # When grouping_power_quarters varies across configs, the raw
        # grouping_bonus column scales differently per config so cross-config
        # heatmaps are misleading. Filter the grouping_bonus heatmap to
        # power-matched configs and emit a separate grouping_borders_raw
        # heatmap (power-independent) for the cross-power view.
        gb_df = df
        power_varies = False
        if "scoring_grouping_power_quarters" in df.columns:
            base_rows_for_power = df[df["config_name"] == baseline_name]
            if not base_rows_for_power.empty:
                base_gpq_for_filter = base_rows_for_power["scoring_grouping_power_quarters"].iloc[0]
                gb_df = df[df["scoring_grouping_power_quarters"] == base_gpq_for_filter]
                power_varies = (df["scoring_grouping_power_quarters"].nunique() > 1)

        for column, title, fmt, src_df in [
            ("ler_area",       "LER area",                                fmt_delta, df),
            ("stranded_cells", "Stranded cells (lower is better)",        fmt_delta, df),
            ("concentration",  "Concentration (HHI; delta in thousandths)", fmt_conc_delta, df),
            ("grouping_bonus", "Grouping bonus (higher is better; power-matched configs only)" if power_varies else "Grouping bonus (higher is better)", fmt_delta, gb_df),
            ("num_rotated",    "Rotated items (lower is better)",         fmt_delta, df),
        ]:
            sections.append(component_delta_heatmap(
                src_df, rot, baseline_name, column, title, fmt=fmt))
            sections.append("")
        if power_varies and "grouping_borders_raw" in df.columns:
            sections.append(component_delta_heatmap(
                df, rot, baseline_name, "grouping_borders_raw",
                "Grouping borders raw (power-independent; cross-power comparison)",
                fmt=fmt_delta))
            sections.append("")
        # Composite score heatmap restricted to configs matching baseline's
        # scoring_grouping_weight -- weight=2 ablations would be trivially
        # offset by ~50-200 points from the formula alone, which would make
        # the composite heatmap misleading across weights.
        composite_df = df
        if "scoring_grouping_weight" in df.columns:
            base_rows = df[df["config_name"] == baseline_name]
            if not base_rows.empty:
                base_gw = base_rows["scoring_grouping_weight"].iloc[0]
                composite_df = df[df["scoring_grouping_weight"] == base_gw]
        sections.append(component_delta_heatmap(
            composite_df, rot, baseline_name,
            "score", "Composite score (reference; weight-matched to baseline)"))
        sections.append("")

        sa = scoring_weight_ablation(df, rot, baseline_name)
        if sa:
            sections.append("### Scoring weight ablation")
            sections.append("")
            sections.append(sa)
            sections.append("")

        rd = repair_diagnostics_section(df, rot, baseline_name)
        if rd:
            sections.append("### Repair move diagnostics")
            sections.append("")
            sections.append(rd)
            sections.append("")

        sections.append("### Basin rate")
        sections.append("")
        sections.append(basin_section(df, rot, baseline_name, is_v2))
        sections.append("")

        if is_v2:
            rs = refinement_section(df, rot, baseline_name)
            if rs:
                sections.append("### Refinement contribution")
                sections.append("")
                sections.append(rs)
                sections.append("")

        sections.append("### Baseline variance floor (unimodal cells)")
        sections.append("")
        sections.append(variance_floor(df, rot, baseline_name))
        sections.append("")
    return "\n".join(sections)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+", help="CSV file(s) or glob patterns")
    ap.add_argument("--baseline", default="baseline", help="Baseline config name")
    ap.add_argument("--out", default=None, help="Output path for report.md")
    args = ap.parse_args()

    paths = []
    for p in args.csv:
        expanded = glob.glob(p)
        if expanded:
            paths.extend(expanded)
        elif os.path.exists(p):
            paths.append(p)
        else:
            print("WARN: no match for {}".format(p), file=sys.stderr)
    if not paths:
        print("ERROR: no input files", file=sys.stderr)
        return 1

    df, schema_version = load_results(paths)
    if df is None or df.empty:
        print("ERROR: no data loaded", file=sys.stderr)
        return 1

    text = report(df, args.baseline, schema_version)

    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("Wrote {}".format(args.out))
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
