#include "Packer.h"

#include <algorithm>
#include <cstring>

#ifdef STACKSORT_PROFILE
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

// Sub-phase counters (STACKSORT_PROFILE_SUBPHASE) — see note in
// PackerSkyline.cpp for why these are gated separately from PROFILE.
#ifdef STACKSORT_PROFILE_SUBPHASE
#define SUBPHASE_BEGIN(tag) unsigned long long _sp_##tag = __rdtsc()
#define SUBPHASE_END(tag, acc)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        acc += (long long)(__rdtsc() - _sp_##tag);                                                                     \
    } while (0)
#else
#define SUBPHASE_BEGIN(tag) ((void)0)
#define SUBPHASE_END(tag, acc) ((void)0)
#endif

// Integer square root via Newton's method (no <cmath> dependency).
static int isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n;
    int y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// Long-long sibling. Used by applyGroupingPower's general path where the
// b^quarters intermediate can exceed int32 range.
static long long isqrt_ll(long long n)
{
    if (n <= 0) return 0;
    long long x = n;
    long long y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// b^(quarters/4). Fast paths keep q=4/6/8 byte-match with the legacy
// formula; general path nests two int isqrts so it always rounds down.
static long long applyGroupingPower(int b, int quarters)
{
    if (b <= 0 || quarters <= 0) return 0;

    switch (quarters)
    {
    case 4:
        return b; // b^1
    case 5:
        return (long long)b * isqrt(isqrt(b)); // b^1.25 — soft track
    case 6:
        return (long long)b * isqrt(b); // b^1.5 — DEFAULT, must match legacy bit-for-bit
    case 8:
        return (long long)b * b; // b^2
    default:
        break;
    }

    long long bq = 1;
    for (int i = 0; i < quarters; ++i)
        bq *= b;
    long long root2 = isqrt_ll(bq);
    return isqrt_ll(root2);
}

// 0..100 multiplier on the function tier. -1 on either side disables; equal
// returns 100; hardcoded cross-function pairs pull their value from ctx so
// ablation can zero them.
static int FunctionSimilarityPct(int funcA, int funcB, const Packer::PackContext& ctx)
{
    if (funcA < 0 || funcB < 0) return 0;
    if (funcA == funcB) return 100;

    int lo = funcA < funcB ? funcA : funcB;
    int hi = funcA < funcB ? funcB : funcA;

    if (lo == 3 && hi == 15) return ctx.funcSimFoodFoodRestricted;  // ITEM_FOOD ↔ ITEM_FOOD_RESTRICTED
    if (lo == 1 && hi == 12) return ctx.funcSimFirstaidRobotrepair; // ITEM_FIRSTAID ↔ ITEM_ROBOTREPAIR

    return 0;
}

// Max weight (0..100) across tiers where a and b match. Function tier is
// pre-multiplied by FunctionSimilarityPct so partial cross-function matches
// contribute less than same-function. Short-circuits on a 100-weight exact match.
static int PairWeight(const Packer::Item& a, const Packer::Item& b, const Packer::PackContext& ctx)
{
    int best = 0;

    if (a.exactId == b.exactId && a.exactId >= 0)
    {
        if (ctx.tierWeightExact >= 100) return ctx.tierWeightExact;
        best = std::max(best, ctx.tierWeightExact);
    }

    if (a.customGroupId >= 0 && a.customGroupId == b.customGroupId) best = std::max(best, ctx.tierWeightCustom);

    if (a.gameDataType >= 0 && a.gameDataType == b.gameDataType) best = std::max(best, ctx.tierWeightType);

    int simPct = FunctionSimilarityPct(a.itemFunction, b.itemFunction, ctx);
    if (simPct > 0)
    {
        int fnWeight = (ctx.tierWeightFunction * simPct + 50) / 100;
        best         = std::max(best, fnWeight);
    }

    // Any shared flag bit triggers the tier — currently bit 0 (food_crop)
    // and bit 1 (trade_item).
    if ((a.flagsMask & b.flagsMask) != 0) best = std::max(best, ctx.tierWeightFlags);

    return best;
}

void Packer::BuildPairWeightMatrix(PackContext& ctx, const std::vector<Item>& items)
{
    int n = (int)items.size();
    // assign() overwrites in place when capacity >= n*n; InitPackContext
    // reserved exactly that much, so this is a memset, not an allocation.
    ctx.pairWeightMatrix.assign((size_t)n * (size_t)n, (unsigned char)0);
    ctx.pairWeightMatrixN = n;
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            unsigned char w = (unsigned char)PairWeight(items[i], items[j], ctx);
            ctx.pairWeightMatrix[(size_t)i * (size_t)n + (size_t)j] = w;
            ctx.pairWeightMatrix[(size_t)j * (size_t)n + (size_t)i] = w;
        }
    }
}

