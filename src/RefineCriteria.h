#pragma once

#include "Packer.h"

static const int REFINE_RESTARTS  = 20;
static const int REFINE_ITERS     = 8000;
static const int REFINE_LAHC_HIST = 200;
static const int REFINE_PLATEAU   = 2000;

// perpendicularGridExtent: axis perpendicular to `target`.
// H-mode target=height → gridW, checks lerWidth.
// W-mode target=width  → gridH, checks lerHeight.
static const int REFINE_ITEM_THRESHOLD = 20;

static inline bool NeedsRefinement(const Packer::Result& r,
                                   Packer::TargetDim dim,
                                   int perpendicularGridExtent,
                                   int target,
                                   int totalFreeCells,
                                   int numItems = 0,
                                   int strandedMax = 0,
                                   int itemThreshold = REFINE_ITEM_THRESHOLD)
{
    if (!r.allPlaced)
        return true;

    int perpendicularLerSide = (dim == Packer::TARGET_H) ? r.lerWidth
                                                          : r.lerHeight;
    // Don't flag for falling short of a structurally impossible target.
    bool strippable = totalFreeCells >= perpendicularGridExtent * target;
    if (strippable && perpendicularLerSide < perpendicularGridExtent)
        return true;

    if (r.concentration < 0.95)
        return true;

    if (r.strandedCells > strandedMax)
        return true;

    if (numItems > itemThreshold)
        return true;

    return false;
}
