#include "Packer.h"

#include <algorithm>
#include <cstring>

// Named functor for VS2010 (no lambdas).

struct ItemSortCompare
{
    bool operator()(const Packer::Item& a, const Packer::Item& b) const
    {
        int maxA = (a.w > a.h) ? a.w : a.h;
        int maxB = (b.w > b.h) ? b.w : b.h;
        if (maxA != maxB) return maxA > maxB;
        int areaA = a.w * a.h;
        int areaB = b.w * b.h;
        if (areaA != areaB) return areaA > areaB;
        // Tertiary: group same-template items together in packing order.
        // Items placed consecutively tend to end up adjacent on the grid.
        return a.exactId < b.exactId;
    }
};

void Packer::SortItems(std::vector<Item>& items)
{
    std::stable_sort(items.begin(), items.end(), ItemSortCompare());
}

void Packer::InitPackContext(PackContext& ctx, int gridW, int gridH, int numItems)
{
    int totalCells = gridW * gridH;
    ctx.freeRects.reserve((size_t)numItems * 4u);
    ctx.newRects.reserve((size_t)numItems * 4u);
    ctx.placements.reserve(numItems);
    ctx.grid.resize(totalCells);
    ctx.visited.resize(totalCells);
    // A reused ctx may carry 1s from a prior flood fill; reset for the
    // visited-guarded concentration/stranded scans.
    memset(&ctx.visited[0], 0, totalCells);
    ctx.heights.resize(gridW);
    ctx.lerStack.resize(gridW + 1);
    ctx.floodStack.resize(totalCells);
    ctx.regionAreas.reserve(16);
    ctx.regionInterior.reserve(16);
    ctx.regionHasLer.reserve(16);
    ctx.wasteRects.reserve((size_t)numItems * 2u);
    ctx.placementIdGrid.resize(totalCells);
    ctx.bssfPl.reserve(numItems);
    ctx.seedPl.reserve(numItems);
    ctx.bestPl.reserve(numItems);
    ctx.skylineWasteCoef           = DEFAULT_SKYLINE_WASTE_COEF;
    ctx.tierWeightExact            = DEFAULT_TIER_WEIGHT_EXACT;
    ctx.tierWeightCustom           = DEFAULT_TIER_WEIGHT_CUSTOM;
    ctx.tierWeightType             = DEFAULT_TIER_WEIGHT_TYPE;
    ctx.tierWeightFunction         = DEFAULT_TIER_WEIGHT_FUNCTION;
    ctx.tierWeightFlags            = DEFAULT_TIER_WEIGHT_FLAGS;
    ctx.funcSimFoodFoodRestricted  = DEFAULT_FUNC_SIM_FOOD_FOOD_RESTRICTED;
    ctx.funcSimFirstaidRobotrepair = DEFAULT_FUNC_SIM_FIRSTAID_ROBOTREPAIR;
    ctx.softGroupingPct            = DEFAULT_SOFT_GROUPING_PCT;
    // Reserve capacity up front so BuildPairWeightMatrix's assign() doesn't
    // reallocate inside the per-pack critical path.
    ctx.pairWeightMatrix.reserve((size_t)numItems * (size_t)numItems);
    ctx.pairWeightMatrixN = 0;
    ctx.gridCacheCount    = 0;
    ctx.gridCacheHead     = 0;
    ctx.skylineSnapBoundaries.reserve((size_t)numItems + 1);
    ctx.skylineSnapWaste.reserve((size_t)numItems * 30);
    ctx.skylineSnapSkyline.reserve((size_t)numItems * 20);
    ctx.skylineSnapGridDelta.reserve((size_t)totalCells);
    ctx.skylineSnapN     = 0;
    ctx.skylineSnapValid = false;
    memset(ctx.typeCount, 0, sizeof(ctx.typeCount));
#ifdef STACKSORT_PROFILE
    ctx.profSkylinePrefixK         = 0;
    ctx.profSkylinePrefixCycles    = 0;
    ctx.profCyclesSkylineWasteMap  = 0;
    ctx.profCyclesSkylineCandidate = 0;
    ctx.profCyclesSkylineAdjacency = 0;
    ctx.profCyclesSkylineCommit    = 0;
    ctx.profCyclesLerHistogram     = 0;
    ctx.profCyclesLerStack         = 0;
#endif
}

