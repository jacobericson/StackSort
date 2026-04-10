#pragma once

// CatalogDump -- dev-only debug dump of unique item templates to a TSV file
// next to the plugin DLL. Entire body compiles to no-ops in prod builds
// (when STACKSORT_VERBOSE is not defined), so callers can call Init/Scan
// unconditionally if they guard the include.
//
// Output: <dll_dir>\stacksort_catalog.tsv
// Format: name\tw\th\tcanRotate  (header row on first creation)
// Dedup:  by GameData::name string; preseeded from any existing file so the
//         catalog accumulates across sessions.

class InventoryGUI;

namespace CatalogDump
{
    void Init();
    void Scan(InventoryGUI* gui);
}
