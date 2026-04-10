#include "Packer.h"

#include <cstring>

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

// Compute b^(quarters/4) via int-only ops. Fast paths for the common values
// pin byte-match at the default (quarters=6 → b^1.5, the legacy formula).
// General path uses (b^quarters)^(1/4) via two long-long isqrt passes,
// which is much more accurate than nested int isqrt at small b.
// Bounded by quarters <= 8 + realistic b <= ~80, so b^quarters <= 1.68e15,
// well within long long range.
static long long applyGroupingPower(int b, int quarters)
{
    if (b <= 0 || quarters <= 0) return 0;

    switch (quarters)
    {
    case 4:
        return b; // b^1
    case 6:
        return (long long)b * isqrt(b); // b^1.5 — DEFAULT, must match legacy bit-for-bit
    case 8:
        return (long long)b * b; // b^2
    }

    long long bq = 1;
    for (int i = 0; i < quarters; ++i)
        bq *= b;
    long long root2 = isqrt_ll(bq);
    return isqrt_ll(root2);
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

    for (int y = 0; y < gridH; ++y)
    {
        for (int x = 0; x < gridW; ++x)
        {
            if (grid[y * gridW + x] == 0) ctx.heights[x] = ctx.heights[x] + 1;
            else ctx.heights[x] = 0;
        }

        ctx.lerStack.clear();

        for (int x = 0; x <= gridW; ++x)
        {
            int curHeight = (x < gridW) ? ctx.heights[x] : 0;
            int si        = x;

            while (!ctx.lerStack.empty() && ctx.lerStack.back().height > curHeight)
            {
                LEREntry top = ctx.lerStack.back();
                ctx.lerStack.pop_back();

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

            LEREntry e;
            e.startIdx = si;
            e.height   = curHeight;
            ctx.lerStack.push_back(e);
        }
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
    ctx.regionAreas.clear();
    ctx.regionInterior.clear();
    ctx.regionHasLer.clear();
    int totalFree = 0;

    int lerXEnd = lerX + lerW;
    int lerYEnd = lerY + lerH;

    for (int i = 0; i < totalCells; ++i)
    {
        if (ctx.grid[i] != 0 || ctx.visited[i]) continue;

        ctx.floodStack.clear();
        ctx.floodStack.push_back(i);
        ctx.visited[i]       = 1;
        int area             = 0;
        int interior         = 0;
        unsigned char hasLer = 0;

        while (!ctx.floodStack.empty())
        {
            int ci = ctx.floodStack.back();
            ctx.floodStack.pop_back();
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

            if (cx > 0 && !ctx.visited[ci - 1] && !ctx.grid[ci - 1])
            {
                ctx.visited[ci - 1] = 1;
                ctx.floodStack.push_back(ci - 1);
            }
            if (cx < gridW - 1 && !ctx.visited[ci + 1] && !ctx.grid[ci + 1])
            {
                ctx.visited[ci + 1] = 1;
                ctx.floodStack.push_back(ci + 1);
            }
            if (cy > 0 && !ctx.visited[ci - gridW] && !ctx.grid[ci - gridW])
            {
                ctx.visited[ci - gridW] = 1;
                ctx.floodStack.push_back(ci - gridW);
            }
            if (cy < gridH - 1 && !ctx.visited[ci + gridW] && !ctx.grid[ci + gridW])
            {
                ctx.visited[ci + gridW] = 1;
                ctx.floodStack.push_back(ci + gridW);
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
void Packer::FloodFillFromLer(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW, int lerH)
{
    int totalCells = gridW * gridH;
    if (totalCells == 0) return;

    ctx.visited.resize(totalCells);
    memset(&ctx.visited[0], 0, totalCells);
    ctx.floodStack.clear();

    for (int y = lerY; y < lerY + lerH; ++y)
    {
        for (int x = lerX; x < lerX + lerW; ++x)
        {
            int idx = y * gridW + x;
            if (ctx.grid[idx] == 0 && !ctx.visited[idx])
            {
                ctx.visited[idx] = 1;
                ctx.floodStack.push_back(idx);
            }
        }
    }

    while (!ctx.floodStack.empty())
    {
        int ci = ctx.floodStack.back();
        ctx.floodStack.pop_back();
        int cx = ci % gridW;
        int cy = ci / gridW;

        if (cx > 0 && !ctx.visited[ci - 1] && !ctx.grid[ci - 1])
        {
            ctx.visited[ci - 1] = 1;
            ctx.floodStack.push_back(ci - 1);
        }
        if (cx < gridW - 1 && !ctx.visited[ci + 1] && !ctx.grid[ci + 1])
        {
            ctx.visited[ci + 1] = 1;
            ctx.floodStack.push_back(ci + 1);
        }
        if (cy > 0 && !ctx.visited[ci - gridW] && !ctx.grid[ci - gridW])
        {
            ctx.visited[ci - gridW] = 1;
            ctx.floodStack.push_back(ci - gridW);
        }
        if (cy < gridH - 1 && !ctx.visited[ci + gridW] && !ctx.grid[ci + gridW])
        {
            ctx.visited[ci + gridW] = 1;
            ctx.floodStack.push_back(ci + gridW);
        }
    }
}

void Packer::ComputeLER(const std::vector<unsigned char>& grid, int gridW, int gridH, int& outArea, int& outWidth,
                        int& outHeight, int& outX, int& outY)
{
    PackContext ctx;
    ctx.heights.resize(gridW);
    ctx.lerStack.reserve(gridW + 1);
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
                                          const AdjGraph& g, int n, int groupingPowerQuarters)
{
    if (n <= 1 || n > 256) return 0;

    // Union-find: connect same-type items that share a border
    int parent[256];
    for (int i = 0; i < n; ++i)
        parent[i] = i;

    // Iterate edges (each edge stored in both directions — only process i < j)
    for (int i = 0; i < n; ++i)
    {
        int typeA = items[placements[i].id].itemTypeId;
        for (int k = 0; k < g.count[i]; ++k)
        {
            int j = g.adj[i][k].neighbor;
            if (j <= i) continue; // avoid double-processing
            if (items[placements[j].id].itemTypeId == typeA) uf_unite(parent, i, j);
        }
    }

    // Accumulate borders per connected component
    int compBorders[256];
    memset(compBorders, 0, n * sizeof(int));

    for (int i = 0; i < n; ++i)
    {
        int typeA = items[placements[i].id].itemTypeId;
        for (int k = 0; k < g.count[i]; ++k)
        {
            int j = g.adj[i][k].neighbor;
            if (j <= i) continue;
            if (items[placements[j].id].itemTypeId == typeA) compBorders[uf_find(parent, i)] += g.adj[i][k].border;
        }
    }

    // Super-linear scaling per component: b^(quarters/4) via applyGroupingPower
    long long total = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(parent, i) == i && compBorders[i] > 0)
            total += applyGroupingPower(compBorders[i], groupingPowerQuarters);
    }

    return total;
}

// O(n^2) version -- used by LAHC inner loop (no precomputed graph).

long long Packer::ComputeGroupingBonus(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                       int groupingPowerQuarters)
{
    int n = (int)placements.size();
    if (n <= 1 || n > 256) return 0;

    // Union-find: connect same-type items that share a qualifying border
    int parent[256];
    for (int i = 0; i < n; ++i)
        parent[i] = i;

    // Pass 1: build connected components
    for (int i = 0; i < n; ++i)
    {
        int typeA = items[placements[i].id].itemTypeId;
        for (int j = i + 1; j < n; ++j)
        {
            if (items[placements[j].id].itemTypeId != typeA) continue;
            if (SharedBorder(placements[i], placements[j]) > 0) uf_unite(parent, i, j);
        }
    }

    // Pass 2: accumulate borders per connected component
    int compBorders[256];
    memset(compBorders, 0, n * sizeof(int));

    for (int i = 0; i < n; ++i)
    {
        int typeA = items[placements[i].id].itemTypeId;
        for (int j = i + 1; j < n; ++j)
        {
            if (items[placements[j].id].itemTypeId != typeA) continue;
            int shared = SharedBorder(placements[i], placements[j]);
            if (shared > 0) compBorders[uf_find(parent, i)] += shared;
        }
    }

    // Super-linear scaling per component: b^(quarters/4). Default quarters=6
    // hits the legacy fast-path b * isqrt(b) so byte-match is preserved.
    long long total = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(parent, i) == i && compBorders[i] > 0)
            total += applyGroupingPower(compBorders[i], groupingPowerQuarters);
    }

    return total;
}

// Power-independent border total. Sums Σ b per component with no exponent
// applied. Walks the same union-find structure as ComputeGroupingBonus —
// fork rather than refactor since this is called once per final result and
// the loops are tiny.

long long Packer::ComputeGroupingBordersRaw(const std::vector<Placement>& placements, const std::vector<Item>& items)
{
    int n = (int)placements.size();
    if (n <= 1 || n > 256) return 0;

    int parent[256];
    for (int i = 0; i < n; ++i)
        parent[i] = i;

    for (int i = 0; i < n; ++i)
    {
        int typeA = items[placements[i].id].itemTypeId;
        for (int j = i + 1; j < n; ++j)
        {
            if (items[placements[j].id].itemTypeId != typeA) continue;
            if (SharedBorder(placements[i], placements[j]) > 0) uf_unite(parent, i, j);
        }
    }

    int compBorders[256];
    memset(compBorders, 0, n * sizeof(int));

    for (int i = 0; i < n; ++i)
    {
        int typeA = items[placements[i].id].itemTypeId;
        for (int j = i + 1; j < n; ++j)
        {
            if (items[placements[j].id].itemTypeId != typeA) continue;
            int shared = SharedBorder(placements[i], placements[j]);
            if (shared > 0) compBorders[uf_find(parent, i)] += shared;
        }
    }

    long long total = 0;
    for (int i = 0; i < n; ++i)
    {
        if (uf_find(parent, i) == i && compBorders[i] > 0) total += compBorders[i];
    }

    return total;
}

// Swap same-footprint items to improve clustering.
// Physical layout unchanged since occupied cells are identical.

void Packer::OptimizeGrouping(std::vector<Placement>& placements, const std::vector<Item>& items,
                              int groupingPowerQuarters)
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
        long long curGrouping = ComputeGroupingBonusAdj(placements, items, g, n, groupingPowerQuarters);

        for (int ci = 0; ci < numCandidates; ++ci)
        {
            int pi = candidates[ci].i;
            int pj = candidates[ci].j;

            // Skip if types now match (a prior swap made them same-type)
            if (items[placements[pi].id].itemTypeId == items[placements[pj].id].itemTypeId) continue;

            std::swap(placements[pi].id, placements[pj].id);
            long long newGrouping = ComputeGroupingBonusAdj(placements, items, g, n, groupingPowerQuarters);

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
    std::vector<unsigned char> grid(gridW * gridH, 0);

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
