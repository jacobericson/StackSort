#pragma once

#include "Packer.h"

#include <algorithm>
#include <vector>

namespace Packer
{

namespace Search
{

// Move-type dispatch thresholds on a 0..99 roll. Cascading: the first
// MOVE_*_MAX that roll < passes selects the move. Repair has no MAX
// constant because it's the residual bucket (roll >= MOVE_ROTATE_MAX).
static const int MOVE_SWAP_MAX   = 50; // 0-49  = swap
static const int MOVE_INSERT_MAX = 75; // 50-74 = insert
static const int MOVE_ROTATE_MAX = 90; // 75-89 = rotate-flip, 90-99 = repair

static const int NUM_RESTARTS      = 16;
static const int ITERS_PER_RESTART = 4000; // 16 * 4000 = 64000 total (plateau-limited in practice)
static const int LAHC_HISTORY_LEN  = 200;
static const int PLATEAU_THRESHOLD = 1500; // break restart after this many iters w/o improvement
static const int MIN_RESERVE_W     = 1;    // minimum LER width for pre-reservation scan

// Adaptive restart thresholds
static const int FAST_CONVERGE_ITER     = 200; // restart 0 best found before this iter = fast
static const double GOOD_CONC_THRESHOLD = 0.9; // concentration above this = high quality

// Simple RNG (no <random> in VS2010).
struct LCG
{
    unsigned int state;
    LCG(unsigned int seed) : state(seed ? seed : 1u) {}
    unsigned int next()
    {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    int nextInt(int n)
    {
        return (int)(next() % (unsigned int)n);
    }
    double nextDouble()
    {
        return (double)(next()) / 4294967296.0;
    }
};

// max-of-K uniform draws: CDF = (x/n)^K biases toward tail.
// K = alphaQ/4 so alphaQ=8 → K=2 (linear bias), alphaQ=12 → K=3 (quadratic).
static inline int SampleBiasedIndex(LCG& rng, int n, int alphaQ, int uniformPct)
{
    if (alphaQ <= 0 || uniformPct >= 100) return rng.nextInt(n);
    if (rng.nextInt(100) < uniformPct) return rng.nextInt(n);
    int k    = std::max(alphaQ / 4, 1);
    int best = 0;
    for (int i = 0; i < k; ++i)
    {
        int v = rng.nextInt(n);
        best  = std::max(v, best);
    }
    return best;
}

enum MoveType
{
    MOVE_SWAP,
    MOVE_INSERT,
    MOVE_ROTATE,
    MOVE_REPAIR
};

struct Move
{
    MoveType type;
    int a;
    int b;
    Item savedItem; // snapshot for rotate-flip undo
};

static inline void UndoMove(std::vector<Item>& order, const Move& m)
{
    switch (m.type)
    {
    case MOVE_SWAP:
        std::swap(order[m.a], order[m.b]);
        break;
    case MOVE_INSERT:
    case MOVE_REPAIR:
    {
        // a = original position, b = position after insert. Reverse via
        // remove-from-b + reinsert-at-a.
        Item tmp = order[m.b];
        order.erase(order.begin() + m.b);
        order.insert(order.begin() + m.a, tmp);
        break;
    }
    case MOVE_ROTATE:
        order[m.a] = m.savedItem;
        break;
    }
}

// Fisher-Yates shuffle of same-type item groups as units.
// Items must already be sorted so same-type items are consecutive.
static inline void ShuffleGroups(std::vector<Item>& items, LCG& rng)
{
    if (items.size() <= 1) return;

    // Identify group boundaries
    struct Group
    {
        int start;
        int count;
    };
    std::vector<Group> groups;

    int i = 0;
    while (i < (int)items.size())
    {
        Group g;
        g.start    = i;
        g.count    = 1;
        int typeId = items[i].exactId;
        while (i + g.count < (int)items.size() && items[i + g.count].exactId == typeId)
            ++g.count;
        groups.push_back(g);
        i += g.count;
    }

    if (groups.size() <= 1) return;

    // Fisher-Yates shuffle of groups
    for (int k = (int)groups.size() - 1; k > 0; --k)
    {
        int j = rng.nextInt(k + 1);
        std::swap(groups[k], groups[j]);
    }

    // Rebuild items in new group order
    std::vector<Item> reordered;
    reordered.reserve(items.size());
    for (size_t g = 0; g < groups.size(); ++g)
    {
        for (int k = 0; k < groups[g].count; ++k)
            reordered.push_back(items[groups[g].start + k]);
    }
    items.swap(reordered);
}

// Empty sentinel for placementIdGrid is -1 (not 0) — writing 0 would alias
// pidx=0 and corrupt CollectAdjacentPids.
static inline void RestoreSkylineState(PackContext& ctx, int gridW, int /*gridH*/, int keptPrefix)
{
    const SkylineBoundary& b = ctx.skyline.snapBoundaries[(size_t)keptPrefix];

    // Roll back ctx.grid + placementIdGrid for every placement being
    // discarded. Walking placements directly is equivalent to the old
    // per-cell gridDelta log and avoids the per-cell push_back cost in
    // EmitBoundary's hot path.
    for (size_t i = (size_t)b.placementsCount; i < ctx.placements.size(); ++i)
    {
        const Placement& p = ctx.placements[i];
        for (int dy = 0; dy < p.h; ++dy)
            for (int dx = 0; dx < p.w; ++dx)
            {
                int cellIdx                  = (p.y + dy) * gridW + (p.x + dx);
                ctx.grid[cellIdx]            = 0;
                ctx.placementIdGrid[cellIdx] = PLACEMENT_ID_EMPTY;
            }
    }

    ctx.placements.resize((size_t)b.placementsCount);

    ctx.skyline.wasteRects.reserve((size_t)b.wasteCount);
    ctx.skyline.wasteRects.assign(ctx.skyline.snapWaste.begin() + b.wasteStart,
                                  ctx.skyline.snapWaste.begin() + b.wasteStart + b.wasteCount);

    // Rebuild the skyline linked-list arena from the saved linear slice.
    ctx.skyline.head     = -1;
    ctx.skyline.freeHead = -1;
    ctx.skyline.count    = 0;
    short tail           = -1;
    for (int i = 0; i < b.skylineCount; ++i)
    {
        short idx              = ctx.skyline.count++;
        ctx.skyline.nodes[idx] = ctx.skyline.snapSkyline[(size_t)b.skylineStart + (size_t)i];
        ctx.skyline.next[idx]  = -1;
        if (tail < 0) ctx.skyline.head = idx;
        else ctx.skyline.next[tail] = idx;
        tail = idx;
    }

    ctx.cache.curHashA = b.hashA;

    // SkylinePack's emit-on-placement push_backs must land at boundary k+1;
    // stale entries past boundary[keptPrefix] would otherwise offset them.
    ctx.skyline.snapBoundaries.resize((size_t)keptPrefix + 1);
    ctx.skyline.snapWaste.resize((size_t)b.wasteStart + (size_t)b.wasteCount);
    ctx.skyline.snapSkyline.resize((size_t)b.skylineStart + (size_t)b.skylineCount);
}

// Shared best-state update for the two LAHC sites and the Path Relinking
// site. Keeps bestState / ctx.bestPl / ctx.repair.dirty / *outBestOrder in
// lockstep. Call-site-specific counters (itersSinceImproved,
// bestIterInRestart0, diagBestFound*) stay inline at the LAHC sites.
static inline void UpdateBestFromCurrent(PackContext& ctx, PackState& bestState, const PackState& newState,
                                         std::vector<Item>* outBestOrder, const std::vector<Item>& curOrder)
{
    bestState        = newState;
    ctx.bestPl       = ctx.placements;
    ctx.repair.dirty = true;
    if (outBestOrder) *outBestOrder = curOrder;
}

// Path Relinking elite admission. Normalizes curOrder (resets w/h/canRotate
// from originalItems to neutralize MOVE_ROTATE pollution) then admits via
// diversity-filter + weakest-replacement pool policy. Called from
// PackAnnealedH at every global-best improvement and at each restart's end.
void CapturePathRelinkElite(PackContext& ctx, const std::vector<Item>& curOrder, const std::vector<Item>& originalItems,
                            long long score, int eliteCap, int diversityThreshold);

// Repair-move body. Extracted to PackerRepairMove.cpp. Called when the LAHC
// move-roll lands in the repair bucket: finds a stranded cell in the current
// best packing, finds an item whose dims could fill it, and moves that item
// to position 0 in curOrder. Falls back to a random swap if no repair target
// is found. Sets move.type + move.a/b and mutates curOrder in place.
// Uses bestState.lerX/Y/W/H/strandedCells for repair-target detection and
// ctx.repair.* for the cached bestPl occupancy + reachability + stranded
// list, rebuilt lazily when ctx.repair.dirty.
void TryRepairMove(PackContext& ctx, int gridW, int gridH, std::vector<Item>& curOrder, int n, Move& move, LCG& rng,
                   const PackState& bestState, int effLateBiasAlphaQ, int effLateBiasUniformPct,
                   int& diagRepairMoveScans, int& diagRepairMoveHits);

} // namespace Search

} // namespace Packer
