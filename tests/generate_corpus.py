#!/usr/bin/env python3
import os
import re
import csv
import sys
from collections import Counter


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
CATALOG_PATH = os.path.join(REPO_DIR, "stacksort_catalog.tsv")
CORPUS_DIR = os.path.join(SCRIPT_DIR, "corpus")


TYPE_NAMES = {
    0: "blades",
    1: "bolts",
    2: "backpacks",
    3: "plate_armor",
    4: "cloth_armor",
    5: "pants",
    6: "medkits",
    7: "food",
    8: "materials",
    9: "hats",
    10: "misc",
}

# Regex classification. First match wins; unmatched items fall through to misc.
TYPE_PATTERNS = [
    (0, [r"katana", r"sabre", r"nodachi", r"ninja blade", r"^ranger$",
         r"tooth pick", r"spring bat", r"eagle's cross"]),
    (1, [r"^bolts"]),
    (2, [r"backpack", r"scavenger's basket"]),
    (3, [r"plate jacket", r"legplates", r"chainmail sheets"]),
    (4, [r"rag shirt", r"cloth shirt", r"leather turtleneck",
         r"leather hive vest", r"leather vest", r"mercenary leather",
         r"trader's leathers", r"armoured rags"]),
    (5, [r"clothpants", r"cargopants", r"halfpants", r"worn-out shorts",
         r"wooden sandals"]),
    (6, [r"first aid", r"repair kit"]),
    (7, [r"raw meat", r"foodcube", r"riceweed", r"greenfruit", r"wheatstraw",
         r"^water$", r"water jug"]),
    (8, [r"^tools$", r"electrical components", r"crossbow parts", r"cotton",
         r"^fabrics$", r"copper alloy", r"steel bars", r"armour plating",
         r"^gears$", r"^fuel$", r"medical supplies", r"engineering research",
         r"^book$", r"animal skin", r"hacksaw", r"scout leg", r"stealth leg"]),
    (9, [r"fog mask", r"straw hat", r"^cap$"]),
]


def assign_type(name):
    lname = name.lower()
    for type_id, patterns in TYPE_PATTERNS:
        for p in patterns:
            if re.search(p, lname):
                return type_id
    return 10  # misc catchall


