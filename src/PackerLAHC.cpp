#include "Packer.h"
#include "PackerLAHCInternals.h"
#include "PackerProfile.h"

#include <algorithm>
#include <climits>
#include <cstring>

Packer::Result Packer::PackAnnealed(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
                                    const volatile long* abortFlag, const std::vector<Item>* seedOrder,
                                    std::vector<Item>* outBestOrder, int skipLAHCIfAreaBelow,
                                    const SearchParams* params, PackDiagnostics* outDiag, PackContext* reuseCtx)
{
    if (dim == TARGET_H)
        return PackAnnealedH(gridW, gridH, items, target, abortFlag, seedOrder, outBestOrder, skipLAHCIfAreaBelow,
                             params, outDiag, reuseCtx);

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

    // NOLINTNEXTLINE(readability-suspicious-call-argument) — W-mode transpose: itemsT has w/h swapped and placements are swapped back after the call.
    Result r = PackAnnealedH(gridH, gridW, itemsT, target, abortFlag, seedTPtr, outBestOrder, skipLAHCIfAreaBelow,
                             params, outDiag, reuseCtx);

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
                                     const volatile long* abortFlag, const std::vector<Item>* seedOrder,
                                     std::vector<Item>* outBestOrder, int skipLAHCIfAreaBelow,
                                     const SearchParams* params, PackDiagnostics* outDiag, PackContext* reuseCtx)
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

    // StripShift/TileSwap run by default; pass 0 in SearchParams to ablate off.
    bool enableStripShift = !(params && params->enableStripShift == 0);
    bool enableTileSwap   = !(params && params->enableTileSwap == 0);

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

    // Grouping tier weights. Sentinel -1 → compiled default; clamp upper to 100.
    // A weight of 0 disables that tier in PairWeight. Function-similarity
    // overrides fold into the function tier before the MAX-across-tiers step.
    int effTierWeightExact =
        (params && params->tierWeightExact >= 0) ? params->tierWeightExact : Packer::DEFAULT_TIER_WEIGHT_EXACT;
    int effTierWeightCustom =
        (params && params->tierWeightCustom >= 0) ? params->tierWeightCustom : Packer::DEFAULT_TIER_WEIGHT_CUSTOM;
    int effTierWeightType =
        (params && params->tierWeightType >= 0) ? params->tierWeightType : Packer::DEFAULT_TIER_WEIGHT_TYPE;
    int effTierWeightFunction =
        (params && params->tierWeightFunction >= 0) ? params->tierWeightFunction : Packer::DEFAULT_TIER_WEIGHT_FUNCTION;
    int effTierWeightFlags =
        (params && params->tierWeightFlags >= 0) ? params->tierWeightFlags : Packer::DEFAULT_TIER_WEIGHT_FLAGS;
    effTierWeightExact    = std::min(effTierWeightExact, 100);
    effTierWeightCustom   = std::min(effTierWeightCustom, 100);
    effTierWeightType     = std::min(effTierWeightType, 100);
    effTierWeightFunction = std::min(effTierWeightFunction, 100);
    effTierWeightFlags    = std::min(effTierWeightFlags, 100);

    int effFuncSimFoodFoodRestricted  = (params && params->funcSimFoodFoodRestricted >= 0)
                                            ? params->funcSimFoodFoodRestricted
                                            : Packer::DEFAULT_FUNC_SIM_FOOD_FOOD_RESTRICTED;
    int effFuncSimFirstaidRobotrepair = (params && params->funcSimFirstaidRobotrepair >= 0)
                                            ? params->funcSimFirstaidRobotrepair
                                            : Packer::DEFAULT_FUNC_SIM_FIRSTAID_ROBOTREPAIR;
    effFuncSimFoodFoodRestricted      = std::min(effFuncSimFoodFoodRestricted, 100);
    effFuncSimFirstaidRobotrepair     = std::min(effFuncSimFirstaidRobotrepair, 100);

    // Soft-grouping scale. 0 disables the soft track; otherwise each soft
    // edge contributes shared * pair_weight * soft_pct / 10000.
    int effSoftGroupingPct =
        (params && params->softGroupingPct >= 0) ? params->softGroupingPct : Packer::DEFAULT_SOFT_GROUPING_PCT;

    // Path Relinking: post-restart intensification over per-restart elites.
    // Compiled default off to preserve baseline parity; flip via SearchParams
    // or [features] enable_path_relinking. Guard all PR work beneath this flag
    // so baseline runs are byte-identical.
    bool enablePathRelinking =
        (params && params->enablePathRelinking == 1) ||
        (params && params->enablePathRelinking == -1 && Packer::DEFAULT_ENABLE_PATH_RELINKING != 0);
    int effPathRelinkEliteCap =
        (params && params->pathRelinkEliteCap > 0) ? params->pathRelinkEliteCap : Packer::DEFAULT_PATH_RELINK_ELITE_CAP;
    int effPathRelinkDiversityPct = (params && params->pathRelinkDiversityPct >= 0)
                                        ? params->pathRelinkDiversityPct
                                        : Packer::DEFAULT_PATH_RELINK_DIVERSITY_PCT;
    int effPathRelinkMaxPathLen   = (params && params->pathRelinkMaxPathLen > 0) ? params->pathRelinkMaxPathLen : 0;

    // Late-biased move generation. alphaQ=0 or uniformPct=100 disables.
    int effLateBiasAlphaQ =
        (params && params->lateBiasAlphaQ >= 0) ? params->lateBiasAlphaQ : Packer::DEFAULT_LATE_BIAS_ALPHA_Q;
    int effLateBiasUniformPct = (params && params->lateBiasUniformPct >= 0) ? params->lateBiasUniformPct
                                                                            : Packer::DEFAULT_LATE_BIAS_UNIFORM_PCT;

    // Diagnostic counters — populated into *outDiag at the bottom of the function.
    int diagPackCalls                         = 0;
    int diagPlateauBreaks                     = 0;
    int diagLahcItersExecuted                 = 0;
    int diagBestFoundIter                     = 0;
    int diagBestFoundRestart                  = 0;
    bool diagUnconstrainedFallbackWon         = false;
    long long diagGreedySeedScore             = 0;
    int diagGreedySeedLerArea                 = 0;
    int diagRepairMoveRolls                   = 0;
    int diagRepairMoveScans                   = 0;
    int diagRepairMoveHits                    = 0;
    int diagRepairMoveAccepts                 = 0;
    int diagSkylineSnapHits                   = 0;
    int diagSkylineSnapProbes                 = 0;
    int diagPathRelinkPairsRun                = 0;
    int diagPathRelinkIntermediatesScored     = 0;
    int diagPathRelinkGlobalBestUpdates       = 0;
    int diagPathRelinkAbortedPaths            = 0;
    long long diagPathRelinkAvgPathLenSum     = 0;
    long long diagPathRelinkGlobalBestGainMax = 0;

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
    // Per-run phase accumulators (outside the LAHC inner loop).
    unsigned long long profPreReservation        = 0;
    unsigned long long profGreedySeed            = 0;
    unsigned long long profUnconstrainedFallback = 0;
    unsigned long long profOptimizeGrouping      = 0;
    unsigned long long profStripShift            = 0;
    unsigned long long profTileSwap              = 0;
    unsigned long long profPathRelink            = 0;
    unsigned long long profBordersRaw            = 0;

    long long diagKeptPrefixSum = 0;
    int diagKeptPrefixCount     = 0;
    int diagGridHashProbes      = 0;
    int diagGridHashHits        = 0;
