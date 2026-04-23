#include "Packer.h"

#include <algorithm>
#include <cstring>

namespace Packer
{

namespace
{

unsigned long long splitmix64_next(unsigned long long* state)
{
    unsigned long long z = (*state += 0x9E3779B97F4A7C15ULL);
    z                    = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z                    = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Named functor for VS2010 (no lambdas).
struct ItemSortCompare
{
    bool operator()(const Item& a, const Item& b) const
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

} // namespace

namespace Geometry
{

int CountRotated(const std::vector<Placement>& placements)
{
    int count = 0;
    for (size_t i = 0; i < placements.size(); ++i)
    {
        if (placements[i].rotated) ++count;
    }
    return count;
}

} // namespace Geometry

namespace Grid
{

void BuildOccupancyGrid(PackContext& ctx, int gridW, int gridH)
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

} // namespace Grid

namespace Search
{

void SortItems(std::vector<Item>& items)
{
    std::stable_sort(items.begin(), items.end(), ItemSortCompare());
}

void InitPackContext(PackContext& ctx, int gridW, int gridH, int numItems)
{
    int totalCells = gridW * gridH;
    ctx.recipGridW = ((1ULL << 48) / (unsigned int)gridW) + 1;
    ctx.maxRects.freeRects.reserve((size_t)numItems * 4u);
    ctx.maxRects.newRects.reserve((size_t)numItems * 4u);
    ctx.placements.reserve(numItems);
    ctx.grid.resize(totalCells);
    ctx.visited.resize(totalCells);
    // A reused ctx may carry 1s from a prior flood fill; reset for the
    // visited-guarded concentration/stranded scans.
    memset(&ctx.visited[0], 0, totalCells);
    ctx.ler.heights.resize(gridW);
    ctx.ler.lerStack.resize(gridW + 1);
    ctx.ler.floodStack.resize(totalCells);
    ctx.ler.regionAreas.reserve(16);
    ctx.ler.regionInterior.reserve(16);
    ctx.ler.regionHasLer.reserve(16);
    ctx.skyline.wasteRects.reserve((size_t)numItems * 2u);
    ctx.placementIdGrid.resize(totalCells);
    ctx.bssfPl.reserve(numItems);
    ctx.seedPl.reserve(numItems);
    ctx.bestPl.reserve(numItems);
    // Repair scratch: sized here so TryRepairMove's first-dirty path can
    // memset + build without an allocation in the inner loop. dirty=true
    // ensures the first TryRepairMove call rebuilds from the (empty)
    // ctx.bestPl we're about to populate.
    ctx.repair.grid.resize(totalCells);
    ctx.repair.reachable.resize(totalCells);
    ctx.repair.strandedList.clear();
    ctx.repair.dirty                        = true;
    ctx.skylineWasteCoef                    = DEFAULT_SKYLINE_WASTE_COEF;
    ctx.scoring.groupingWeight              = DEFAULT_GROUPING_WEIGHT;
    ctx.scoring.fragWeight                  = DEFAULT_FRAG_WEIGHT;
    ctx.scoring.groupingPowerQuarters       = DEFAULT_GROUPING_POWER_QUARTERS;
    ctx.grouping.tierWeightExact            = DEFAULT_TIER_WEIGHT_EXACT;
    ctx.grouping.tierWeightCustom           = DEFAULT_TIER_WEIGHT_CUSTOM;
    ctx.grouping.tierWeightType             = DEFAULT_TIER_WEIGHT_TYPE;
    ctx.grouping.tierWeightFunction         = DEFAULT_TIER_WEIGHT_FUNCTION;
    ctx.grouping.tierWeightFlags            = DEFAULT_TIER_WEIGHT_FLAGS;
    ctx.grouping.funcSimFoodFoodRestricted  = DEFAULT_FUNC_SIM_FOOD_FOOD_RESTRICTED;
    ctx.grouping.funcSimFirstaidRobotrepair = DEFAULT_FUNC_SIM_FIRSTAID_ROBOTREPAIR;
    ctx.grouping.softGroupingPct            = DEFAULT_SOFT_GROUPING_PCT;
    // Reserve capacity up front so BuildPairWeightMatrix's assign() doesn't
    // reallocate inside the per-pack critical path.
    ctx.grouping.pairWeightMatrix.reserve((size_t)numItems * (size_t)numItems);
    ctx.grouping.pairWeightMatrixN = 0;
    ctx.cache.count                = 0;
    ctx.cache.ringHead             = 0;

    // Seed Zobrist tables from grid dimensions only — cache entries survive
    // restart boundaries so the key must be stable across restarts. Skip
    // reseed when a reused ctx has matching dims; empty() catches
    // default-constructed ctx where zobristTableW/H are still uninitialized.
    if (ctx.cache.zobristA.empty() || ctx.cache.tableW != gridW || ctx.cache.tableH != gridH)
    {
        ctx.cache.zobristA.resize((size_t)totalCells);
        unsigned long long stateA =
            0xA5A5A5A5A5A5A5A5ULL ^ ((unsigned long long)gridW * 131ULL) ^ ((unsigned long long)gridH * 7919ULL);
        for (int i = 0; i < totalCells; ++i)
            ctx.cache.zobristA[(size_t)i] = splitmix64_next(&stateA);
        ctx.cache.tableW = gridW;
        ctx.cache.tableH = gridH;
    }
    ctx.cache.curHashA = 0;
    ctx.skyline.snapBoundaries.reserve((size_t)numItems + 1);
    ctx.skyline.snapWaste.reserve((size_t)numItems * 30);
    ctx.skyline.snapSkyline.reserve((size_t)numItems * 20);
    ctx.skyline.snapN     = 0;
    ctx.skyline.snapValid = false;
    memset(ctx.typeCount, 0, sizeof(ctx.typeCount));
#ifdef STACKSORT_PROFILE
    ctx.prof.skylinePrefixK         = 0;
    ctx.prof.skylinePrefixCycles    = 0;
    ctx.prof.cyclesSkylineWasteMap  = 0;
    ctx.prof.cyclesSkylineCandidate = 0;
    ctx.prof.cyclesSkylineAdjacency = 0;
    ctx.prof.cyclesSkylineCommit    = 0;
    ctx.prof.cyclesLerHistogram     = 0;
    ctx.prof.cyclesLerStack         = 0;
#endif
}

Result Pack(const GridSpec& dims, const std::vector<Item>& items, TargetDim dim, const volatile long* abortFlag,
            PackContext* reuseCtx)
{
    if (dim == TARGET_H) return PackH(dims, items, abortFlag, reuseCtx);

    // W-mode: transpose, run H-mode, transpose back.
    std::vector<Item> itemsT = items;
    for (size_t i = 0; i < itemsT.size(); ++i)
        std::swap(itemsT[i].w, itemsT[i].h);

    GridSpec dimsT;
    dimsT.gridW  = dims.gridH;
    dimsT.gridH  = dims.gridW;
    dimsT.target = dims.target;
    Result r     = PackH(dimsT, itemsT, abortFlag, reuseCtx);

    for (size_t i = 0; i < r.placements.size(); ++i)
    {
        std::swap(r.placements[i].x, r.placements[i].y);
        std::swap(r.placements[i].w, r.placements[i].h);
    }
    std::swap(r.lerX, r.lerY);
    std::swap(r.lerWidth, r.lerHeight);

    return r;
}

Result PackH(const GridSpec& dims, const std::vector<Item>& items, const volatile long* abortFlag,
             PackContext* reuseCtx)
{
    const int gridW  = dims.gridW;
    const int gridH  = dims.gridH;
    const int target = dims.target;
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

    Heuristics::MaxRectsPack(ctx, dims, sorted, abortFlag, 0, 0, 0);
    if (abortFlag && *abortFlag != 0) return result;
    std::vector<Placement> bssfPl = ctx.placements;

    Heuristics::MaxRectsPack(ctx, dims, sorted, abortFlag, 0, 0, 1);
    if (abortFlag && *abortFlag != 0)
    {
        result.placements = bssfPl;
        return result;
    }

    if (ctx.placements.size() < bssfPl.size()) ctx.placements = bssfPl;
    result.placements = ctx.placements;

    result.allPlaced = (result.placements.size() == items.size());

    Grid::BuildOccupancyGrid(ctx, gridW, gridH);
    Ler::ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, result.lerArea, result.lerWidth, result.lerHeight, result.lerX,
                       result.lerY);
    result.concentration = Ler::ComputeConcentrationAndStrandedCtx(
        ctx, gridW, gridH, result.lerX, result.lerY, result.lerWidth, result.lerHeight, result.strandedCells);

    // Score — PackH uses the compile-time defaults already seeded into
    // ctx.scoring by InitPackContext; no SearchParams plumbing here.
    int numRot           = Geometry::CountRotated(result.placements);
    long long grouping   = Scoring::ComputeGroupingBonus(result.placements, items, ctx);
    result.groupingBonus = grouping;
    result.score         = Scoring::ComputeScore(ctx, result.placements.size(), result.lerArea, result.lerHeight,
                                                 result.concentration, target, numRot, grouping, result.strandedCells);

    return result;
}

} // namespace Search

} // namespace Packer
