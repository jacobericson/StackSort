#pragma once

#include <string>

#include "Packer.h"

struct HarnessConfig
{
    std::string name;
    Packer::SearchParams params;

    // Refinement overrides. -1 = use RefineCriteria.h defaults.
    int refineRestarts;
    int refineIters;
    int refineLahcHist;
    int refinePlateau;
    int refineAlways;        // 1 = skip NeedsRefinement gate, always refine
    int refineStrandedMax;   // override stranded_cells threshold (-1 = default 0)
    int refineItemThreshold; // override item count threshold (-1 = default REFINE_ITEM_THRESHOLD)

    HarnessConfig()
        : refineRestarts(-1), refineIters(-1), refineLahcHist(-1), refinePlateau(-1), refineAlways(-1),
          refineStrandedMax(-1), refineItemThreshold(-1)
    {
    }
};

// Parse an INI config file. `base` is the starting SearchParams; fields
// present in the file override, fields absent are inherited from base.
// Loader is expected to load baseline.ini first with SearchParams::defaults()
// and use the resulting params as `base` for subsequent ablation configs.
bool ParseConfigFile(const std::string& filePath, const Packer::SearchParams& base, HarnessConfig& out,
                     std::string& errMsg);
