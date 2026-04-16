#include "Packer.h"

#include <algorithm>
#include <climits>
#include <cstring>

#ifdef STACKSORT_PROFILE
#include <intrin.h>
#pragma intrinsic(__rdtsc)
// Inner-loop phase timing. PROF_DECL snapshots the TSC at the top of an
// iter; PROF_TICK attributes elapsed cycles to the named counter and
// advances the snapshot for the next phase. Zero overhead in non-profile
// builds.
#define PROF_DECL() unsigned long long _profT = __rdtsc()
#define PROF_TICK(acc)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        unsigned long long _now = __rdtsc();                                                                           \
        acc += _now - _profT;                                                                                          \
        _profT = _now;                                                                                                 \
    } while (0)
#else
#define PROF_DECL() ((void)0)
#define PROF_TICK(acc) ((void)0)
#endif

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

static const int NUM_RESTARTS      = 16;
static const int ITERS_PER_RESTART = 4000; // 16 * 4000 = 64000 total (plateau-limited in practice)
static const int LAHC_HISTORY_LEN  = 200;
static const int PLATEAU_THRESHOLD = 1500; // break restart after this many iters w/o improvement
static const int MIN_RESERVE_W     = 1;    // minimum LER width for pre-reservation scan

// Move type weights (out of 100)
static const int MOVE_SWAP_MAX   = 50; // 0-49  = swap
static const int MOVE_INSERT_MAX = 75; // 50-74 = insert
static const int MOVE_ROTATE_MAX = 90; // 75-89 = rotate-flip
                                       // 90-99 = repair

// Adaptive restart thresholds
static const int FAST_CONVERGE_ITER     = 200; // restart 0 best found before this iter = fast
static const double GOOD_CONC_THRESHOLD = 0.9; // concentration above this = high quality

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
    Packer::Item savedItem; // snapshot for rotate-flip undo
};

static void UndoMove(std::vector<Packer::Item>& order, const Move& m)
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
        Packer::Item tmp = order[m.b];
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

static void ShuffleGroups(std::vector<Packer::Item>& items, LCG& rng)
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
        int typeId = items[i].itemTypeId;
        while (i + g.count < (int)items.size() && items[i + g.count].itemTypeId == typeId)
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
    std::vector<Packer::Item> reordered;
    reordered.reserve(items.size());
    for (size_t g = 0; g < groups.size(); ++g)
    {
        for (int k = 0; k < groups[g].count; ++k)
            reordered.push_back(items[groups[g].start + k]);
    }
    items.swap(reordered);
}

Packer::Result Packer::PackAnnealed(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
                                    volatile long* abortFlag, const std::vector<Item>* seedOrder,
                                    std::vector<Item>* outBestOrder, int skipLAHCIfAreaBelow,
                                    const SearchParams* params, PackDiagnostics* outDiag)
{
    if (dim == TARGET_H)
        return PackAnnealedH(gridW, gridH, items, target, abortFlag, seedOrder, outBestOrder, skipLAHCIfAreaBelow,
                             params, outDiag);

    // W-mode: transpose inputs, run H-mode, transpose outputs.
    std::vector<Item> itemsT = items;
    for (size_t i = 0; i < itemsT.size(); ++i)
        std::swap(itemsT[i].w, itemsT[i].h);

    std::vector<Item> seedT;
    const std::vector<Item>* seedTPtr = NULL;
    if (seedOrder)
    {
        seedT = *seedOrder;
        for (size_t i = 0; i < seedT.size(); ++i)
            std::swap(seedT[i].w, seedT[i].h);
        seedTPtr = &seedT;
    }

    Result r = PackAnnealedH(gridH, gridW, itemsT, target, abortFlag, seedTPtr, outBestOrder, skipLAHCIfAreaBelow,
                             params, outDiag);

    for (size_t i = 0; i < r.placements.size(); ++i)
    {
        std::swap(r.placements[i].x, r.placements[i].y);
        std::swap(r.placements[i].w, r.placements[i].h);
    }
    std::swap(r.lerX, r.lerY);
    std::swap(r.lerWidth, r.lerHeight);

    if (outBestOrder)
    {
        for (size_t i = 0; i < outBestOrder->size(); ++i)
            std::swap((*outBestOrder)[i].w, (*outBestOrder)[i].h);
    }

    return r;
}