// Scoring constants -- strict tier separation in ComputeScore.
// Each tier's max contribution < the next tier's minimum nonzero delta,
// so lower-priority metrics never override higher ones.
// fragWeight (default 50): max 50pts, < min LER delta of 3 (2^2 - 1^2).
// ROTATION_PENALTY=1: max ~42pts. Pure tiebreaker.
// groupingWeight (default 1): max ~200pts, concentration-discounted.
// fragWeight and groupingWeight are passed into ComputeScore per-call so
// the harness can ablate them via SearchParams overrides. Defaults live in
// Packer.h as DEFAULT_FRAG_WEIGHT / DEFAULT_GROUPING_WEIGHT.

static const int ROTATION_PENALTY = 1;

void Packer::ComputeLERCtx(PackContext& ctx, const unsigned char* grid, int gridW, int gridH, int& outArea,
                           int& outWidth, int& outHeight, int& outX, int& outY)
{
    outArea   = 0;
    outWidth  = 0;
    outHeight = 0;
    outX      = 0;
    outY      = 0;

    if (gridW <= 0 || gridH <= 0) return;

    ctx.heights.assign(gridW, 0);
    int* heights    = &ctx.heights[0];
    LEREntry* stack = &ctx.lerStack[0];

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
        SUBPHASE_END(hist, ctx.profCyclesLerHistogram);

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
        SUBPHASE_END(stk, ctx.profCyclesLerStack);
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

double Packer::ComputeConcentrationAndStrandedCtx(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW,
                                                  int lerH, int& outStrandedCells)
{
    outStrandedCells = 0;

    int totalCells = gridW * gridH;
    if (totalCells == 0) return 0.0;

    ctx.visited.resize(totalCells);
    memset(&ctx.visited[0], 0, totalCells);
    // External clear() can drop size; reclaim for raw-pointer indexing.
    if ((int)ctx.floodStack.size() < totalCells) ctx.floodStack.resize(totalCells);
    ctx.regionAreas.clear();
    ctx.regionInterior.clear();
    ctx.regionHasLer.clear();
    int* flood                   = &ctx.floodStack[0];
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

        ctx.regionAreas.push_back(area);
        ctx.regionInterior.push_back(interior);
        ctx.regionHasLer.push_back(hasLer);
        totalFree += area;
    }

    // Stranded count: sum interior counts from regions NOT connected to
    // the LER. When no region has hasLer==1 (lerW==0 / lerH==0 / full grid),
    // every region contributes, matching legacy's "all interior cells
    // stranded when no LER" behavior.
    int stranded = 0;
    for (size_t r = 0; r < ctx.regionAreas.size(); ++r)
    {
        if (!ctx.regionHasLer[r]) stranded += ctx.regionInterior[r];
    }
    outStrandedCells = stranded;

    if (totalFree == 0) return 0.0;

    // HHI: preserve legacy per-region summation order bit-for-bit. Changing
    // to sumSq/(totalFree*totalFree) would drift by ~1 ULP, which then gets
    // amplified by ComputeScore's groupingBonus*concentration*groupingWeight
    // multiply chain into visible long long score differences.
    double hhi = 0.0;
    for (size_t r = 0; r < ctx.regionAreas.size(); ++r)
    {
        double share = (double)ctx.regionAreas[r] / (double)totalFree;
        hhi += share * share;
    }
    return hhi;
}