#endif

    // Trivial cases — just do a single greedy pack
    if (items.size() <= 2)
    {
        Result r = PackH(gridW, gridH, items, target, abortFlag, reuseCtx);
        if (outDiag)
        {
            outDiag->packCalls         = 2; // PackH runs BSSF + BAF internally
            outDiag->greedySeedScore   = r.score;
            outDiag->greedySeedLerArea = r.lerArea;
        }
        return r;
    }

    // Pre-allocate scratch buffers — reused across all iterations.
    PackContext localCtx;
    PackContext& ctx = reuseCtx ? *reuseCtx : localCtx;
    InitPackContext(ctx, gridW, gridH, (int)items.size());
    // Reset Path Relinking elite pool every call — reuseCtx would otherwise
    // carry a prior pack's elites forward. Cheap because the vector usually
    // holds at most eliteCap entries (default 8).
    ctx.pathRelink.elites.clear();
    ctx.pathRelink.eliteScores.clear();
    ctx.skylineWasteCoef                    = effSkylineWasteCoef;
    ctx.grouping.tierWeightExact            = effTierWeightExact;
    ctx.grouping.tierWeightCustom           = effTierWeightCustom;
    ctx.grouping.tierWeightType             = effTierWeightType;
    ctx.grouping.tierWeightFunction         = effTierWeightFunction;
    ctx.grouping.tierWeightFlags            = effTierWeightFlags;
    ctx.grouping.funcSimFoodFoodRestricted  = effFuncSimFoodFoodRestricted;
    ctx.grouping.funcSimFirstaidRobotrepair = effFuncSimFirstaidRobotrepair;
    ctx.grouping.softGroupingPct            = effSoftGroupingPct;
    // Prebuild the PairWeight lookup table once per pack. Soft-track loops
    // inside ComputeGroupingBonus* will O(1) lookup instead of recomputing
    // per LAHC iter. Skipped when the soft track is off.
    if (effSoftGroupingPct > 0) BuildPairWeightMatrix(ctx, items);
    else ctx.grouping.pairWeightMatrixN = 0;

    // Pre-reservation scan (H > 1 only)
    // Reserve a W x target rectangle at the grid bottom and pack into the
    // L-shaped complement. Try widths from gridW down; accept the widest
    // where all items fit. Right-aligned (reserveX = gridW - W).
    std::vector<Item> order = items;
    SortItems(order);

    int bestReserveW = 0;
    int bestReserveX = 0;
    PROF_PHASE_BEGIN(preRes);
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
    PROF_PHASE_END(preRes, profPreReservation);

    // Aggressive H-skip: if upper-bound LER can't beat caller's threshold,
    // skip LAHC. Still run Pack() for coverage.
    if (skipLAHCIfAreaBelow > 0 && target > 1 && bestReserveW > 0)
    {
        int upperBound = bestReserveW * target;
        if (upperBound < skipLAHCIfAreaBelow)
        {
            Result quickResult = PackH(gridW, gridH, items, target, abortFlag, reuseCtx);
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
    PROF_PHASE_BEGIN(greedy);
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

    ctx.bssfPl = ctx.placements;

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
            r.placements    = ctx.bssfPl;
            if (outDiag) outDiag->packCalls = diagPackCalls;
            return r;
        }

        if (ctx.placements.size() < ctx.bssfPl.size()) ctx.placements = ctx.bssfPl;
    }
    else
    {
        ctx.placements = ctx.bssfPl;
    }

    ctx.seedPl = ctx.placements;
    PROF_PHASE_END(greedy, profGreedySeed);

    BuildOccupancyGrid(ctx, gridW, gridH);
    int seedLerA, seedLerW, seedLerH, seedLerX, seedLerY;
    ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, seedLerA, seedLerW, seedLerH, seedLerX, seedLerY);
    int seedStranded = 0;
    double seedConc =
        ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, seedLerX, seedLerY, seedLerW, seedLerH, seedStranded);
    int seedNumRot         = CountRotated(ctx.seedPl);
    long long seedGrouping = ComputeGroupingBonus(ctx.seedPl, items, ctx, effGroupingPower);
    long long seedScore    = ComputeScore(ctx.seedPl.size(), seedLerA, seedLerH, seedConc, target, seedNumRot,
                                          seedGrouping, seedStranded, effGroupingWeight, effFragWeight);

    diagGreedySeedScore   = seedScore;
    diagGreedySeedLerArea = seedLerA;

    // Best solution tracking
    ctx.bestPl          = ctx.seedPl;
    long long bestScore = seedScore;
    int bestLerA        = seedLerA;
    int bestLerW        = seedLerW;
    int bestLerH        = seedLerH;
    int bestLerX        = seedLerX;
    int bestLerY        = seedLerY;
    double bestConc     = seedConc;
    int bestStranded    = seedStranded;

    if (outBestOrder) *outBestOrder = order;

    // Deterministic seed: params->rngSeed wins if non-zero, else derive from (target, numItems).
    unsigned int seed = (params && params->rngSeed != 0)
                            ? params->rngSeed
                            : ((unsigned int)(target * 2654435761u) ^ (unsigned int)(items.size() * 40503u));
    LCG rng(seed);

    int effectiveRestarts  = effRestarts;
    int bestIterInRestart0 = -1;
    std::vector<long long> history(effHistLen);

    // Cached ctx.bestPl occupancy grid + LER reachability for repair moves.
    // Only rebuilt when ctx.bestPl changes (dirty flag).
    int totalCellsRepair = gridW * gridH;
    std::vector<unsigned char> repairGrid(totalCellsRepair);
    std::vector<unsigned char> repairReachable(totalCellsRepair);
    std::vector<int> repairStrandedList; // interior cells not reachable from LER
    bool repairGridDirty = true;

    // Outside the restart loop so copy-assign reuses capacity instead
    // of allocating per restart.
    std::vector<Item> curOrder;
    curOrder.reserve(items.size());

    for (int restart = 0; restart < effectiveRestarts; ++restart)
    {
        if (abortFlag && *abortFlag != 0) break;

        // Restart 0: cached seed or sorted order. 1+: group-shuffled.
        if (restart == 0 && seedOrder != NULL) curOrder = *seedOrder;
        else curOrder = order;
        if (restart > 0) ShuffleGroups(curOrder, rng);

#ifdef STACKSORT_PROFILE
        // 0 → skip prefix measurement for the restart-seed SkylinePack.
        ctx.prof.skylinePrefixK = 0;
#endif

        // Pack the restart seed
        SkylinePack(ctx, gridW, gridH, curOrder, target, abortFlag, bestReserveX, bestReserveW);
        ++diagPackCalls;
        if (abortFlag && *abortFlag != 0) break;
        // ctx.grid is maintained incrementally inside SkylinePack — no
        // BuildOccupancyGrid rebuild needed before the scorer reads it.

        int curLerA, curLerW, curLerH, curLerX, curLerY;
        int curStranded = 0;
        double curConc  = 0.0;
#ifdef STACKSORT_PROFILE
        bool curCacheHit =
            GridCacheLookup(ctx, gridW, gridH, curLerA, curLerW, curLerH, curLerX, curLerY, curConc, curStranded);
        ++diagGridHashProbes;
        if (curCacheHit) ++diagGridHashHits;
#else
        GridCacheLookup(ctx, gridW, gridH, curLerA, curLerW, curLerH, curLerX, curLerY, curConc, curStranded);
#endif
        int curNumRot         = CountRotated(ctx.placements);
        long long curGrouping = ComputeGroupingBonus(ctx.placements, items, ctx, effGroupingPower);
        long long curScore    = ComputeScore(ctx.placements.size(), curLerA, curLerH, curConc, target, curNumRot,
                                             curGrouping, curStranded, effGroupingWeight, effFragWeight);

        if (curScore > bestScore)
        {
            UpdateBestFromCurrent(ctx, bestScore, curScore, bestLerA, curLerA, bestLerW, curLerW, bestLerH, curLerH,
                                  bestLerX, curLerX, bestLerY, curLerY, bestConc, curConc, bestStranded, curStranded,
                                  repairGridDirty, outBestOrder, curOrder);
            if (restart == 0) bestIterInRestart0 = 0;
            diagBestFoundIter    = 0;
            diagBestFoundRestart = restart;

            if (enablePathRelinking)
                CapturePathRelinkElite(ctx, curOrder, items, bestScore, effPathRelinkEliteCap,
                                       (int)items.size() * effPathRelinkDiversityPct / 100);
        }

        // Fresh LAHC history per restart (vector allocated once above the loop)
        for (int i = 0; i < effHistLen; ++i)
            history[i] = curScore;

        int itersSinceImproved = 0;

        // Reset the grid cache per-restart. Cross-restart sharing is
        // rare because each restart perturbs from a different ordering.
        ctx.cache.count    = 0;
        ctx.cache.ringHead = 0;

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
                move.a    = SampleBiasedIndex(rng, n, effLateBiasAlphaQ, effLateBiasUniformPct);
                move.b    = rng.nextInt(n - 1);
                if (move.b >= move.a) ++move.b;
                std::swap(curOrder[move.a], curOrder[move.b]);
            }
            else if (moveRoll < effInsertMax)
            {
                // Insert move: remove at a, insert at b
                move.type = MOVE_INSERT;
                move.a    = SampleBiasedIndex(rng, n, effLateBiasAlphaQ, effLateBiasUniformPct);
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
                move.a                = SampleBiasedIndex(rng, n, effLateBiasAlphaQ, effLateBiasUniformPct);
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
                // Repair move (10%): see TryRepairMove in PackerRepairMove.cpp
                // for details. Finds a stranded cell in ctx.bestPl and
                // promotes a fitting item to position 0 in curOrder; falls
                // back to random swap if no repair target found.
                ++diagRepairMoveRolls;
                TryRepairMove(ctx, gridW, gridH, curOrder, n, move, rng, bestLerX, bestLerY, bestLerW, bestLerH,
                              bestStranded, repairGrid, repairReachable, repairStrandedList, repairGridDirty,
                              totalCellsRepair, effLateBiasAlphaQ, effLateBiasUniformPct, diagRepairMoveScans,
                              diagRepairMoveHits);
            }

            // Fraction of curOrder preserved by this move. REPAIR-hit sets
            // move.b=0, so min(a,b)=0 handles it without a special case.
            int keptPrefix = (move.type == MOVE_ROTATE) ? move.a : std::min(move.a, move.b);
