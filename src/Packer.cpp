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
        // Tertiary: group same-type items together in packing order.
        // Items placed consecutively tend to end up adjacent on the grid.
        return a.itemTypeId < b.itemTypeId;
    }
};

void Packer::SortItems(std::vector<Item>& items)
{
    std::sort(items.begin(), items.end(), ItemSortCompare());
}

void Packer::InitPackContext(PackContext& ctx, int gridW, int gridH, int numItems)
{
    int totalCells = gridW * gridH;
    ctx.freeRects.reserve(numItems * 4);
    ctx.newRects.reserve(numItems * 4);
    ctx.placements.reserve(numItems);
    ctx.grid.resize(totalCells);
    ctx.visited.resize(totalCells);
    ctx.heights.resize(gridW);
    ctx.lerStack.reserve(gridW + 1);
    ctx.floodStack.reserve(totalCells);
    ctx.regionAreas.reserve(16);
    ctx.wasteRects.reserve(numItems * 2);
    ctx.placementIdGrid.resize(totalCells);
    ctx.skylineWasteCoef = DEFAULT_SKYLINE_WASTE_COEF;
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
                            volatile long* abortFlag)
{
    if (dim == TARGET_H) return PackH(gridW, gridH, items, target, abortFlag);

    // W-mode: transpose, run H-mode, transpose back.
    std::vector<Item> itemsT = items;
    for (size_t i = 0; i < itemsT.size(); ++i)
        std::swap(itemsT[i].w, itemsT[i].h);

    Result r = PackH(gridH, gridW, itemsT, target, abortFlag);

    for (size_t i = 0; i < r.placements.size(); ++i)
    {
        std::swap(r.placements[i].x, r.placements[i].y);
        std::swap(r.placements[i].w, r.placements[i].h);
    }
    std::swap(r.lerX, r.lerY);
    std::swap(r.lerWidth, r.lerHeight);

    return r;
}

Packer::Result Packer::PackH(int gridW, int gridH, const std::vector<Item>& items, int target, volatile long* abortFlag)
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

    PackContext ctx;
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
    long long grouping   = ComputeGroupingBonus(result.placements, items, Packer::DEFAULT_GROUPING_POWER_QUARTERS);
    result.groupingBonus = grouping;
    result.score =
        ComputeScore(result.placements.size(), result.lerArea, result.lerHeight, result.concentration, target, numRot,
                     grouping, result.strandedCells, Packer::DEFAULT_GROUPING_WEIGHT, Packer::DEFAULT_FRAG_WEIGHT);

    return result;
}