// Shared helper used by the repair_move branch in PackAnnealedH. Assumes
// ctx.grid is already populated. Resets ctx.visited and flood-fills
// 4-connected empty cells starting from all empty cells inside the LER
// rectangle. On return, ctx.visited[i] == 1 iff cell i is empty AND
// reachable from the LER through empty cells.
void Packer::FloodFillFromLer(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW, int lerH,
                              const unsigned char* extGrid)
{
    int totalCells = gridW * gridH;
    if (totalCells == 0) return;

    ctx.visited.resize(totalCells);
    memset(&ctx.visited[0], 0, totalCells);
    // External clear() can drop size; reclaim for raw-pointer indexing.
    if ((int)ctx.floodStack.size() < totalCells) ctx.floodStack.resize(totalCells);
    int* flood                   = &ctx.floodStack[0];
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

void Packer::ComputeLER(const std::vector<unsigned char>& grid, int gridW, int gridH, int& outArea, int& outWidth,
                        int& outHeight, int& outX, int& outY)
{
    PackContext ctx;
    ctx.heights.resize(gridW);
    ctx.lerStack.resize(gridW + 1);
    ComputeLERCtx(ctx, &grid[0], gridW, gridH, outArea, outWidth, outHeight, outX, outY);
}

long long Packer::ComputeScore(size_t numPlaced, int lerArea, int lerHeight, double concentration, int target,
                               int numRotated, long long groupingBonus, int strandedCells, int groupingWeight,
                               int fragWeight)
{
    long long score = (long long)numPlaced * 1000000LL;
    score += (long long)lerArea * (long long)lerArea;
    // Stranded cell penalty: interior empty cells outside the LER are
    // geometrically unreachable waste. Quadratic penalty (same tier as LER)
    // provides steep gradient away from scattered configurations.
    score -= (long long)strandedCells * (long long)strandedCells;
    if (lerHeight >= target) score += TARGET_BONUS;
    score += (long long)(concentration * (double)fragWeight);
    score -= (long long)numRotated * ROTATION_PENALTY;
    // Grouping bonus discounted by concentration: clustering that fragments
    // the free space gets reduced credit. concentration=1.0 (one blob) = full
    // bonus, concentration=0.3 (scattered gaps) = 30% bonus. groupingBonus
    // is already power-applied (b^(quarters/4)) by ComputeGroupingBonus.
    score += (long long)((double)groupingBonus * concentration * groupingWeight);
    return score;
}

static const int FLUSH_BONUS = 1;

static int uf_find(int* parent, int x)
{
    while (parent[x] != x)
    {
        parent[x] = parent[parent[x]];
        x         = parent[x];
    }
    return x;
}

static void uf_unite(int* parent, int a, int b)
{
    a = uf_find(parent, a);
    b = uf_find(parent, b);
    if (a != b) parent[a] = b;
}

// Compute shared border between two placements, applying corner filter
// and flush bonus. Returns 0 if no qualifying border.
int Packer::SharedBorder(const Placement& a, const Placement& b)
{
    // AABB early-out: if one rect's left edge is past the other's right edge
    // (or symmetric in y), the pair can't share a border. Strict > so
    // touching pairs (a.x + a.w == b.x, etc.) still reach the overlap logic.
    if (a.x > b.x + b.w || b.x > a.x + a.w || a.y > b.y + b.h || b.y > a.y + a.h) return 0;

    int overlapX1 = (a.x > b.x) ? a.x : b.x;
    int overlapX2 = ((a.x + a.w) < (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int overlapY1 = (a.y > b.y) ? a.y : b.y;
    int overlapY2 = ((a.y + a.h) < (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);

    int total = 0;

    // Horizontal border (a bottom touches b top, or vice versa)
    if (overlapX1 < overlapX2 && (a.y + a.h == b.y || b.y + b.h == a.y))
    {
        int sharedW   = overlapX2 - overlapX1;
        bool fullSide = (sharedW == a.w || sharedW == b.w);
        // Corner filter only for sides > 2: small items always count
        if (sharedW > 1 || a.w <= 2 || b.w <= 2)
        {
            total += sharedW;
            if (fullSide) total += FLUSH_BONUS;
        }
    }

    // Vertical border (a right touches b left, or vice versa)
    if (overlapY1 < overlapY2 && (a.x + a.w == b.x || b.x + b.w == a.x))
    {
        int sharedH   = overlapY2 - overlapY1;
        bool fullSide = (sharedH == a.h || sharedH == b.h);
        // Corner filter only for sides > 2: small items always count
        if (sharedH > 1 || a.h <= 2 || b.h <= 2)
        {
            total += sharedH;
            if (fullSide) total += FLUSH_BONUS;
        }
    }

    return total;
}

void Packer::BuildAdjGraph(AdjGraph& g, const std::vector<Placement>& placements)
{
    int n = (int)placements.size();
    if (n > 256) return;
    memset(g.count, 0, n * sizeof(int));

    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            int b = SharedBorder(placements[i], placements[j]);
            if (b > 0)
            {
                // Drop symmetrically if either side is full --
                // ComputeGroupingBonusAdj walks j > i only, so a one-sided
                // entry would be silently missed.
                if (g.count[i] < 24 && g.count[j] < 24)
                {
                    g.adj[i][g.count[i]].neighbor = j;
                    g.adj[i][g.count[i]].border   = b;
                    ++g.count[i];
                    g.adj[j][g.count[j]].neighbor = i;
                    g.adj[j][g.count[j]].border   = b;
                    ++g.count[j];
                }
            }
        }
    }
}

// O(E) version of ComputeGroupingBonus using precomputed AdjGraph edges.
// Used by OptimizeGrouping's inner loop.

long long Packer::ComputeGroupingBonusAdj(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                          const AdjGraph& g, int n, const PackContext& ctx, int groupingPowerQuarters)
{
    if (n <= 1 || n > 256) return 0;

    bool softActive              = ctx.softGroupingPct > 0;
    const int matN               = ctx.pairWeightMatrixN;
    const unsigned char* matBase = softActive && (matN > 0) ? &ctx.pairWeightMatrix[0] : (const unsigned char*)0;

    int parent[256];
    int compBorders[256];
    int softParent[256];
    int softCompBorders[256];
    for (int i = 0; i < n; ++i)
    {
        parent[i]          = i;
        compBorders[i]     = 0;
        softParent[i]      = i;
        softCompBorders[i] = 0;
    }

    for (int i = 0; i < n; ++i)
    {
        int idA = placements[i].id;
        int exA = items[idA].exactId;
        for (int k = 0; k < g.count[i]; ++k)
        {
            int j = g.adj[i][k].neighbor;
            if (j <= i) continue;
            int idB = placements[j].id;
            if (exA >= 0 && items[idB].exactId == exA)
            {
                uf_unite(parent, i, j);
                compBorders[uf_find(parent, i)] += g.adj[i][k].border;
            }
            else if (softActive)
            {
                int w = matBase ? matBase[(size_t)idA * (size_t)matN + (size_t)idB]
                                : PairWeight(items[idA], items[idB], ctx);
                if (w <= 0) continue;
                uf_unite(softParent, i, j);
                softCompBorders[uf_find(softParent, i)] += g.adj[i][k].border * w;
            }
        }
    }

    for (int i = 0; i < n; ++i)
    {
        int root = uf_find(parent, i);
        if (root != i && compBorders[i] > 0)
        {
            compBorders[root] += compBorders[i];
            compBorders[i] = 0;
        }
    }

    long long exactBonus = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(parent, i) == i && compBorders[i] > 0)
            exactBonus += applyGroupingPower(compBorders[i], groupingPowerQuarters);
    }

    if (!softActive) return exactBonus;

    for (int i = 0; i < n; ++i)
    {
        int root = uf_find(softParent, i);
        if (root != i && softCompBorders[i] > 0)
        {
            softCompBorders[root] += softCompBorders[i];
            softCompBorders[i] = 0;
        }
    }

    long long softBonus = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(softParent, i) == i && softCompBorders[i] > 0)
            softBonus += applyGroupingPower(softCompBorders[i] / 100, 5);
    }
    return exactBonus + softBonus * ctx.softGroupingPct / 100;
}

