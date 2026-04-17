#!/usr/bin/env python3
"""Recipe-driven corpus generator for the StackSort tuning harness.

Reads tests/corpus_recipes/*.recipe and writes tests/corpus/*.txt in the format
tests/harness/Instance.cpp parses:

    grid W H
    w h type rotatable name

type_id assignment is per-item-name: the zero-based row index of the item in
stacksort_catalog.tsv. Catalog growth is append-only (CatalogDump dedups on
name), so existing type_ids stay stable across dev-build runs.

A complete mapping is written to tests/corpus/type_ids.tsv so analyzers can
resolve numeric type_ids back to catalog names.
"""

import argparse
import glob
import os
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
CATALOG_PATH = os.path.join(REPO_DIR, "stacksort_catalog.tsv")
RECIPES_DIR = os.path.join(SCRIPT_DIR, "corpus_recipes")
CORPUS_DIR = os.path.join(SCRIPT_DIR, "corpus")
TYPE_IDS_PATH = os.path.join(CORPUS_DIR, "type_ids.tsv")

EXPECTED_COLUMNS = [
    "name", "w", "h", "canRotate", "gameDataType", "itemFunction",
    "foodCrop", "tradeItem", "price", "sellPrice", "weight",
]


def load_catalog():
    """Return (items: {name -> dict}, type_ids: {name -> int})."""
    items = {}
    type_ids = {}
    with open(CATALOG_PATH, "r", encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        if header != EXPECTED_COLUMNS:
            sys.exit(
                "catalog schema mismatch.\n"
                "  expected: " + str(EXPECTED_COLUMNS) + "\n"
                "  got:      " + str(header)
            )
        for idx, raw in enumerate(f):
            line = raw.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != len(EXPECTED_COLUMNS):
                sys.exit(
                    "catalog line {}: expected {} fields, got {}"
                    .format(idx + 2, len(EXPECTED_COLUMNS), len(parts))
                )
            name = parts[0]
            items[name] = {
                "w": int(parts[1]),
                "h": int(parts[2]),
                "canRotate": int(parts[3]),
                "weight": float(parts[10]),
            }
            type_ids[name] = idx  # zero-based catalog row index
    return items, type_ids


def parse_recipe(path):
    """Return a spec dict, or raise ValueError on malformed input."""
    spec = {
        "path": path,
        "name": None,
        "grid_w": None,
        "grid_h": None,
        "density_target": None,
        "weight_budget": None,
        "items": [],  # list of (count, item_name)
    }
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            hash_pos = raw.find("#")
            body = (raw[:hash_pos] if hash_pos >= 0 else raw).strip()
            if not body:
                continue

            head, _, tail = body.partition(" ")
            tail = tail.strip()

            if head == "grid":
                dims = tail.split()
                if len(dims) != 2:
                    raise ValueError(
                        "{}:{}: `grid` expects 2 ints, got '{}'"
                        .format(path, lineno, tail)
                    )
                spec["grid_w"] = int(dims[0])
                spec["grid_h"] = int(dims[1])
            elif head == "name":
                spec["name"] = tail
            elif head == "density_target":
                spec["density_target"] = float(tail)
            elif head == "weight_budget":
                spec["weight_budget"] = float(tail)
            elif head.isdigit():
                count = int(head)
                if not tail:
                    raise ValueError(
                        "{}:{}: item line missing name after count"
                        .format(path, lineno)
                    )
                spec["items"].append((count, tail))
            else:
                raise ValueError(
                    "{}:{}: unrecognized directive '{}'"
                    .format(path, lineno, head)
                )

    if spec["grid_w"] is None or spec["grid_h"] is None:
        raise ValueError("{}: missing `grid W H`".format(path))
    if spec["name"] is None:
        spec["name"] = os.path.splitext(os.path.basename(path))[0]
    if not spec["items"]:
        raise ValueError("{}: no items".format(path))
    return spec


def analyze(spec, items):
    """Compute density + weight + distinct-type metrics. Returns None on missing items."""
    missing = [n for _, n in spec["items"] if n not in items]
    if missing:
        return None, missing
    total_cells = 0
    total_weight = 0.0
    distinct = set()
    for count, name in spec["items"]:
        entry = items[name]
        total_cells += count * entry["w"] * entry["h"]
        total_weight += count * entry["weight"]
        distinct.add(name)
    grid_area = spec["grid_w"] * spec["grid_h"]
    return {
        "cells": total_cells,
        "grid_area": grid_area,
        "density": total_cells / grid_area,
        "weight": total_weight,
        "distinct_types": len(distinct),
    }, []


def emit_instance(spec, items, type_ids, out_path):
    lines = []
    lines.append("# " + spec["name"])
    lines.append("# cells={} density={:.3f} weight={:.2f}".format(
        sum(c * items[n]["w"] * items[n]["h"] for c, n in spec["items"]),
        sum(c * items[n]["w"] * items[n]["h"] for c, n in spec["items"])
            / (spec["grid_w"] * spec["grid_h"]),
        sum(c * items[n]["weight"] for c, n in spec["items"]),
    ))
    lines.append("grid {} {}".format(spec["grid_w"], spec["grid_h"]))
    lines.append("# w h type_id rotatable name")
    for count, name in spec["items"]:
        entry = items[name]
        tid = type_ids[name]
        row = "{} {} {} {} {}".format(
            entry["w"], entry["h"], tid, entry["canRotate"], name
        )
        for _ in range(count):
            lines.append(row)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def write_type_ids(type_ids):
    os.makedirs(CORPUS_DIR, exist_ok=True)
    with open(TYPE_IDS_PATH, "w", encoding="utf-8") as f:
        f.write("type_id\tname\n")
        for name, tid in sorted(type_ids.items(), key=lambda kv: kv[1]):
            f.write("{}\t{}\n".format(tid, name))


def clear_old_corpus_txt():
    os.makedirs(CORPUS_DIR, exist_ok=True)
    for old in glob.glob(os.path.join(CORPUS_DIR, "*.txt")):
        os.remove(old)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--dry-run", action="store_true",
                    help="Analyze + report without writing corpus files")
    args = ap.parse_args()

    items, type_ids = load_catalog()
    print("Catalog: {} items loaded from {}".format(len(items), CATALOG_PATH))

    recipe_paths = sorted(glob.glob(os.path.join(RECIPES_DIR, "*.recipe")))
    if not recipe_paths:
        sys.exit("No recipes found at {}".format(RECIPES_DIR))

    if not args.dry_run:
        clear_old_corpus_txt()
        write_type_ids(type_ids)
        print("Wrote type_ids.tsv ({} entries)".format(len(type_ids)))

    print()
    header_fmt = "{:<34} {:<7} {:>5} {:>8} {:>8} {:>5}   {}"
    print(header_fmt.format(
        "name", "grid", "cells", "density", "weight", "types", "flags"
    ))
    print("-" * 90)

    errors = 0
    warnings = 0
    instances_written = 0

    for path in recipe_paths:
        try:
            spec = parse_recipe(path)
        except ValueError as e:
            print("ERROR parsing recipe:", e, file=sys.stderr)
            errors += 1
            continue

        stats, missing = analyze(spec, items)
        if missing:
            for m in missing:
                print("ERROR {}: item '{}' not in catalog"
                      .format(spec["name"], m), file=sys.stderr)
            errors += len(missing)
            continue

        flags = []
        if stats["density"] < 0.60:
            flags.append("LOW<60%")
            warnings += 1
        elif stats["density"] > 0.98:
            flags.append("HIGH>98%")
            warnings += 1
        if (spec["weight_budget"] is not None
                and stats["weight"] > spec["weight_budget"] * 1.2):
            flags.append("weight>1.2x budget({})"
                        .format(spec["weight_budget"]))
            warnings += 1

        grid = "{}x{}".format(spec["grid_w"], spec["grid_h"])
        print(header_fmt.format(
            spec["name"], grid,
            stats["cells"],
            "{:5.1f}%".format(stats["density"] * 100),
            "{:7.2f}".format(stats["weight"]),
            stats["distinct_types"],
            " ".join(flags) if flags else "",
        ))

        if not args.dry_run:
            out_path = os.path.join(CORPUS_DIR, spec["name"] + ".txt")
            emit_instance(spec, items, type_ids, out_path)
            instances_written += 1

    print()
    print("{} recipes processed, {} errors, {} warnings"
          .format(len(recipe_paths), errors, warnings))
    if not args.dry_run:
        print("{} instances written to {}".format(instances_written, CORPUS_DIR))
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
