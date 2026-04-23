#pragma once

#include "Packer.h"

#include <vector>

namespace Packer
{

namespace PostPack
{

// Rewrite placementIdGrid for a set of placement indices. The cells those
// placements covered in their *old* positions should already have been
// overwritten by their new positions elsewhere in the same batch — callers use
// this after mutating placements[idx].{x,y} for a layout-preserving move.
// Shared between StripShift and TileSwap; both post-pack moves need to
// resync placementIdGrid after moving placements without rebuilding from
// scratch.

static inline void StampPlacementCells(std::vector<unsigned char>& placementIdGrid,
                                       const std::vector<Placement>& placements, int gridW, const int* indices,
                                       int count)
{
    for (int k = 0; k < count; ++k)
    {
        int idx            = indices[k];
        const Placement& p = placements[idx];
        for (int dy = 0; dy < p.h; ++dy)
        {
            int rowOffset = (p.y + dy) * gridW;
            for (int dx = 0; dx < p.w; ++dx)
                placementIdGrid[rowOffset + (p.x + dx)] = (unsigned char)idx;
        }
    }
}

} // namespace PostPack

} // namespace Packer