// O(n^2) version -- used by LAHC inner loop (no precomputed graph).

long long Packer::ComputeGroupingBonus(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                       const PackContext& ctx, int groupingPowerQuarters, long long* outExactOnly)
{
    int n = (int)placements.size();
    if (n <= 1 || n > 256)
    {
        if (outExactOnly) *outExactOnly = 0;
        return 0;
    }

    bool softActive              = ctx.softGroupingPct > 0;
    const int matN               = ctx.pairWeightMatrixN;
    const unsigned char* matBase = softActive && (matN > 0) ? &ctx.pairWeightMatrix[0] : (const unsigned char*)0;

    int parent[256];
    int compBorders[256];
    int softParent[256];
    int softCompBorders[256];
    for (int i = 0; i < n; ++i)
    {
        parent[i]          = i;
        compBorders[i]     = 0;
        softParent[i]      = i;
        softCompBorders[i] = 0;
    }

    if (!softActive)
    {
        // Same-exactId pairs only can unite. Sort by exactId and enumerate
        // within-run pairs; union order is irrelevant since downstream only
        // consumes per-component totals (fixup + applyGroupingPower).
        int exBuf[256];
        int ixBuf[256];
        int m = 0;
        for (int i = 0; i < n; ++i)
        {
            int ex = items[placements[i].id].exactId;
            if (ex < 0) continue;
            exBuf[m] = ex;
            ixBuf[m] = i;
            ++m;
        }
        for (int p = 1; p < m; ++p)
        {
            int keyEx = exBuf[p];
            int keyIx = ixBuf[p];
            int q     = p - 1;
            while (q >= 0 && exBuf[q] > keyEx)
            {
                exBuf[q + 1] = exBuf[q];
                ixBuf[q + 1] = ixBuf[q];
                --q;
            }
            exBuf[q + 1] = keyEx;
            ixBuf[q + 1] = keyIx;
        }
        int runStart = 0;
        while (runStart < m)
        {
            int runEnd = runStart + 1;
            while (runEnd < m && exBuf[runEnd] == exBuf[runStart])
                ++runEnd;
            for (int pi = runStart; pi < runEnd; ++pi)
            {
                int i = ixBuf[pi];
                for (int pj = pi + 1; pj < runEnd; ++pj)
                {
                    int j      = ixBuf[pj];
                    int shared = SharedBorder(placements[i], placements[j]);
                    if (shared <= 0) continue;
                    uf_unite(parent, i, j);
                    compBorders[uf_find(parent, i)] += shared;
                }
            }
            runStart = runEnd;
        }
    }
    else
    {
        // Soft track active: every pair can contribute via PairWeight even
        // without an exactId match, so we can't skip cross-bucket pairs.
        for (int i = 0; i < n; ++i)
        {
            int idA = placements[i].id;
            int exA = items[idA].exactId;
            for (int j = i + 1; j < n; ++j)
            {
                int shared = SharedBorder(placements[i], placements[j]);
                if (shared <= 0) continue;
                int idB = placements[j].id;
                if (exA >= 0 && items[idB].exactId == exA)
                {
                    uf_unite(parent, i, j);
                    compBorders[uf_find(parent, i)] += shared;
                }
                else
                {
                    int w = matBase ? matBase[(size_t)idA * (size_t)matN + (size_t)idB]
                                    : PairWeight(items[idA], items[idB], ctx);
                    if (w <= 0) continue;
                    uf_unite(softParent, i, j);
                    softCompBorders[uf_find(softParent, i)] += shared * w;
                }
            }
        }
    }

    // Fixup: collect borders scattered at intermediate roots to final roots.
    for (int i = 0; i < n; ++i)
    {
        int root = uf_find(parent, i);
        if (root != i && compBorders[i] > 0)
        {
            compBorders[root] += compBorders[i];
            compBorders[i] = 0;
        }
    }

    long long exactBonus = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(parent, i) == i && compBorders[i] > 0)
            exactBonus += applyGroupingPower(compBorders[i], groupingPowerQuarters);
    }

    if (outExactOnly) *outExactOnly = exactBonus;
    if (!softActive) return exactBonus;

    for (int i = 0; i < n; ++i)
    {
        int root = uf_find(softParent, i);
        if (root != i && softCompBorders[i] > 0)
        {
            softCompBorders[root] += softCompBorders[i];
            softCompBorders[i] = 0;
        }
    }

    long long softBonus = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(softParent, i) == i && softCompBorders[i] > 0)
            softBonus += applyGroupingPower(softCompBorders[i] / 100, 5);
    }
    return exactBonus + softBonus * ctx.softGroupingPct / 100;
}

