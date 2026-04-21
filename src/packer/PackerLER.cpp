#include "Packer.h"
#include "PackerProfile.h"

#include <algorithm>
#include <cstring>

namespace Packer
{

namespace Ler
{

void ComputeLERCtx(PackContext& ctx, const unsigned char* grid, int gridW, int gridH, int& outArea, int& outWidth,
                   int& outHeight, int& outX, int& outY)
{
    outArea   = 0;
    outWidth  = 0;
    outHeight = 0;
    outX      = 0;
    outY      = 0;

    if (gridW <= 0 || gridH <= 0) return;

    ctx.ler.heights.assign(gridW, 0);
    int* heights    = &ctx.ler.heights[0];
    LEREntry* stack = &ctx.ler.lerStack[0];

    for (int y = 0; y < gridH; ++y)
    {
        SUBPHASE_BEGIN(hist);
        int maxHeight = 0;
        int rowOffset = y * gridW;
        for (int x = 0; x < gridW; ++x)
        {
            heights[x] = (grid[rowOffset + x] == 0) ? heights[x] + 1 : 0;
            maxHeight  = std::max(maxHeight, heights[x]);
        }
        SUBPHASE_END(hist, ctx.prof.cyclesLerHistogram);

        // Any rectangle with its bottom at row y is bounded above by
        // gridW * maxHeight. If that's already <= the best so far, the
        // monotonic-stack sweep can't improve outArea; skip it.
        if (gridW * maxHeight <= outArea) continue;

        SUBPHASE_BEGIN(stk);
        int stackTop = 0;

        for (int x = 0; x <= gridW; ++x)
        {
            int curHeight = (x < gridW) ? heights[x] : 0;
            int si        = x;

            while (stackTop > 0 && stack[stackTop - 1].height > curHeight)
            {
                LEREntry top = stack[--stackTop];

                int rectW = x - top.startIdx;
                int area  = rectW * top.height;

                if (area > outArea)
                {
                    outArea   = area;
                    outWidth  = rectW;
                    outHeight = top.height;
                    outX      = top.startIdx;
                    outY      = y - top.height + 1;
                }

                si = top.startIdx;
            }

            stack[stackTop].startIdx = si;
            stack[stackTop].height   = curHeight;
            ++stackTop;
        }
        SUBPHASE_END(stk, ctx.prof.cyclesLerStack);
    }
}

// Fused: single flood-fill pass computes both HHI concentration AND
// stranded-cell count. Replaces the previous ComputeConcentrationCtx +
// ComputeStrandedCells pair (both scanned the same empty-cell set).
//
// Per region, we track area (for HHI), interior-cell count (for stranded),
// and whether any cell falls inside the LER rect (for LER-connectivity).
// Post-pass: HHI uses the legacy per-region (share²) summation order;
// stranded sums interior counts from regions NOT connected to the LER.
// When lerW==0 or lerH==0 (no LER found), no region is LER-connected and
// ALL interior empty cells count as stranded — matches legacy semantics.

double ComputeConcentrationAndStrandedCtx(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW,
                                          int lerH, int& outStrandedCells)
{
    outStrandedCells = 0;

    int totalCells = gridW * gridH;
    if (totalCells == 0) return 0.0;

    ctx.visited.resize(totalCells);
    memset(&ctx.visited[0], 0, totalCells);
    // External clear() can drop size; reclaim for raw-pointer indexing.
    if ((int)ctx.ler.floodStack.size() < totalCells) ctx.ler.floodStack.resize(totalCells);
    ctx.ler.regionAreas.clear();
    ctx.ler.regionInterior.clear();
    ctx.ler.regionHasLer.clear();
    int* flood                   = &ctx.ler.floodStack[0];
    unsigned char* vis           = &ctx.visited[0];
    const unsigned char* gridPtr = &ctx.grid[0];
    int totalFree                = 0;

    int lerXEnd = lerX + lerW;
    int lerYEnd = lerY + lerH;

    for (int i = 0; i < totalCells; ++i)
    {
        if (gridPtr[i] != 0 || vis[i]) continue;

        int floodTop         = 0;
        flood[floodTop++]    = i;
        vis[i]               = 1;
        int area             = 0;
        int interior         = 0;
        unsigned char hasLer = 0;

        while (floodTop > 0)
        {
            int ci = flood[--floodTop];
            ++area;
            int cx = ci % gridW;
            int cy = ci / gridW;

            // Interior filter: x∈[1,W-2], y∈[1,H-2]. Border cells never
            // contribute to stranded count, matching legacy.
            if (cx >= 1 && cx <= gridW - 2 && cy >= 1 && cy <= gridH - 2) ++interior;

            // LER-rect membership: if any cell of this region is within
            // the LER rect, the whole region is LER-connected by definition
            // (the LER rect is empty by construction, so its cells are all
            // in one connected component).
            if (cx >= lerX && cx < lerXEnd && cy >= lerY && cy < lerYEnd) hasLer = 1;

            if (cx > 0 && !vis[ci - 1] && !gridPtr[ci - 1])
            {
                vis[ci - 1]       = 1;
                flood[floodTop++] = ci - 1;
            }
            if (cx < gridW - 1 && !vis[ci + 1] && !gridPtr[ci + 1])
            {
                vis[ci + 1]       = 1;
                flood[floodTop++] = ci + 1;
            }
            if (cy > 0 && !vis[ci - gridW] && !gridPtr[ci - gridW])
            {
                vis[ci - gridW]   = 1;
                flood[floodTop++] = ci - gridW;
            }
            if (cy < gridH - 1 && !vis[ci + gridW] && !gridPtr[ci + gridW])
            {
                vis[ci + gridW]   = 1;
                flood[floodTop++] = ci + gridW;
            }
        }

        ctx.ler.regionAreas.push_back(area);
        ctx.ler.regionInterior.push_back(interior);
        ctx.ler.regionHasLer.push_back(hasLer);
        totalFree += area;
    }

    // Stranded count: sum interior counts from regions NOT connected to
    // the LER. When no region has hasLer==1 (lerW==0 / lerH==0 / full grid),
    // every region contributes, matching legacy's "all interior cells
    // stranded when no LER" behavior.
    int stranded = 0;
    for (size_t r = 0; r < ctx.ler.regionAreas.size(); ++r)
    {
        if (!ctx.ler.regionHasLer[r]) stranded += ctx.ler.regionInterior[r];
    }
    outStrandedCells = stranded;

    if (totalFree == 0) return 0.0;

    // HHI: preserve legacy per-region summation order bit-for-bit. Changing
    // to sumSq/(totalFree*totalFree) would drift by ~1 ULP, which then gets
    // amplified by ComputeScore's groupingBonus*concentration*groupingWeight
    // multiply chain into visible long long score differences.
    double hhi = 0.0;
    for (size_t r = 0; r < ctx.ler.regionAreas.size(); ++r)
    {
        double share = (double)ctx.ler.regionAreas[r] / (double)totalFree;
        hhi += share * share;
    }
    return hhi;
}

void ComputeLER(const std::vector<unsigned char>& grid, int gridW, int gridH, int& outArea, int& outWidth,
                int& outHeight, int& outX, int& outY)
{
    PackContext ctx;
    ctx.ler.heights.resize(gridW);
    ctx.ler.lerStack.resize(gridW + 1);
    ComputeLERCtx(ctx, &grid[0], gridW, gridH, outArea, outWidth, outHeight, outX, outY);
}

} // namespace Ler

namespace Grid
{

// Shared helper used by the repair_move branch in PackAnnealedH. Assumes
// ctx.grid is already populated. Resets ctx.visited and flood-fills
// 4-connected empty cells starting from all empty cells inside the LER
// rectangle. On return, ctx.visited[i] == 1 iff cell i is empty AND
// reachable from the LER through empty cells.
void FloodFillFromLer(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW, int lerH,
                      const unsigned char* extGrid)
{
    int totalCells = gridW * gridH;
    if (totalCells == 0) return;

    ctx.visited.resize(totalCells);
    memset(&ctx.visited[0], 0, totalCells);
    // External clear() can drop size; reclaim for raw-pointer indexing.
    if ((int)ctx.ler.floodStack.size() < totalCells) ctx.ler.floodStack.resize(totalCells);
    int* flood                   = &ctx.ler.floodStack[0];
    unsigned char* vis           = &ctx.visited[0];
    const unsigned char* gridPtr = extGrid ? extGrid : &ctx.grid[0];
    int floodTop                 = 0;

    for (int y = lerY; y < lerY + lerH; ++y)
    {
        for (int x = lerX; x < lerX + lerW; ++x)
        {
            int idx = y * gridW + x;
            if (gridPtr[idx] == 0 && !vis[idx])
            {
                vis[idx]          = 1;
                flood[floodTop++] = idx;
            }
        }
    }

    while (floodTop > 0)
    {
        int ci = flood[--floodTop];
        int cx = ci % gridW;
        int cy = ci / gridW;

        if (cx > 0 && !vis[ci - 1] && !gridPtr[ci - 1])
        {
            vis[ci - 1]       = 1;
            flood[floodTop++] = ci - 1;
        }
        if (cx < gridW - 1 && !vis[ci + 1] && !gridPtr[ci + 1])
        {
            vis[ci + 1]       = 1;
            flood[floodTop++] = ci + 1;
        }
        if (cy > 0 && !vis[ci - gridW] && !gridPtr[ci - gridW])
        {
            vis[ci - gridW]   = 1;
            flood[floodTop++] = ci - gridW;
        }
        if (cy < gridH - 1 && !vis[ci + gridW] && !gridPtr[ci + gridW])
        {
            vis[ci + gridW]   = 1;
            flood[floodTop++] = ci + gridW;
        }
    }
}

} // namespace Grid

} // namespace Packer