#ifdef STACKSORT_PROFILE
            diagKeptPrefixSum += keptPrefix;
            ++diagKeptPrefixCount;
#endif

            int startIdx = 0;
            ++diagSkylineSnapProbes;
            if (ctx.skyline.snapValid && ctx.skyline.snapN == (int)curOrder.size() && keptPrefix > 0 &&
                keptPrefix < ctx.skyline.snapN)
            {
                RestoreSkylineState(ctx, gridW, gridH, keptPrefix);
                startIdx = keptPrefix;
                ++diagSkylineSnapHits;
            }
#ifdef STACKSORT_PROFILE
            // Skip prefix measurement on restore — the timer in SkylinePack
            // would fire immediately and record ~0 cycles otherwise.
            ctx.prof.skylinePrefixK = (startIdx > 0) ? 0 : keptPrefix;
#endif

            PROF_TICK(profMoveGen);

            // Pack with perturbed order
            SkylinePack(ctx, gridW, gridH, curOrder, target, abortFlag, bestReserveX, bestReserveW, startIdx);
            ++diagPackCalls;
            if (abortFlag && *abortFlag != 0) break;

            PROF_TICK(profSkyline);

            int candLerA, candLerW, candLerH, candLerX, candLerY;
            int candStranded = 0;
            double candConc  = 0.0;