// Power-independent border total. Sums weighted Σ b per component with no
// exponent applied. Walks the same union-find structure as ComputeGroupingBonus
// — fork rather than refactor since this is called once per final result.

long long Packer::ComputeGroupingBordersRaw(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                            const PackContext& ctx)
{
    int n = (int)placements.size();
    if (n <= 1 || n > 256) return 0;

    bool softActive              = ctx.softGroupingPct > 0;
    const int matN               = ctx.pairWeightMatrixN;
    const unsigned char* matBase = softActive && (matN > 0) ? &ctx.pairWeightMatrix[0] : (const unsigned char*)0;

    int parent[256];
    int compBorders[256];
    int softParent[256];
    int softCompBorders[256];
    for (int i = 0; i < n; ++i)
    {
        parent[i]          = i;
        compBorders[i]     = 0;
        softParent[i]      = i;
        softCompBorders[i] = 0;
    }

    for (int i = 0; i < n; ++i)
    {
        int idA = placements[i].id;
        int exA = items[idA].exactId;
        for (int j = i + 1; j < n; ++j)
        {
            int shared = SharedBorder(placements[i], placements[j]);
            if (shared <= 0) continue;
            int idB = placements[j].id;
            if (exA >= 0 && items[idB].exactId == exA)
            {
                uf_unite(parent, i, j);
                compBorders[uf_find(parent, i)] += shared;
            }
            else if (softActive)
            {
                int w = matBase ? matBase[(size_t)idA * (size_t)matN + (size_t)idB]
                                : PairWeight(items[idA], items[idB], ctx);
                if (w <= 0) continue;
                uf_unite(softParent, i, j);
                softCompBorders[uf_find(softParent, i)] += shared * w;
            }
        }
    }

    for (int i = 0; i < n; ++i)
    {
        int root = uf_find(parent, i);
        if (root != i && compBorders[i] > 0)
        {
            compBorders[root] += compBorders[i];
            compBorders[i] = 0;
        }
    }

    long long total = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(parent, i) == i && compBorders[i] > 0) total += compBorders[i];
    }

    if (!softActive) return total;

    for (int i = 0; i < n; ++i)
    {
        int root = uf_find(softParent, i);
        if (root != i && softCompBorders[i] > 0)
        {
            softCompBorders[root] += softCompBorders[i];
            softCompBorders[i] = 0;
        }
    }

    long long softTotal = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(softParent, i) == i && softCompBorders[i] > 0) softTotal += softCompBorders[i] / 100;
    }
    return total + softTotal * ctx.softGroupingPct / 100;
}