Packer::Result Packer::PackAnnealedH(int gridW, int gridH, const std::vector<Item>& items, int target,
                                     volatile long* abortFlag, const std::vector<Item>* seedOrder,
                                     std::vector<Item>* outBestOrder, int skipLAHCIfAreaBelow,
                                     const SearchParams* params, PackDiagnostics* outDiag)
{
    // Resolve effective LAHC parameters. Positive overrides win; <= 0 means
    // "use compiled default" (sentinel set by SearchParams default ctor).
    int effRestarts = (params && params->numRestarts > 0) ? params->numRestarts : NUM_RESTARTS;
    int effIters    = (params && params->itersPerRestart > 0) ? params->itersPerRestart : ITERS_PER_RESTART;
    int effHistLen  = (params && params->lahcHistoryLen > 0) ? params->lahcHistoryLen : LAHC_HISTORY_LEN;
    int effPlateau  = (params && params->plateauThreshold > 0) ? params->plateauThreshold : PLATEAU_THRESHOLD;

    // Ablation flags. -1 in SearchParams = use compiled default (on).
    bool enableBafSeed               = !(params && params->enableBafSeed == 0);
    bool enableUnconstrainedFallback = !(params && params->enableUnconstrainedFallback == 0);
    bool enableOptimizeGrouping      = !(params && params->enableOptimizeGrouping == 0);
    bool enableFastConverge          = !(params && params->enableFastConverge == 0);
    bool enableRepairMove            = !(params && params->enableRepairMove == 0);
    bool enablePreReservation        = !(params && params->enablePreReservation == 0);

    // Move-weight thresholds. -1 = default.
    int effSwapMax   = (params && params->moveSwapMax >= 0) ? params->moveSwapMax : MOVE_SWAP_MAX;
    int effInsertMax = (params && params->moveInsertMax >= 0) ? params->moveInsertMax : MOVE_INSERT_MAX;
    int effRotateMax = (params && params->moveRotateMax >= 0) ? params->moveRotateMax : MOVE_ROTATE_MAX;

    // Scoring weights. -1 = compile-time default.
    int effGroupingWeight =
        (params && params->scoringGroupingWeight > 0) ? params->scoringGroupingWeight : Packer::DEFAULT_GROUPING_WEIGHT;
    int effFragWeight =
        (params && params->scoringFragWeight > 0) ? params->scoringFragWeight : Packer::DEFAULT_FRAG_WEIGHT;

    // Skyline tiebreaker waste coefficient. Sentinel <= 0 → default. Resolver
    // enforces >= 1: a coef of 0 makes the inner loop chase contact at the
    // expense of arbitrary waste, which breaks packing.
    int effSkylineWasteCoef =
        (params && params->skylineWasteCoef >= 1) ? params->skylineWasteCoef : Packer::DEFAULT_SKYLINE_WASTE_COEF;

    // Grouping power exponent in quarter-steps: b^(quarters/4). Sentinel
    // <= 0 → default 6 (b^1.5 = legacy). Clamp upper to 8 (= b^2) so the
    // applyGroupingPower general path's b^quarters intermediate stays in
    // long long range with headroom for any realistic component size.
    int effGroupingPower = (params && params->groupingPowerQuarters >= 1 && params->groupingPowerQuarters <= 8)
                               ? params->groupingPowerQuarters
                               : Packer::DEFAULT_GROUPING_POWER_QUARTERS;

    // Diagnostic counters — populated into *outDiag at the bottom of the function.
    int diagPackCalls                 = 0;
    int diagPlateauBreaks             = 0;
    int diagLahcItersExecuted         = 0;
    int diagBestFoundIter             = 0;
    int diagBestFoundRestart          = 0;
    bool diagUnconstrainedFallbackWon = false;
    long long diagGreedySeedScore     = 0;
    int diagGreedySeedLerArea         = 0;
    int diagRepairMoveRolls           = 0;
    int diagRepairMoveScans           = 0;
    int diagRepairMoveHits            = 0;
    int diagRepairMoveAccepts         = 0;

#ifdef STACKSORT_PROFILE
    // Cycle accumulators for the LAHC inner loop. Unsigned so subtraction
    // (when TSC briefly drifts backward across cores) doesn't underflow —
    // we cast once at write-back.
    unsigned long long profMoveGen  = 0;
    unsigned long long profSkyline  = 0;
    unsigned long long profLer      = 0;
    unsigned long long profConc     = 0;
    unsigned long long profGrouping = 0;
    unsigned long long profStranded = 0;
    unsigned long long profScore    = 0;
    unsigned long long profAccept   = 0;
#endif

    // Trivial cases — just do a single greedy pack
    if (items.size() <= 2)
    {
        Result r = PackH(gridW, gridH, items, target, abortFlag);
        if (outDiag)
        {
            outDiag->packCalls         = 2; // PackH runs BSSF + BAF internally
            outDiag->greedySeedScore   = r.score;
            outDiag->greedySeedLerArea = r.lerArea;
        }
        return r;
    }

    // Pre-allocate scratch buffers — reused across all iterations
    PackContext ctx;
    InitPackContext(ctx, gridW, gridH, (int)items.size());
    ctx.skylineWasteCoef = effSkylineWasteCoef;

    // Pre-reservation scan (H > 1 only)
    // Reserve a W x target rectangle at the grid bottom and pack into the
    // L-shaped complement. Try widths from gridW down; accept the widest
    // where all items fit. Right-aligned (reserveX = gridW - W).
    std::vector<Item> order = items;
    SortItems(order);

    int bestReserveW = 0;
    int bestReserveX = 0;
    if (target > 1 && enablePreReservation)
    {
        for (int w = gridW; w >= MIN_RESERVE_W; --w)
        {
            if (abortFlag && *abortFlag != 0) break;
            int rx = gridW - w;
            MaxRectsPack(ctx, gridW, gridH, order, target, abortFlag, rx, w);
            ++diagPackCalls;
            if ((int)ctx.placements.size() == (int)items.size())
            {
                bestReserveW = w;
                bestReserveX = rx;
                break;
            }
        }
    }

    // Aggressive H-skip: if upper-bound LER can't beat caller's threshold,
    // skip LAHC. Still run Pack() for coverage.
    if (skipLAHCIfAreaBelow > 0 && target > 1 && bestReserveW > 0)
    {
        int upperBound = bestReserveW * target;
        if (upperBound < skipLAHCIfAreaBelow)
        {
            Result quickResult = PackH(gridW, gridH, items, target, abortFlag);
            if (outBestOrder) *outBestOrder = order;
            if (outDiag)
            {
                outDiag->packCalls         = diagPackCalls + 2; // PackH = BSSF + BAF internally
                outDiag->greedySeedScore   = quickResult.score;
                outDiag->greedySeedLerArea = quickResult.lerArea;
            }
            return quickResult;
        }
    }

    // Greedy seed: run BSSF. If enableBafSeed, also run BAF and keep whichever placed more.
    MaxRectsPack(ctx, gridW, gridH, order, target, abortFlag, bestReserveX, bestReserveW, 0); // BSSF
    ++diagPackCalls;
    if (abortFlag && *abortFlag != 0)
    {
        Result r;
        r.lerArea       = 0;
        r.lerWidth      = 0;
        r.lerHeight     = 0;
        r.lerX          = 0;
        r.lerY          = 0;
        r.score         = 0;
        r.concentration = 0.0;
        r.strandedCells = 0;
        r.groupingBonus = 0;
        r.allPlaced     = false;
        r.placements    = ctx.placements;
        if (outDiag) outDiag->packCalls = diagPackCalls;
        return r;
    }

    std::vector<Placement> bssfPl = ctx.placements;

    if (enableBafSeed)
    {
        MaxRectsPack(ctx, gridW, gridH, order, target, abortFlag, bestReserveX, bestReserveW, 1); // BAF
        ++diagPackCalls;
        if (abortFlag && *abortFlag != 0)
        {
            Result r;
            r.lerArea       = 0;
            r.lerWidth      = 0;
            r.lerHeight     = 0;
            r.lerX          = 0;
            r.lerY          = 0;
            r.score         = 0;
            r.concentration = 0.0;
            r.strandedCells = 0;
            r.groupingBonus = 0;
            r.allPlaced     = false;
            r.placements    = bssfPl;
            if (outDiag) outDiag->packCalls = diagPackCalls;
            return r;
        }

        if (ctx.placements.size() < bssfPl.size()) ctx.placements = bssfPl;
    }
    else
    {
        ctx.placements = bssfPl;
    }

    std::vector<Placement> seedPl = ctx.placements;

    BuildOccupancyGrid(ctx, gridW, gridH);
    int seedLerA, seedLerW, seedLerH, seedLerX, seedLerY;
    ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, seedLerA, seedLerW, seedLerH, seedLerX, seedLerY);
    int seedStranded = 0;
    double seedConc =
        ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, seedLerX, seedLerY, seedLerW, seedLerH, seedStranded);
    int seedNumRot         = CountRotated(seedPl);
    long long seedGrouping = ComputeGroupingBonus(seedPl, items, effGroupingPower);
    long long seedScore    = ComputeScore(seedPl.size(), seedLerA, seedLerH, seedConc, target, seedNumRot, seedGrouping,
                                          seedStranded, effGroupingWeight, effFragWeight);

    diagGreedySeedScore   = seedScore;
    diagGreedySeedLerArea = seedLerA;

    // Best solution tracking
    std::vector<Placement> bestPl = seedPl;
    long long bestScore           = seedScore;
    int bestLerA                  = seedLerA;
    int bestLerW                  = seedLerW;
    int bestLerH                  = seedLerH;
    int bestLerX                  = seedLerX;
    int bestLerY                  = seedLerY;
    double bestConc               = seedConc;
    int bestNumRot                = seedNumRot;
    long long bestGrouping        = seedGrouping;
    int bestStranded              = seedStranded;

    if (outBestOrder) *outBestOrder = order;

    // Deterministic seed: params->rngSeed wins if non-zero, else derive from (target, numItems).
    unsigned int seed = (params && params->rngSeed != 0)
                            ? params->rngSeed
                            : ((unsigned int)(target * 2654435761u) ^ (unsigned int)(items.size() * 40503u));
    LCG rng(seed);

    int effectiveRestarts  = effRestarts;
    int bestIterInRestart0 = -1;
    std::vector<long long> history(effHistLen);

    // Cached bestPl occupancy grid + LER reachability for repair moves.
    // Only rebuilt when bestPl changes (dirty flag).
    int totalCellsRepair = gridW * gridH;
    std::vector<unsigned char> repairGrid(totalCellsRepair);
    std::vector<unsigned char> repairReachable(totalCellsRepair);
    std::vector<int> repairStrandedList; // interior cells not reachable from LER
    bool repairGridDirty = true;

    for (int restart = 0; restart < effectiveRestarts; ++restart)
    {
        if (abortFlag && *abortFlag != 0) break;

        // Restart 0: cached seed or sorted order. 1+: group-shuffled.
        std::vector<Item> curOrder;
        if (restart == 0 && seedOrder != NULL) curOrder = *seedOrder;
        else curOrder = order;
        if (restart > 0) ShuffleGroups(curOrder, rng);

        // Pack the restart seed
        SkylinePack(ctx, gridW, gridH, curOrder, target, abortFlag, bestReserveX, bestReserveW);
        ++diagPackCalls;
        if (abortFlag && *abortFlag != 0) break;
        BuildOccupancyGrid(ctx, gridW, gridH);

        int curLerA, curLerW, curLerH, curLerX, curLerY;
        ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, curLerA, curLerW, curLerH, curLerX, curLerY);
        int curStranded = 0;
        double curConc =
            ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, curLerX, curLerY, curLerW, curLerH, curStranded);
        int curNumRot         = CountRotated(ctx.placements);
        long long curGrouping = ComputeGroupingBonus(ctx.placements, items, effGroupingPower);
        long long curScore    = ComputeScore(ctx.placements.size(), curLerA, curLerH, curConc, target, curNumRot,
                                             curGrouping, curStranded, effGroupingWeight, effFragWeight);

        if (curScore > bestScore)
        {
            bestScore       = curScore;
            bestPl          = ctx.placements;
            bestLerA        = curLerA;
            bestLerW        = curLerW;
            bestLerH        = curLerH;
            bestLerX        = curLerX;
            bestLerY        = curLerY;
            bestConc        = curConc;
            bestNumRot      = curNumRot;
            bestGrouping    = curGrouping;
            bestStranded    = curStranded;
            repairGridDirty = true;
            if (outBestOrder) *outBestOrder = curOrder;
            if (restart == 0) bestIterInRestart0 = 0;
            diagBestFoundIter    = 0;
            diagBestFoundRestart = restart;
        }

        // Fresh LAHC history per restart (vector allocated once above the loop)
        for (int i = 0; i < effHistLen; ++i)
            history[i] = curScore;

        int itersSinceImproved = 0;

        for (int iter = 0; iter < effIters; ++iter)
        {
            if (abortFlag && *abortFlag != 0) break;
            ++diagLahcItersExecuted;

            PROF_DECL();

            int n = (int)curOrder.size();

            // Diversified move selection
            int moveRoll = rng.nextInt(100);
            // Repair disabled: rolls that would land in the repair bucket
            // [effRotateMax, 100) get remapped into the swap bucket so total
            // move count is preserved.
            if (!enableRepairMove && moveRoll >= effRotateMax) moveRoll = rng.nextInt(effSwapMax > 0 ? effSwapMax : 1);

            Move move;
            move.a = 0;
            move.b = 0;

            if (moveRoll < effSwapMax)
            {
                // Swap move
                move.type = MOVE_SWAP;
                move.a    = rng.nextInt(n);
                move.b    = rng.nextInt(n - 1);
                if (move.b >= move.a) ++move.b;
                std::swap(curOrder[move.a], curOrder[move.b]);
            }
            else if (moveRoll < effInsertMax)
            {
                // Insert move: remove at a, insert at b
                move.type = MOVE_INSERT;
                move.a    = rng.nextInt(n);
                move.b    = rng.nextInt(n - 1);
                if (move.b >= move.a) ++move.b;
                Item tmp = curOrder[move.a];
                curOrder.erase(curOrder.begin() + move.a);
                curOrder.insert(curOrder.begin() + move.b, tmp);
            }
            else if (moveRoll < effRotateMax)
            {
                // Rotate-flip move (15%): force orientation by swapping w/h
                // and locking canRotate=false. Square or non-rotatable
                // victims reroll to swap so we don't waste an LAHC iter
                // on an unchanged ordering.
                move.a                = rng.nextInt(n);
                const Item& candidate = curOrder[move.a];
                if (candidate.canRotate && candidate.w != candidate.h)
                {
                    move.type                  = MOVE_ROTATE;
                    move.savedItem             = candidate;
                    curOrder[move.a].w         = candidate.h;
                    curOrder[move.a].h         = candidate.w;
                    curOrder[move.a].canRotate = false;
                }
                else
                {
                    move.type = MOVE_SWAP;
                    move.b    = rng.nextInt(n - 1);
                    if (move.b >= move.a) ++move.b;
                    std::swap(curOrder[move.a], curOrder[move.b]);
                }
            }
            else
            {
                // Repair move (10%): find a stranded cell in the current best
                // packing, find an item whose dims could fill it, move that
                // item to position 0 in the ordering (highest packing priority).
                // Falls back to random swap if no repair target found.
                ++diagRepairMoveRolls;
                move.type        = MOVE_REPAIR;
                bool repairFound = false;

                if (bestStranded > 0 && !bestPl.empty())
                {
                    ++diagRepairMoveScans;
                    // Rebuild bestPl occupancy + LER reachability only when
                    // bestPl changed. Cache hit: memcpy 800 bytes vs full
                    // rebuild + flood fill (~2-4us).
                    if (repairGridDirty)
                    {
                        memset(&ctx.grid[0], 0, totalCellsRepair);
                        for (size_t pi = 0; pi < bestPl.size(); ++pi)
                        {
                            const Placement& bp = bestPl[pi];
                            for (int dy = 0; dy < bp.h; ++dy)
                                memset(&ctx.grid[(bp.y + dy) * gridW + bp.x], 1, bp.w);
                        }
                        FloodFillFromLer(ctx, gridW, gridH, bestLerX, bestLerY, bestLerW, bestLerH);
                        memcpy(&repairGrid[0], &ctx.grid[0], totalCellsRepair);
                        memcpy(&repairReachable[0], &ctx.visited[0], totalCellsRepair);
                        // Build stranded-cell list in the same scan order the
                        // reservoir loop used to walk, so the RNG stream below
                        // is byte-identical to the pre-cache implementation.
                        repairStrandedList.clear();
                        for (int sy = 1; sy < gridH - 1; ++sy)
                        {
                            for (int sx = 1; sx < gridW - 1; ++sx)
                            {
                                int si = sy * gridW + sx;
                                if (ctx.grid[si] == 0 && !ctx.visited[si]) repairStrandedList.push_back(si);
                            }
                        }
                        repairGridDirty = false;
                    }
                    else
                    {
                        memcpy(&ctx.grid[0], &repairGrid[0], totalCellsRepair);
                        memcpy(&ctx.visited[0], &repairReachable[0], totalCellsRepair);
                    }

                    int strandedX = -1, strandedY = -1;
                    int candidates = 0;
                    for (size_t li = 0; li < repairStrandedList.size(); ++li)
                    {
                        int si = repairStrandedList[li];
                        ++candidates;
                        if (rng.nextInt(candidates) == 0)
                        {
                            strandedX = si % gridW;
                            strandedY = si / gridW;
                        }
                    }

                    if (strandedX >= 0)
                    {
                        // Flood-fill to find connected gap region (capped)
                        int minGX = strandedX, maxGX = strandedX;
                        int minGY = strandedY, maxGY = strandedY;
                        ctx.visited.resize(totalCellsRepair);
                        memset(&ctx.visited[0], 0, totalCellsRepair);
                        ctx.floodStack.clear();
                        ctx.floodStack.push_back(strandedY * gridW + strandedX);
                        ctx.visited[strandedY * gridW + strandedX] = 1;
                        int gapCells                               = 0;
                        while (!ctx.floodStack.empty() && gapCells < 16)
                        {
                            int ci = ctx.floodStack.back();
                            ctx.floodStack.pop_back();
                            ++gapCells;
                            int cx = ci % gridW;
                            int cy = ci / gridW;
                            if (cx < minGX) minGX = cx;
                            if (cx > maxGX) maxGX = cx;
                            if (cy < minGY) minGY = cy;
                            if (cy > maxGY) maxGY = cy;

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

                        int gapW = maxGX - minGX + 1;
                        int gapH = maxGY - minGY + 1;

                        // One pass to find each type's first index + count.
                        // n <= 256 (AdjGraph cap), and typeId <= n-1 by construction.
                        int firstIdx[256];
                        int groupCount[256];
                        for (int i = 0; i < n; ++i)
                        {
                            firstIdx[i]   = -1;
                            groupCount[i] = 0;
                        }
                        for (int ki = 0; ki < n; ++ki)
                        {
                            int typeId = curOrder[ki].itemTypeId;
                            if (typeId < 0 || typeId >= n) continue;
                            if (firstIdx[typeId] < 0) firstIdx[typeId] = ki;
                            ++groupCount[typeId];
                        }

                        for (int ki = 0; ki < n; ++ki)
                        {
                            // Skip first item of a multi-item type group —
                            // it anchors contact-point clustering for its type.
                            int typeId = curOrder[ki].itemTypeId;
                            if (typeId >= 0 && typeId < n && ki == firstIdx[typeId] && groupCount[typeId] > 1) continue;

                            bool fitsNormal = (curOrder[ki].w <= gapW && curOrder[ki].h <= gapH);
                            bool fitsRotated =
                                (curOrder[ki].canRotate && curOrder[ki].h <= gapW && curOrder[ki].w <= gapH);
                            if (fitsNormal || fitsRotated)
                            {
                                // Targeted insert: move this item to position 0
                                move.a   = ki;
                                move.b   = 0;
                                Item tmp = curOrder[move.a];
                                curOrder.erase(curOrder.begin() + move.a);
                                curOrder.insert(curOrder.begin() + move.b, tmp);
                                repairFound = true;
                                ++diagRepairMoveHits;
                                break;
                            }
                        }
                    }
                }

                if (!repairFound)
                {
                    // Fall back to random swap
                    move.type = MOVE_SWAP;
                    move.a    = rng.nextInt(n);
                    move.b    = rng.nextInt(n - 1);
                    if (move.b >= move.a) ++move.b;
                    std::swap(curOrder[move.a], curOrder[move.b]);
                }
            }

            PROF_TICK(profMoveGen);

            // Pack with perturbed order
            SkylinePack(ctx, gridW, gridH, curOrder, target, abortFlag, bestReserveX, bestReserveW);
            ++diagPackCalls;
            if (abortFlag && *abortFlag != 0) break;

            PROF_TICK(profSkyline);

            BuildOccupancyGrid(ctx, gridW, gridH);
            int candLerA, candLerW, candLerH, candLerX, candLerY;
            ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, candLerA, candLerW, candLerH, candLerX, candLerY);
            PROF_TICK(profLer);

            int candStranded = 0;
            double candConc  = ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, candLerX, candLerY, candLerW,
                                                                  candLerH, candStranded);
            // Post-fusion: profConc captures the combined Conc+Stranded work.
            // profStranded stays at 0 in profile builds (kept in schema for
            // pre-fusion comparison).
            PROF_TICK(profConc);

            int candNumRot         = CountRotated(ctx.placements);
            long long candGrouping = ComputeGroupingBonus(ctx.placements, items, effGroupingPower);
            PROF_TICK(profGrouping);

            long long candScore = ComputeScore(ctx.placements.size(), candLerA, candLerH, candConc, target, candNumRot,
                                               candGrouping, candStranded, effGroupingWeight, effFragWeight);
            PROF_TICK(profScore);

            // LAHC acceptance
            int hi      = iter % effHistLen;
            bool accept = (candScore >= curScore || candScore >= history[hi]);

            if (accept)
            {
                if (move.type == MOVE_REPAIR) ++diagRepairMoveAccepts;
                curScore = candScore;
                if (candScore > bestScore)
                {
                    bestScore          = candScore;
                    bestPl             = ctx.placements;
                    bestLerA           = candLerA;
                    bestLerW           = candLerW;
                    bestLerH           = candLerH;
                    bestLerX           = candLerX;
                    bestLerY           = candLerY;
                    bestConc           = candConc;
                    bestNumRot         = candNumRot;
                    bestGrouping       = candGrouping;
                    bestStranded       = candStranded;
                    repairGridDirty    = true;
                    itersSinceImproved = 0;
                    if (outBestOrder) *outBestOrder = curOrder;
                    if (restart == 0) bestIterInRestart0 = iter;
                    diagBestFoundIter    = iter;
                    diagBestFoundRestart = restart;
                }
                else
                {
                    ++itersSinceImproved;
                }
            }
            else
            {
                ++itersSinceImproved;

                UndoMove(curOrder, move);
            }

            history[hi] = curScore;

            // Plateau detection: if no global-best improvement for a full
            // LAHC history cycle, this restart has converged. End early.
            if (itersSinceImproved >= effPlateau)
            {
                ++diagPlateauBreaks;
                PROF_TICK(profAccept);
                break;
            }

            PROF_TICK(profAccept);
        }

        // Adaptive: fast-converging cold starts skip restarts 2-3.
        // Disabled when seeded (seeds bias toward local optima).
        if (enableFastConverge && restart == 0 && seedOrder == NULL)
        {
            if (bestIterInRestart0 >= 0 && bestIterInRestart0 < FAST_CONVERGE_ITER && bestConc > GOOD_CONC_THRESHOLD &&
                bestPl.size() == items.size())
            {
                effectiveRestarts = 2;
            }
        }
    }

    // Unconstrained fallback: items may naturally leave a good gap without
    // being forced into an L-shape. Keep whichever scores higher.
    if (enableUnconstrainedFallback && bestReserveW > 0 && !(abortFlag && *abortFlag != 0))
    {
        Result unconResult = PackH(gridW, gridH, items, target, abortFlag);
        diagPackCalls += 2; // PackH = BSSF + BAF internally

        // Rescore under effective weights: PackH hardcodes defaults, bestScore doesn't.
        if (unconResult.allPlaced)
        {
            long long unGrouping      = ComputeGroupingBonus(unconResult.placements, items, effGroupingPower);
            unconResult.groupingBonus = unGrouping;
            unconResult.score = ComputeScore(unconResult.placements.size(), unconResult.lerArea, unconResult.lerHeight,
                                             unconResult.concentration, target, CountRotated(unconResult.placements),
                                             unGrouping, unconResult.strandedCells, effGroupingWeight, effFragWeight);
        }

        if (unconResult.allPlaced && unconResult.score > bestScore)
        {
            bestPl                       = unconResult.placements;
            bestScore                    = unconResult.score;
            bestLerA                     = unconResult.lerArea;
            bestLerW                     = unconResult.lerWidth;
            bestLerH                     = unconResult.lerHeight;
            bestLerX                     = unconResult.lerX;
            bestLerY                     = unconResult.lerY;
            bestConc                     = unconResult.concentration;
            bestNumRot                   = CountRotated(unconResult.placements);
            bestStranded                 = unconResult.strandedCells;
            diagUnconstrainedFallbackWon = true;
        }
    }

    if (enableOptimizeGrouping) OptimizeGrouping(bestPl, items, effGroupingPower);
    bestGrouping = ComputeGroupingBonus(bestPl, items, effGroupingPower);

    // Rotated flags relative to original input dims
    for (size_t i = 0; i < bestPl.size(); ++i)
        bestPl[i].rotated = (bestPl[i].w != items[bestPl[i].id].w);
    bestNumRot = CountRotated(bestPl);

    Result result;
    result.placements    = bestPl;
    result.lerArea       = bestLerA;
    result.lerWidth      = bestLerW;
    result.lerHeight     = bestLerH;
    result.lerX          = bestLerX;
    result.lerY          = bestLerY;
    result.concentration = bestConc;
    result.strandedCells = bestStranded;
    result.groupingBonus = bestGrouping;
    result.allPlaced     = (bestPl.size() == items.size());
    result.score         = ComputeScore(bestPl.size(), bestLerA, bestLerH, bestConc, target, bestNumRot, bestGrouping,
                                        bestStranded, effGroupingWeight, effFragWeight);

    if (outDiag)
    {
        outDiag->packCalls                = diagPackCalls;
        outDiag->plateauBreaks            = diagPlateauBreaks;
        outDiag->lahcItersExecuted        = diagLahcItersExecuted;
        outDiag->bestFoundIter            = diagBestFoundIter;
        outDiag->bestFoundRestart         = diagBestFoundRestart;
        outDiag->unconstrainedFallbackWon = diagUnconstrainedFallbackWon;
        outDiag->greedySeedScore          = diagGreedySeedScore;
        outDiag->greedySeedLerArea        = diagGreedySeedLerArea;
        outDiag->repairMoveRolls          = diagRepairMoveRolls;
        outDiag->repairMoveScans          = diagRepairMoveScans;
        outDiag->repairMoveHits           = diagRepairMoveHits;
        outDiag->repairMoveAccepts        = diagRepairMoveAccepts;
        // Power-independent clustering metric for cross-power CSV/analysis.
        // Computed once per final result, not per LAHC iter.
        outDiag->groupingBordersRaw = ComputeGroupingBordersRaw(bestPl, items);
#ifdef STACKSORT_PROFILE
        outDiag->profCyclesMoveGen       = (long long)profMoveGen;
        outDiag->profCyclesSkylinePack   = (long long)profSkyline;
        outDiag->profCyclesLer           = (long long)profLer;
        outDiag->profCyclesConcentration = (long long)profConc;
        outDiag->profCyclesGrouping      = (long long)profGrouping;
        outDiag->profCyclesStranded      = (long long)profStranded;
        outDiag->profCyclesScore         = (long long)profScore;
        outDiag->profCyclesAccept        = (long long)profAccept;
#endif
    }
    return result;
}