#ifdef STACKSORT_PROFILE
            bool candCacheHit = GridCacheLookup(ctx, gridW, gridH, candLerA, candLerW, candLerH, candLerX, candLerY,
                                                candConc, candStranded);
            ++diagGridHashProbes;
            if (candCacheHit) ++diagGridHashHits;
#else
            GridCacheLookup(ctx, gridW, gridH, candLerA, candLerW, candLerH, candLerX, candLerY, candConc,
                            candStranded);
#endif
            // profConc's tick is preserved even though the cache attributes
            // all its work to profLer — skipping it would fold those cycles
            // into profGrouping instead.
            PROF_TICK(profLer);
            PROF_TICK(profConc);

            int candNumRot         = CountRotated(ctx.placements);
            long long candGrouping = ComputeGroupingBonus(ctx.placements, items, ctx, effGroupingPower);
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
                    UpdateBestFromCurrent(ctx, bestScore, candScore, bestLerA, candLerA, bestLerW, candLerW, bestLerH,
                                          candLerH, bestLerX, candLerX, bestLerY, candLerY, bestConc, candConc,
                                          bestStranded, candStranded, repairGridDirty, outBestOrder, curOrder);
                    itersSinceImproved = 0;
                    if (restart == 0) bestIterInRestart0 = iter;
                    diagBestFoundIter    = iter;
                    diagBestFoundRestart = restart;

                    if (enablePathRelinking)
                        CapturePathRelinkElite(ctx, curOrder, items, bestScore, effPathRelinkEliteCap,
                                               (int)items.size() * effPathRelinkDiversityPct / 100);
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
                // Undo leaves curOrder diverging from snap at the move's
                // positions; keptPrefix vs snap is no longer safe next iter.
                ctx.skyline.snapValid = false;
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

        // Per-restart elite capture for Path Relinking. After the LAHC inner
        // loop exits (plateau or iter exhaustion), the current curOrder +
        // curScore reflect this restart's latest accepted state — a local
        // optimum in the LAHC sense. Capturing here, in addition to the
        // global-best sites, guarantees the elite pool has up to R entries
        // even when only restart 0 ever beats the global best. Diversity
        // filter + cap inside CapturePathRelinkElite prevent duplicates.
        if (enablePathRelinking)
            CapturePathRelinkElite(ctx, curOrder, items, curScore, effPathRelinkEliteCap,
                                   (int)items.size() * effPathRelinkDiversityPct / 100);

        // Adaptive: fast-converging cold starts skip restarts 2-3.
        // Disabled when seeded (seeds bias toward local optima).
        if (enableFastConverge && restart == 0 && seedOrder == NULL)
        {
            if (bestIterInRestart0 >= 0 && bestIterInRestart0 < FAST_CONVERGE_ITER && bestConc > GOOD_CONC_THRESHOLD &&
                ctx.bestPl.size() == items.size())
            {
                effectiveRestarts = 2;
            }
        }
    }

    // Path Relinking: after all restarts, walk transposition paths between
    // captured elites. Each path intermediate re-packs via SkylinePack with
    // keptPrefix = leftmost swap position and is scored; any strict
    // improvement over the current bestScore commits via UpdateBestFromCurrent.
    // Off unless enablePathRelinking; also skipped when the pool has < 2
    // distinct elites (captures the FastConverge-collapsed case).
    PROF_PHASE_BEGIN(pathRelink);
    if (enablePathRelinking && ctx.pathRelink.elites.size() >= 2 && !(abortFlag && *abortFlag != 0))
    {
        int poolSize   = (int)ctx.pathRelink.elites.size();
        int maxPathLen = (effPathRelinkMaxPathLen > 0) ? effPathRelinkMaxPathLen : (int)items.size();
        // Scratch ordering mutated in place by PathRelinkWalk. Reserved once to
        // avoid reallocation per pair.
        std::vector<Item> prWorking;
        prWorking.reserve(items.size());
        for (int i = 0; i < poolSize; ++i)
        {
            for (int j = 0; j < poolSize; ++j)
            {
                if (i == j) continue;
                if (abortFlag && *abortFlag != 0) break;
                prWorking            = ctx.pathRelink.elites[i];
                long long endpointSc = ctx.pathRelink.eliteScores[i];
                PathRelinkWalk(ctx, gridW, gridH, prWorking, ctx.pathRelink.elites[j], items, target, abortFlag,
                               bestReserveX, bestReserveW, effGroupingWeight, effFragWeight, effGroupingPower,
                               bestScore, bestLerA, bestLerW, bestLerH, bestLerX, bestLerY, bestConc, bestStranded,
                               repairGridDirty, outBestOrder, endpointSc, maxPathLen, diagPathRelinkIntermediatesScored,
                               diagPathRelinkGlobalBestUpdates, diagPathRelinkAbortedPaths,
                               diagPathRelinkGlobalBestGainMax, diagPathRelinkAvgPathLenSum);
                ++diagPathRelinkPairsRun;
            }
            if (abortFlag && *abortFlag != 0) break;
        }
    }
    PROF_PHASE_END(pathRelink, profPathRelink);

    // Unconstrained fallback: items may naturally leave a good gap without
    // being forced into an L-shape. Keep whichever scores higher.
    PROF_PHASE_BEGIN(unc);
    if (enableUnconstrainedFallback && bestReserveW > 0 && !(abortFlag && *abortFlag != 0))
    {
        Result unconResult = PackH(gridW, gridH, items, target, abortFlag, reuseCtx);
        diagPackCalls += 2; // PackH = BSSF + BAF internally

        // Rescore under effective weights: PackH hardcodes defaults, bestScore doesn't.
        if (unconResult.allPlaced)
        {
            long long unGrouping      = ComputeGroupingBonus(unconResult.placements, items, ctx, effGroupingPower);
            unconResult.groupingBonus = unGrouping;
            unconResult.score = ComputeScore(unconResult.placements.size(), unconResult.lerArea, unconResult.lerHeight,
                                             unconResult.concentration, target, CountRotated(unconResult.placements),
                                             unGrouping, unconResult.strandedCells, effGroupingWeight, effFragWeight);
        }

        if (unconResult.allPlaced && unconResult.score > bestScore)
        {
            ctx.bestPl                   = unconResult.placements;
            bestLerA                     = unconResult.lerArea;
            bestLerW                     = unconResult.lerWidth;
            bestLerH                     = unconResult.lerHeight;
            bestLerX                     = unconResult.lerX;
            bestLerY                     = unconResult.lerY;
            bestConc                     = unconResult.concentration;
            bestStranded                 = unconResult.strandedCells;
            diagUnconstrainedFallbackWon = true;
        }
    }
    PROF_PHASE_END(unc, profUnconstrainedFallback);

    int diagStripShiftStripsFound       = 0;
    int diagStripShiftStripsImproved    = 0;
    int diagTileSwapCandidatesFound     = 0;
    int diagTileSwapCandidatesCommitted = 0;

    PROF_PHASE_BEGIN(stripShift);
    if (enableStripShift)
        StripShift(ctx.bestPl, items, ctx, gridW, gridH, effGroupingPower, &diagStripShiftStripsFound,
                   &diagStripShiftStripsImproved);
    PROF_PHASE_END(stripShift, profStripShift);

    PROF_PHASE_BEGIN(tileSwap);
    if (enableTileSwap)
        TileSwap(ctx.bestPl, items, ctx, gridW, gridH, effGroupingPower, &diagTileSwapCandidatesFound,
                 &diagTileSwapCandidatesCommitted);
    PROF_PHASE_END(tileSwap, profTileSwap);

    PROF_PHASE_BEGIN(optGrp);
    if (enableOptimizeGrouping) OptimizeGrouping(ctx.bestPl, items, ctx, effGroupingPower);
    PROF_PHASE_END(optGrp, profOptimizeGrouping);

    long long bestGroupingExact = 0;
    long long bestGrouping      = ComputeGroupingBonus(ctx.bestPl, items, ctx, effGroupingPower, &bestGroupingExact);

    // Rotated flags relative to original input dims
    for (size_t i = 0; i < ctx.bestPl.size(); ++i)
        ctx.bestPl[i].rotated = (ctx.bestPl[i].w != items[ctx.bestPl[i].id].w);
    int bestNumRot = CountRotated(ctx.bestPl);

    Result result;
    result.placements    = ctx.bestPl;
    result.lerArea       = bestLerA;
    result.lerWidth      = bestLerW;
    result.lerHeight     = bestLerH;
    result.lerX          = bestLerX;
    result.lerY          = bestLerY;
    result.concentration = bestConc;
    result.strandedCells = bestStranded;
    result.groupingBonus = bestGrouping;
    result.allPlaced     = (ctx.bestPl.size() == items.size());
    result.score = ComputeScore(ctx.bestPl.size(), bestLerA, bestLerH, bestConc, target, bestNumRot, bestGrouping,
                                bestStranded, effGroupingWeight, effFragWeight);

    if (outDiag)
    {
        outDiag->packCalls                   = diagPackCalls;
        outDiag->plateauBreaks               = diagPlateauBreaks;
        outDiag->lahcItersExecuted           = diagLahcItersExecuted;
        outDiag->bestFoundIter               = diagBestFoundIter;
        outDiag->bestFoundRestart            = diagBestFoundRestart;
        outDiag->unconstrainedFallbackWon    = diagUnconstrainedFallbackWon;
        outDiag->greedySeedScore             = diagGreedySeedScore;
        outDiag->greedySeedLerArea           = diagGreedySeedLerArea;
        outDiag->repairMoveRolls             = diagRepairMoveRolls;
        outDiag->repairMoveScans             = diagRepairMoveScans;
        outDiag->repairMoveHits              = diagRepairMoveHits;
        outDiag->repairMoveAccepts           = diagRepairMoveAccepts;
        outDiag->stripShiftStripsFound       = diagStripShiftStripsFound;
        outDiag->stripShiftStripsImproved    = diagStripShiftStripsImproved;
        outDiag->tileSwapCandidatesFound     = diagTileSwapCandidatesFound;
        outDiag->tileSwapCandidatesCommitted = diagTileSwapCandidatesCommitted;

        outDiag->pathRelinkPairsRun            = diagPathRelinkPairsRun;
        outDiag->pathRelinkIntermediatesScored = diagPathRelinkIntermediatesScored;
        outDiag->pathRelinkGlobalBestUpdates   = diagPathRelinkGlobalBestUpdates;
        outDiag->pathRelinkAbortedPaths        = diagPathRelinkAbortedPaths;
        outDiag->pathRelinkAvgPathLenSum       = diagPathRelinkAvgPathLenSum;
        outDiag->pathRelinkGlobalBestGainMax   = diagPathRelinkGlobalBestGainMax;
        outDiag->skylineSnapHits               = diagSkylineSnapHits;
        outDiag->skylineSnapProbes             = diagSkylineSnapProbes;
        // Power-independent clustering metric for cross-power CSV/analysis.
        // Computed once per final result, not per LAHC iter.
        PROF_PHASE_BEGIN(bordersRaw);
        outDiag->groupingBordersRaw = ComputeGroupingBordersRaw(ctx.bestPl, items, ctx);
        outDiag->groupingBonusExact = bestGroupingExact;
        PROF_PHASE_END(bordersRaw, profBordersRaw);
#ifdef STACKSORT_PROFILE
        outDiag->profCyclesMoveGen               = (long long)profMoveGen;
        outDiag->profCyclesSkylinePack           = (long long)profSkyline;
        outDiag->profCyclesLer                   = (long long)profLer;
        outDiag->profCyclesConcentration         = (long long)profConc;
        outDiag->profCyclesGrouping              = (long long)profGrouping;
        outDiag->profCyclesStranded              = (long long)profStranded;
        outDiag->profCyclesScore                 = (long long)profScore;
        outDiag->profCyclesAccept                = (long long)profAccept;
        outDiag->profCyclesPreReservation        = (long long)profPreReservation;
        outDiag->profCyclesGreedySeed            = (long long)profGreedySeed;
        outDiag->profCyclesUnconstrainedFallback = (long long)profUnconstrainedFallback;
        outDiag->profCyclesOptimizeGrouping      = (long long)profOptimizeGrouping;
        outDiag->profCyclesStripShift            = (long long)profStripShift;
        outDiag->profCyclesTileSwap              = (long long)profTileSwap;
        outDiag->profCyclesPathRelink            = (long long)profPathRelink;
        outDiag->profCyclesBordersRaw            = (long long)profBordersRaw;
        outDiag->keptPrefixSum                   = diagKeptPrefixSum;
        outDiag->keptPrefixCount                 = diagKeptPrefixCount;
        outDiag->gridHashProbes                  = diagGridHashProbes;
        outDiag->gridHashHits                    = diagGridHashHits;
        outDiag->profCyclesSkylinePrefix         = ctx.prof.skylinePrefixCycles;
        outDiag->profCyclesSkylineWasteMap       = ctx.prof.cyclesSkylineWasteMap;
        outDiag->profCyclesSkylineCandidate      = ctx.prof.cyclesSkylineCandidate;
        outDiag->profCyclesSkylineAdjacency      = ctx.prof.cyclesSkylineAdjacency;
        outDiag->profCyclesSkylineCommit         = ctx.prof.cyclesSkylineCommit;
        outDiag->profCyclesLerHistogram          = ctx.prof.cyclesLerHistogram;
        outDiag->profCyclesLerStack              = ctx.prof.cyclesLerStack;
#endif
    }
    return result;
}