// Swap same-footprint items to improve clustering.
// Physical layout unchanged since occupied cells are identical.

void Packer::OptimizeGrouping(std::vector<Placement>& placements, const std::vector<Item>& items,
                              const PackContext& ctx, int groupingPowerQuarters)
{
    int n = (int)placements.size();
    if (n <= 1 || n > 256) return;

    // Precompute adjacency graph (O(n²) once)
    AdjGraph g;
    BuildAdjGraph(g, placements);

    // Precompute candidate swap pairs by footprint.
    // Group placements by (w,h). Only cross-type pairs within the same
    // footprint group are valid swap candidates.
    // Cap at n * 16 pairs -- realistic footprint groups are small
    // (same w/h, different type), so this is plenty for 20x20 grids.
    struct CandPair
    {
        int i;
        int j;
    };
    static const int MAX_CANDIDATES = 256 * 16;
    CandPair candidates[MAX_CANDIDATES];
    int numCandidates = 0;

    // Sort indices by footprint key for grouping
    int sortedIdx[256];
    int footprintKey[256];
    for (int i = 0; i < n; ++i)
    {
        sortedIdx[i]    = i;
        footprintKey[i] = (placements[i].w << 16) | placements[i].h;
    }
    // Insertion sort (n <= 256, simpler than std::sort with functor)
    for (int i = 1; i < n; ++i)
    {
        int key = footprintKey[sortedIdx[i]];
        int tmp = sortedIdx[i];
        int j   = i - 1;
        while (j >= 0 && footprintKey[sortedIdx[j]] > key)
        {
            sortedIdx[j + 1] = sortedIdx[j];
            --j;
        }
        sortedIdx[j + 1] = tmp;
    }

    // Find runs of same footprint and generate cross-type candidate pairs
    int runStart = 0;
    while (runStart < n)
    {
        int runEnd = runStart + 1;
        while (runEnd < n && footprintKey[sortedIdx[runEnd]] == footprintKey[sortedIdx[runStart]])
            ++runEnd;

        // All pairs within this footprint group. Include same-type pairs
        // because swaps mutate the type at each position — a pair that starts
        // same-type may become cross-type after earlier swaps. The inner loop's
        // same-type skip handles the common case cheaply.
        for (int a = runStart; a < runEnd; ++a)
        {
            for (int b = a + 1; b < runEnd; ++b)
            {
                if (numCandidates < MAX_CANDIDATES)
                {
                    candidates[numCandidates].i = sortedIdx[a];
                    candidates[numCandidates].j = sortedIdx[b];
                    ++numCandidates;
                }
            }
        }

        runStart = runEnd;
    }

    if (numCandidates == 0) return;

    // Greedy swap loop using adjacency-based scoring
    bool improved = true;
    while (improved)
    {
        improved              = false;
        long long curGrouping = ComputeGroupingBonusAdj(placements, items, g, n, ctx, groupingPowerQuarters);

        for (int ci = 0; ci < numCandidates; ++ci)
        {
            int pi = candidates[ci].i;
            int pj = candidates[ci].j;

            // Skip items of the same template — swap is a no-op for scoring.
            if (items[placements[pi].id].exactId == items[placements[pj].id].exactId) continue;

            std::swap(placements[pi].id, placements[pj].id);
            long long newGrouping = ComputeGroupingBonusAdj(placements, items, g, n, ctx, groupingPowerQuarters);

            if (newGrouping > curGrouping)
            {
                curGrouping = newGrouping;
                improved    = true;
            }
            else
            {
                std::swap(placements[pi].id, placements[pj].id);
            }
        }
    }
}

bool Packer::ValidatePlacements(int gridW, int gridH, const std::vector<Placement>& placements)
{
    std::vector<unsigned char> grid((size_t)gridW * (size_t)gridH, 0);

    for (size_t i = 0; i < placements.size(); ++i)
    {
        const Placement& p = placements[i];
        for (int dy = 0; dy < p.h; ++dy)
        {
            for (int dx = 0; dx < p.w; ++dx)
            {
                int cx = p.x + dx;
                int cy = p.y + dy;
                if (cx < 0 || cx >= gridW || cy < 0 || cy >= gridH) return false;
                int idx = cy * gridW + cx;
                if (grid[idx]) return false;
                grid[idx] = 1;
            }
        }
    }

    return true;
}