def load_catalog(path):
    items = []
    with open(path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            items.append({
                "name": row["name"],
                "w": int(row["w"]),
                "h": int(row["h"]),
                "canRotate": int(row["canRotate"]),
                "type_id": assign_type(row["name"]),
            })
    return items


def find_item(catalog, name_substr):
    """Case-insensitive. Raises KeyError on no match."""
    low = name_substr.lower()
    for it in catalog:
        if low in it["name"].lower():
            return it
    raise KeyError("No catalog item matching '%s'" % name_substr)


def build_items(catalog, specs):
    """specs: list of (count, name_substr). Returns flat item list."""
    out = []
    for count, substr in specs:
        it = find_item(catalog, substr)
        for _ in range(count):
            out.append(dict(it))
    return out


def write_instance(name, grid_w, grid_h, items, extra_comments=None):
    os.makedirs(CORPUS_DIR, exist_ok=True)
    path = os.path.join(CORPUS_DIR, name + ".txt")
    total_area = sum(it["w"] * it["h"] for it in items)
    density = total_area / float(grid_w * grid_h)
    types = sorted(set(it["type_id"] for it in items))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# {}\n".format(name))
        f.write("# {}x{} grid, {} items, {} types, density {:.2f}\n".format(
            grid_w, grid_h, len(items), len(types), density))
        if extra_comments:
            for line in extra_comments:
                f.write("# {}\n".format(line))
        f.write("grid {} {}\n".format(grid_w, grid_h))
        f.write("# w h type rotatable name\n")
        for it in items:
            safe = it["name"].replace("\t", " ").replace("\n", " ")
            f.write("{} {} {} {} {}\n".format(
                it["w"], it["h"], it["type_id"], it["canRotate"], safe))
    return (path, len(items), density)


def recipe_sparse_backpack_small(catalog):
    items = build_items(catalog, [
        (1, "Guardless Katana"),      # 7x1 blade
        (1, "Raw Meat"),               # 2x2 food
        (1, "Foodcube"),               # 3x2 food
        (2, "Basic First Aid Kit"),   # 2x1 med x2
        (2, "Lantern"),                # 1x2 misc x2
        (1, "Bolts [Toothpicks]"),    # 3x1 bolts
    ])
    # Total 28 / 64 = 44%.
    return write_instance("sparse_backpack_small", 8, 8, items)


def recipe_sparse_backpack_large(catalog):
    items = build_items(catalog, [
        (2, "Guardless Katana"),       # 14
        (1, "Ranger"),                 # 14
        (4, "Raw Meat"),                # 16
        (4, "Foodcube"),                # 24
        (3, "Riceweed"),                # 18
        (3, "Bolts [Longs]"),           # 15
        (2, "Skeleton Repair Kit"),    # 12
        (2, "Tools"),                   # 12
        (2, "Crossbow Parts"),          # 12
        (1, "Straw Hat"),               # 9
        (3, "Lantern"),                 # 6
        (1, "Book"),                    # 4
        (2, "Basic First Aid Kit"),    # 4
    ])
    # Total 160 / 400 = 40%. Animal pack loaded for a journey.
    return write_instance("sparse_backpack_large", 20, 20, items)


def recipe_sparse_uniform_small(catalog):
    items = build_items(catalog, [
        (2, "Raw Meat"),                # 8
        (1, "Foodcube"),                # 6
        (1, "Riceweed"),                # 6
    ])
    return write_instance("sparse_uniform_small", 8, 8, items, [
        "Food-only instance. Stresses grouping on an easy density."
    ])


def recipe_typical_body_mixed(catalog):
    items = build_items(catalog, [
        (2, "Foodcube"),                # 12
        (2, "Raw Meat"),                # 8
        (1, "Tools"),                   # 6
        (2, "Lantern"),                 # 4
        (1, "Bolts [Toothpicks]"),     # 3
        (2, "Basic First Aid Kit"),    # 4
        (1, "Book"),                    # 4
        (1, "Straw Hat"),               # 9
    ])
    # Total 50 / 80 = 63%.
    return write_instance("typical_body_mixed", 8, 10, items)


def recipe_typical_backpack_mixed(catalog):
    items = build_items(catalog, [
        (1, "Guardless Katana"),        # 7
        (2, "Raw Meat"),                # 8
        (2, "Foodcube"),                # 12
        (2, "Bolts [Longs]"),           # 10
        (2, "Bolts [Toothpicks]"),     # 6
        (1, "Skeleton Repair Kit"),    # 6
        (2, "Basic First Aid Kit"),    # 4
        (1, "Tools"),                   # 6
        (2, "Lantern"),                 # 4
        (1, "Straw Hat"),               # 9
        (1, "Crossbow Parts"),          # 6
        (1, "Book"),                    # 4
        (1, "Engineering Research"),   # 4
    ])
    return write_instance("typical_backpack_mixed", 10, 14, items)


def recipe_typical_body_weapons(catalog):
    items = build_items(catalog, [
        (1, "Guardless Katana"),        # 7
        (1, "Ninja Blade"),             # 7
        (1, "Ranger"),                  # 14
        (2, "Bolts [Longs]"),           # 10
        (2, "Bolts [Toothpicks]"),     # 6
        (2, "Basic First Aid Kit"),    # 4
    ])
    # Total 48 / 80 = 60%.
    return write_instance("typical_body_weapons", 8, 10, items, [
        "Blade-heavy body inventory. Large items, few types."
    ])


def recipe_typical_backpack_trader(catalog):
    items = build_items(catalog, [
        (5, "Raw Meat"),                # 20
        (4, "Foodcube"),                # 24
        (4, "Riceweed"),                # 24
        (2, "Tools"),                   # 12
        (2, "Crossbow Parts"),          # 12
        (2, "Bolts [Longs]"),           # 10
        (2, "Bolts [Toothpicks]"),     # 6
        (2, "Bolts [Regulars]"),       # 8
        (3, "Basic First Aid Kit"),    # 6
        (2, "Skeleton Repair Kit"),    # 12
        (2, "Fuel"),                    # 18
    ])
    # Total 152 / 240 = 63%.
    return write_instance("typical_backpack_trader", 12, 20, items, [
        "Trader hauler: high count, low type variety, dense packing."
    ])


def recipe_typical_body_survivor(catalog):
    items = build_items(catalog, [
        (2, "Basic First Aid Kit"),    # 4
        (2, "Raw Meat"),                # 8
        (2, "Foodcube"),                # 12
        (1, "Bolts [Toothpicks]"),     # 3
        (2, "Lantern"),                 # 4
        (1, "Book"),                    # 4
        (1, "Riceweed"),                # 6
        (1, "Bolts [Regulars]"),       # 4
    ])
    # Total 45 / 80 = 56%.
    return write_instance("typical_body_survivor", 8, 10, items, [
        "High variety, small grid. Rotation makes real difference here."
    ])


def recipe_tight_body_small(catalog):
    items = build_items(catalog, [
        (2, "Guardless Katana"),       # 14
        (2, "Foodcube"),                # 12
        (3, "Raw Meat"),                # 12
        (1, "Tools"),                   # 6
        (2, "Lantern"),                 # 4
        (2, "Bolts [Toothpicks]"),     # 6
        (1, "Basic First Aid Kit"),    # 2
        (1, "Bolts [Longs]"),           # 5
        (1, "Straw Hat"),               # 9
    ])
    # Total area: 70 / 80 cells = 88%.
    return write_instance("tight_body_small", 8, 10, items, [
        "Barely fits. All-placed rate matters more than LER quality."
    ])


def recipe_tight_backpack_stuffed(catalog):
    items = build_items(catalog, [
        (2, "Guardless Katana"),        # 14
        (3, "Foodcube"),                # 18
        (3, "Raw Meat"),                # 12
        (3, "Skeleton Repair Kit"),    # 18
        (3, "Bolts [Longs]"),           # 15
        (3, "Bolts [Toothpicks]"),     # 9
        (1, "Tools"),                   # 6
        (1, "Crossbow Parts"),          # 6
        (2, "Basic First Aid Kit"),    # 4
        (2, "Lantern"),                 # 4
        (2, "Fuel"),                    # 18
    ])
    # Total 124 / 140 = 89%.
    return write_instance("tight_backpack_stuffed", 10, 14, items, [
        "Grinder pack: nearly full, tight packing."
    ])


def recipe_tight_uniform_food(catalog):
    items = build_items(catalog, [
        (4, "Raw Meat"),               # 16
        (4, "Foodcube"),               # 24
        (2, "Riceweed"),               # 12
        (1, "Greenfruit"),             # 9
    ])
    # Total 61 / 64 = 95% — tight.
    return write_instance("tight_uniform_food", 8, 8, items, [
        "Food hoarder: LAHC threads needles to fit everything."
    ])


def recipe_diverse_small_grid(catalog):
    items = build_items(catalog, [
        (1, "Guardless Katana"),        # 7 tall strip
        (1, "Straw Hat"),               # 9 big square
        (1, "Leather Turtleneck"),      # 8 wide rect
        (1, "Foodcube"),                # 6
        (2, "Raw Meat"),                # 8
        (1, "Basic First Aid Kit"),    # 2
        (2, "Lantern"),                 # 4
        (1, "Bolts [Toothpicks]"),     # 3
        (1, "Book"),                    # 4
    ])
    # Total 51 / 80 = 64%.
    return write_instance("diverse_small_grid", 8, 10, items, [
        "Wide size range on a small grid. 'Place big first' matters."
    ])


def recipe_diverse_large_grid(catalog):
    items = build_items(catalog, [
        (1, "Black Plate Jacket"),    # 4x6 = 24
        (1, "AI Core"),                # 4x4 = 16
        (1, "Water"),                  # 5x5 = 25
        (2, "Raw Meat"),               # 8
        (1, "Foodcube"),               # 6
        (2, "Lantern"),                # 4
        (1, "Guardless Katana"),       # 7
        (1, "Cap"),                    # 9
        (1, "Book"),                   # 4
        (2, "Bolts [Toothpicks]"),    # 6
    ])
    # Total ~109 / 240 = 45%.
    return write_instance("diverse_large_grid", 12, 20, items, [
        "Varied sizes on a big grid. One 5x5, one 4x6, small items."
    ])


def recipe_uniform_small(catalog):
    # "uniform" in the sense of small-item uniformity (smallest items Kenshi has).
    items = build_items(catalog, [
        (4, "Basic First Aid Kit"),    # 2x1 x4 = 8
        (4, "Lantern"),                 # 1x2 x4 = 8
        (4, "Bolts [Toothpicks]"),     # 3x1 x4 = 12
        (3, "Bolts [Regulars]"),       # 4x1 x3 = 12
    ])
    # Total 40 / 64 = 62%. All small/strip items.
    return write_instance("uniform_small", 8, 8, items, [
        "Smallest items available. Kenshi has no 1x1; 2x1/1x2/3x1 are the floor.",
        "Grouping + ordering dominate score here."
    ])


def recipe_strips_only(catalog):
    items = build_items(catalog, [
        (2, "Nodachi"),                 # 10x1 x2 = 20
        (1, "Eagle's Cross"),           # 10x2 = 20
        (2, "Steel Bars"),              # 6x1 x2 = 12
        (2, "Bolts [Longs]"),           # 5x1 x2 = 10
        (1, "Ninja Blade"),             # 7x1 = 7
        (1, "Guardless Katana"),        # 7x1 = 7
    ])
    # Total 76 / 140 = 54%. All strip items. Rotation-critical.
    return write_instance("strips_only", 10, 14, items, [
        "All long thin items. Rotation makes or breaks this instance.",
        "Run with --rotate 1 to see the difference."
    ])


def recipe_one_big_rest_small(catalog):
    items = build_items(catalog, [
        (1, "Water"),                   # 5x5 = 25 (the one big)
        (4, "Bolts [Toothpicks]"),     # 3x1 x4 = 12
        (4, "Lantern"),                 # 1x2 x4 = 8
        (3, "Basic First Aid Kit"),    # 2x1 x3 = 6
        (2, "Bolts [Regulars]"),       # 4x1 x2 = 8
        (2, "Raw Meat"),                # 2x2 x2 = 8
    ])
    # Total 67 / 140 = 48%. One big item, many fillers.
    return write_instance("one_big_rest_small", 10, 14, items, [
        "One 5x5 Water + many small fillers. Does 'big first' corner correctly?"
    ])


def recipe_l_shape_forcing(catalog):
    items = build_items(catalog, [
        (1, "AI Core"),                 # 4x4 = 16
        (1, "Leather Turtleneck"),     # 4x2 = 8
        (1, "Cap"),                     # 3x3 = 9
        (1, "Book"),                    # 2x2 = 4
        (1, "Foodcube"),                # 3x2 = 6
        (2, "Basic First Aid Kit"),    # 2x1 x2 = 4
        (1, "Lantern"),                 # 1x2 = 2
    ])
    # Total 49 / 64 = 77%. Dense, varied sizes that force L-shaped residual.
    return write_instance("l_shape_forcing", 8, 8, items, [
        "Dense mix where the free space can only form an L-shape.",
        "Tests ordering/rotation decisions on a fully-packed instance."
    ])


def recipe_stranded_heavy(catalog):
    # Dense blocky mix with odd-dimension items to force interior stranded
    # cells. The mix of 4x6, 4x5, 3x3, 2x7 doesn't tile cleanly into 20x20 --
    # 20 mod 4 = 0, 20 mod 6 = 2, 20 mod 3 = 2, 20 mod 7 = 6 -- forcing
    # leftover strips and isolated interior pockets. Primary workload for
    # exercising repair_move in the tuning harness.
    items = build_items(catalog, [
        (4, "Plate Jacket"),            # 4x6, plate_armor, 24 cells each -> 96
        (3, "Samurai Legplates"),       # 4x5, plate_armor, 20 cells each -> 60
        (2, "Scout Leg"),               # 2x7, misc, 14 cells each        -> 28
        (8, "Fuel"),                    # 3x3, materials, 9 cells each    -> 72
        (5, "Armour Plating"),          # 3x3, materials, 9 cells each    -> 45
        (4, "Foodcube"),                # 3x2, food, 6 cells each         -> 24
        (6, "Raw Meat"),                # 2x2, food, 4 cells each         -> 24
        (6, "Basic First Aid"),         # 2x1, medkits, 2 cells each      -> 12
    ])
    # Total 361 / 400 = 90%. 38 items, 5 types. Forcing mix.
    return write_instance("stranded_heavy", 20, 20, items, [
        "Dense blocky mix designed to force interior stranded cells.",
        "Workload for exercising repair_move in the tuning harness."
    ])


RECIPES = [
    recipe_sparse_backpack_small,
    recipe_sparse_backpack_large,
    recipe_sparse_uniform_small,
    recipe_typical_body_mixed,
    recipe_typical_backpack_mixed,
    recipe_typical_body_weapons,
    recipe_typical_backpack_trader,
    recipe_typical_body_survivor,
    recipe_tight_body_small,
    recipe_tight_backpack_stuffed,
    recipe_tight_uniform_food,
    recipe_diverse_small_grid,
    recipe_diverse_large_grid,
    recipe_uniform_small,
    recipe_strips_only,
    recipe_one_big_rest_small,
    recipe_l_shape_forcing,
    recipe_stranded_heavy,
]


def main():
    if not os.path.exists(CATALOG_PATH):
        print("ERROR: Catalog not found at {}".format(CATALOG_PATH))
        print("Run the in-game catalog dump (DEV build) first.")
        return 1

    catalog = load_catalog(CATALOG_PATH)
    print("Loaded {} items from {}".format(len(catalog), CATALOG_PATH))

    type_counts = Counter(it["type_id"] for it in catalog)
    for tid in sorted(type_counts.keys()):
        print("  Type {:2d} {:14s}: {} items".format(
            tid, TYPE_NAMES[tid], type_counts[tid]))

    print("")
    print("Generating recipes ->", CORPUS_DIR)
    written = 0
    for recipe in RECIPES:
        try:
            result = recipe(catalog)
            if result is None:
                continue
            path, n_items, density = result
            name = os.path.splitext(os.path.basename(path))[0]
            marker = "ok"
            if density > 1.0:
                marker = "OVERFULL"
            print("  {:28s}  {:3d} items  density {:.2f}  {}".format(
                name, n_items, density, marker))
            written += 1
        except Exception as e:
            print("  FAIL {}: {}".format(recipe.__name__, e))

    print("")
    print("Wrote {} instance files.".format(written))
    return 0


if __name__ == "__main__":
    sys.exit(main())