void Packer::BuildOccupancyGrid(PackContext& ctx, int gridW, int gridH)
{
    int totalCells = gridW * gridH;
    ctx.grid.resize(totalCells);
    memset(&ctx.grid[0], 0, totalCells);

    for (size_t i = 0; i < ctx.placements.size(); ++i)
    {
        const Placement& p = ctx.placements[i];
        for (int dy = 0; dy < p.h; ++dy)
            memset(&ctx.grid[(p.y + dy) * gridW + p.x], 1, p.w);
    }
}

int Packer::CountRotated(const std::vector<Placement>& placements)
{
    int count = 0;
    for (size_t i = 0; i < placements.size(); ++i)
    {
        if (placements[i].rotated) ++count;
    }
    return count;
}

Packer::Result Packer::Pack(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
                            const volatile long* abortFlag, PackContext* reuseCtx)
{
    if (dim == TARGET_H) return PackH(gridW, gridH, items, target, abortFlag, reuseCtx);

    // W-mode: transpose, run H-mode, transpose back.
    std::vector<Item> itemsT = items;
    for (size_t i = 0; i < itemsT.size(); ++i)
        std::swap(itemsT[i].w, itemsT[i].h);

    // NOLINTNEXTLINE(readability-suspicious-call-argument) — W-mode transpose: itemsT has w/h swapped and placements are swapped back after the call.
    Result r = PackH(gridH, gridW, itemsT, target, abortFlag, reuseCtx);

    for (size_t i = 0; i < r.placements.size(); ++i)
    {
        std::swap(r.placements[i].x, r.placements[i].y);
        std::swap(r.placements[i].w, r.placements[i].h);
    }
    std::swap(r.lerX, r.lerY);
    std::swap(r.lerWidth, r.lerHeight);

    return r;
}

Packer::Result Packer::PackH(int gridW, int gridH, const std::vector<Item>& items, int target,
                             const volatile long* abortFlag, PackContext* reuseCtx)
{
    Result result;
    result.lerArea       = 0;
    result.lerWidth      = 0;
    result.lerHeight     = 0;
    result.lerX          = 0;
    result.lerY          = 0;
    result.score         = 0;
    result.concentration = 0.0;
    result.strandedCells = 0;
    result.groupingBonus = 0;
    result.allPlaced     = false;

    std::vector<Item> sorted = items;
    SortItems(sorted);

    PackContext localCtx;
    PackContext& ctx = reuseCtx ? *reuseCtx : localCtx;
    InitPackContext(ctx, gridW, gridH, (int)items.size());

    MaxRectsPack(ctx, gridW, gridH, sorted, target, abortFlag, 0, 0, 0);
    if (abortFlag && *abortFlag != 0) return result;
    std::vector<Placement> bssfPl = ctx.placements;

    MaxRectsPack(ctx, gridW, gridH, sorted, target, abortFlag, 0, 0, 1);
    if (abortFlag && *abortFlag != 0)
    {
        result.placements = bssfPl;
        return result;
    }

    if (ctx.placements.size() < bssfPl.size()) ctx.placements = bssfPl;
    result.placements = ctx.placements;

    result.allPlaced = (result.placements.size() == items.size());

    BuildOccupancyGrid(ctx, gridW, gridH);
    ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, result.lerArea, result.lerWidth, result.lerHeight, result.lerX,
                  result.lerY);
    result.concentration = ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, result.lerX, result.lerY,
                                                              result.lerWidth, result.lerHeight, result.strandedCells);

    // Score — PackH has no SearchParams plumbing, so use compile-time defaults.
    int numRot           = CountRotated(result.placements);
    long long grouping   = ComputeGroupingBonus(result.placements, items, ctx, Packer::DEFAULT_GROUPING_POWER_QUARTERS);
    result.groupingBonus = grouping;
    result.score =
        ComputeScore(result.placements.size(), result.lerArea, result.lerHeight, result.concentration, target, numRot,
                     grouping, result.strandedCells, Packer::DEFAULT_GROUPING_WEIGHT, Packer::DEFAULT_FRAG_WEIGHT);

    return result;
}
