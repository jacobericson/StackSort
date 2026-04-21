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

// One-shot rdtsc timers for per-run phases (pre-reservation scan, greedy
// seed, unconstrained fallback, OptimizeGrouping, BordersRaw). Each phase
// gets a uniquely-named snapshot variable so the BEGIN/END pairs can be
// nested or sequenced without collision.
#ifdef STACKSORT_PROFILE
#define PROF_PHASE_BEGIN(tag) unsigned long long _profPhase_##tag = __rdtsc()
#define PROF_PHASE_END(tag, acc)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        acc += __rdtsc() - _profPhase_##tag;                                                                           \
    } while (0)
#else
#define PROF_PHASE_BEGIN(tag) ((void)0)
#define PROF_PHASE_END(tag, acc) ((void)0)
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

// max-of-K uniform draws: CDF = (x/n)^K biases toward tail.
// K = alphaQ/4 so alphaQ=8 → K=2 (linear bias), alphaQ=12 → K=3 (quadratic).
static int SampleBiasedIndex(LCG& rng, int n, int alphaQ, int uniformPct)
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

static const int NUM_RESTARTS      = 16;
static const int ITERS_PER_RESTART = 4000; // 16 * 4000 = 64000 total (plateau-limited in practice)
static const int LAHC_HISTORY_LEN  = 200;
static const int PLATEAU_THRESHOLD = 1500; // break restart after this many iters w/o improvement
static const int MIN_RESERVE_W     = 1;    // minimum LER width for pre-reservation scan

// Move-type dispatch thresholds on a 0..99 roll. Cascading: the first
// MOVE_*_MAX that roll < passes selects the move. Repair has no MAX
// constant because it's the residual bucket (roll >= MOVE_ROTATE_MAX).
static const int MOVE_SWAP_MAX   = 50; // 0-49  = swap
static const int MOVE_INSERT_MAX = 75; // 50-74 = insert
static const int MOVE_ROTATE_MAX = 90; // 75-89 = rotate-flip, 90-99 = repair

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
    std::vector<Packer::Item> reordered;
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
static void RestoreSkylineState(Packer::PackContext& ctx, int gridW, int /*gridH*/, int keptPrefix)
{
    const Packer::SkylineBoundary& b = ctx.skylineSnapBoundaries[(size_t)keptPrefix];

    // Roll back ctx.grid + placementIdGrid for every placement being
    // discarded. Walking placements directly is equivalent to the old
    // per-cell gridDelta log and avoids the per-cell push_back cost in
    // EmitBoundary's hot path.
    for (size_t i = (size_t)b.placementsCount; i < ctx.placements.size(); ++i)
    {
        const Packer::Placement& p = ctx.placements[i];
        for (int dy = 0; dy < p.h; ++dy)
            for (int dx = 0; dx < p.w; ++dx)
            {
                int cellIdx                  = (p.y + dy) * gridW + (p.x + dx);
                ctx.grid[cellIdx]            = 0;
                ctx.placementIdGrid[cellIdx] = -1;
            }
    }

    ctx.placements.resize((size_t)b.placementsCount);

    ctx.wasteRects.reserve((size_t)b.wasteCount);
    ctx.wasteRects.assign(ctx.skylineSnapWaste.begin() + b.wasteStart,
                          ctx.skylineSnapWaste.begin() + b.wasteStart + b.wasteCount);

    // Rebuild the skyline linked-list arena from the saved linear slice.
    ctx.skylineHead     = -1;
    ctx.skylineFreeHead = -1;
    ctx.skylineCount    = 0;
    short tail          = -1;
    for (int i = 0; i < b.skylineCount; ++i)
    {
        short idx             = ctx.skylineCount++;
        ctx.skylineNodes[idx] = ctx.skylineSnapSkyline[(size_t)b.skylineStart + (size_t)i];
        ctx.skylineNext[idx]  = -1;
        if (tail < 0) ctx.skylineHead = idx;
        else ctx.skylineNext[tail] = idx;
        tail = idx;
    }

    ctx.curHashA = b.hashA;
    ctx.curHashB = b.hashB;

    // SkylinePack's emit-on-placement push_backs must land at boundary k+1;
    // stale entries past boundary[keptPrefix] would otherwise offset them.
    ctx.skylineSnapBoundaries.resize((size_t)keptPrefix + 1);
    ctx.skylineSnapWaste.resize((size_t)b.wasteStart + (size_t)b.wasteCount);
    ctx.skylineSnapSkyline.resize((size_t)b.skylineStart + (size_t)b.skylineCount);
}

// Shared best-scalar update for the two LAHC sites and the Path Relinking
// site. Keeps bestScore / ctx.bestPl / best LER tuple / bestConc /
// bestStranded / repairGridDirty / *outBestOrder in lockstep. Call-site-
// specific counters (itersSinceImproved, bestIterInRestart0, diagBestFound*)
// stay inline at the LAHC sites.
static void UpdateBestFromCurrent(Packer::PackContext& ctx, long long& bestScore, long long newScore, int& bestLerA,
                                  int newLerA, int& bestLerW, int newLerW, int& bestLerH, int newLerH, int& bestLerX,
                                  int newLerX, int& bestLerY, int newLerY, double& bestConc, double newConc,
                                  int& bestStranded, int newStranded, bool& repairGridDirty,
                                  std::vector<Packer::Item>* outBestOrder, const std::vector<Packer::Item>& curOrder)
{
    bestScore       = newScore;
    ctx.bestPl      = ctx.placements;
    bestLerA        = newLerA;
    bestLerW        = newLerW;
    bestLerH        = newLerH;
    bestLerX        = newLerX;
    bestLerY        = newLerY;
    bestConc        = newConc;
    bestStranded    = newStranded;
    repairGridDirty = true;
    if (outBestOrder) *outBestOrder = curOrder;
}

// Count positions where two orderings' exactIds disagree. Used as the
// diversity metric for Path Relinking elite admission — raw id-Hamming
// would false-reject pairs that differ only by swapping same-exactId
// siblings, which are packing-equivalent.
static int PathRelinkExactIdHamming(const std::vector<Packer::Item>& a, const std::vector<Packer::Item>& b)
{
    int d    = 0;
    size_t n = a.size();
    if (b.size() != n) return (int)n;
    for (size_t i = 0; i < n; ++i)
        if (a[i].exactId != b[i].exactId) ++d;
    return d;
}

// Capture curOrder into the elite pool. Two safety nets:
//   - Normalize w/h/canRotate from originalItems so MOVE_ROTATE's in-place
//     mutation of curOrder entries doesn't produce elites whose geometry
//     diverges from their id.
//   - Reject near-duplicates via exactId-Hamming < diversityThreshold.
// Pool policy: append until cap, then replace weakest-scoring entry (only
// if the new score is strictly higher).
static void CapturePathRelinkElite(Packer::PackContext& ctx, const std::vector<Packer::Item>& curOrder,
                                   const std::vector<Packer::Item>& originalItems, long long score, int eliteCap,
                                   int diversityThreshold)
{
    std::vector<Packer::Item> normalized(curOrder);
    for (size_t i = 0; i < normalized.size(); ++i)
    {
        int id = normalized[i].id;
        if (id >= 0 && id < (int)originalItems.size())
        {
            normalized[i].w         = originalItems[id].w;
            normalized[i].h         = originalItems[id].h;
            normalized[i].canRotate = originalItems[id].canRotate;
        }
    }

    for (size_t i = 0; i < ctx.pathRelinkElites.size(); ++i)
    {
        if (PathRelinkExactIdHamming(normalized, ctx.pathRelinkElites[i]) < diversityThreshold) return;
    }

    if ((int)ctx.pathRelinkElites.size() < eliteCap)
    {
        ctx.pathRelinkElites.push_back(normalized);
        ctx.pathRelinkEliteScores.push_back(score);
        return;
    }

    size_t weakest      = 0;
    long long weakestSc = ctx.pathRelinkEliteScores[0];
    for (size_t i = 1; i < ctx.pathRelinkEliteScores.size(); ++i)
    {
        if (ctx.pathRelinkEliteScores[i] < weakestSc)
        {
            weakest   = i;
            weakestSc = ctx.pathRelinkEliteScores[i];
        }
    }
    if (score > weakestSc)
    {
        ctx.pathRelinkElites[weakest]      = normalized;
        ctx.pathRelinkEliteScores[weakest] = score;
    }
}

// Walk a transposition path from s toward goalOrder, scoring every
// intermediate. Any sc > bestScore commits as the new global best. Starts
// with a cold SkylinePack to establish the snapshot log, then each
// transposition reuses keptPrefix = leftmost swap position. Snapshot gate
// mirrors the LAHC loop's so PR degrades gracefully when the restore is
// unsafe (full cold re-pack instead).
bool Packer::PathRelinkWalk(PackContext& ctx, int gridW, int gridH, std::vector<Item>& s,
                            const std::vector<Item>& goalOrder, const std::vector<Item>& originalItems, int target,
                            const volatile long* abortFlag, int bestReserveX, int bestReserveW, int effGroupingWeight,
                            int effFragWeight, int effGroupingPower, long long& bestScore, int& bestLerA, int& bestLerW,
                            int& bestLerH, int& bestLerX, int& bestLerY, double& bestConc, int& bestStranded,
                            bool& repairGridDirty, std::vector<Item>* outBestOrder, long long endpointScore,
                            int maxPathLen, int& diagIntermediatesScored, int& diagBestUpdates, int& diagAbortedPaths,
                            long long& diagGainMax, long long& diagAvgPathLenSum)
{
    int n = (int)s.size();
    if (n != (int)goalOrder.size()) return false;

    ctx.skylineSnapValid = false;
    SkylinePack(ctx, gridW, gridH, s, target, abortFlag, bestReserveX, bestReserveW);
    if (abortFlag && *abortFlag != 0) return false;
    if (!ctx.skylineSnapValid || ctx.skylineSnapN != n)
    {
        ++diagAbortedPaths;
        return false;
    }

    bool improved = false;
    int steps     = 0;
    int p         = 0;
    while (p < n && steps < maxPathLen)
    {
        if (abortFlag && *abortFlag != 0) break;
        if (s[p].id == goalOrder[p].id)
        {
            ++p;
            continue;
        }

        int q = -1;
        for (int k = p + 1; k < n; ++k)
        {
            if (s[k].id == goalOrder[p].id)
            {
                q = k;
                break;
            }
        }
        if (q < 0)
        {
            ++diagAbortedPaths;
            break;
        }

        std::swap(s[p], s[q]);
        int keptPrefix = p;
        ++steps;

        bool canRestore =
            ctx.skylineSnapValid && ctx.skylineSnapN == n && keptPrefix > 0 && keptPrefix < ctx.skylineSnapN;
        int startIdx = canRestore ? keptPrefix : 0;
        if (canRestore) RestoreSkylineState(ctx, gridW, gridH, keptPrefix);
        SkylinePack(ctx, gridW, gridH, s, target, abortFlag, bestReserveX, bestReserveW, startIdx);
        if (abortFlag && *abortFlag != 0) break;
        ++diagIntermediatesScored;

        int lerA, lerW, lerH, lerX, lerY, stranded = 0;
        double conc = 0.0;
        GridCacheLookup(ctx, gridW, gridH, lerA, lerW, lerH, lerX, lerY, conc, stranded);
        long long grp = ComputeGroupingBonus(ctx.placements, originalItems, ctx, effGroupingPower);
        long long sc  = ComputeScore(ctx.placements.size(), lerA, lerH, conc, target, CountRotated(ctx.placements), grp,
                                     stranded, effGroupingWeight, effFragWeight);

        if (sc > bestScore)
        {
            long long gain = sc - endpointScore;
            diagGainMax    = std::max(gain, diagGainMax);
            UpdateBestFromCurrent(ctx, bestScore, sc, bestLerA, lerA, bestLerW, lerW, bestLerH, lerH, bestLerX, lerX,
                                  bestLerY, lerY, bestConc, conc, bestStranded, stranded, repairGridDirty, outBestOrder,
                                  s);
            ++diagBestUpdates;
            improved = true;
        }
    }

    diagAvgPathLenSum += steps;
    return improved;
}

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
    ctx.pathRelinkElites.clear();
    ctx.pathRelinkEliteScores.clear();
    ctx.skylineWasteCoef           = effSkylineWasteCoef;
    ctx.tierWeightExact            = effTierWeightExact;
    ctx.tierWeightCustom           = effTierWeightCustom;
    ctx.tierWeightType             = effTierWeightType;
    ctx.tierWeightFunction         = effTierWeightFunction;
    ctx.tierWeightFlags            = effTierWeightFlags;
    ctx.funcSimFoodFoodRestricted  = effFuncSimFoodFoodRestricted;
    ctx.funcSimFirstaidRobotrepair = effFuncSimFirstaidRobotrepair;
    ctx.softGroupingPct            = effSoftGroupingPct;
    // Prebuild the PairWeight lookup table once per pack. Soft-track loops
    // inside ComputeGroupingBonus* will O(1) lookup instead of recomputing
    // per LAHC iter. Skipped when the soft track is off.
    if (effSoftGroupingPct > 0) BuildPairWeightMatrix(ctx, items);
    else ctx.pairWeightMatrixN = 0;

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
        ctx.profSkylinePrefixK = 0;
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
        ctx.gridCacheCount = 0;
        ctx.gridCacheHead  = 0;

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
                // Repair move (10%): find a stranded cell in the current best
                // packing, find an item whose dims could fill it, move that
                // item to position 0 in the ordering (highest packing priority).
                // Falls back to random swap if no repair target found.
                ++diagRepairMoveRolls;
                move.type        = MOVE_REPAIR;
                bool repairFound = false;

                if (bestStranded > 0 && !ctx.bestPl.empty())
                {
                    ++diagRepairMoveScans;
                    // Rebuild ctx.bestPl occupancy + LER reachability only when
                    // ctx.bestPl changed. Cache hit: memcpy 400 bytes vs full
                    // rebuild + flood fill (~2-4us). ctx.grid is kept
                    // incrementally current by SkylinePack for the *current*
                    // ordering, so we build bestPl occupancy into repairGrid
                    // directly rather than clobbering ctx.grid.
                    if (repairGridDirty)
                    {
                        memset(&repairGrid[0], 0, totalCellsRepair);
                        for (size_t pi = 0; pi < ctx.bestPl.size(); ++pi)
                        {
                            const Placement& bp = ctx.bestPl[pi];
                            for (int dy = 0; dy < bp.h; ++dy)
                                memset(&repairGrid[(bp.y + dy) * gridW + bp.x], 1, bp.w);
                        }
                        FloodFillFromLer(ctx, gridW, gridH, bestLerX, bestLerY, bestLerW, bestLerH, &repairGrid[0]);
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
                                if (repairGrid[si] == 0 && !ctx.visited[si]) repairStrandedList.push_back(si);
                            }
                        }
                        repairGridDirty = false;
                    }
                    else
                    {
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
                            minGX  = std::min(minGX, cx);
                            maxGX  = std::max(maxGX, cx);
                            minGY  = std::min(minGY, cy);
                            maxGY  = std::max(maxGY, cy);

                            if (cx > 0 && !ctx.visited[ci - 1] && !repairGrid[ci - 1])
                            {
                                ctx.visited[ci - 1] = 1;
                                ctx.floodStack.push_back(ci - 1);
                            }
                            if (cx < gridW - 1 && !ctx.visited[ci + 1] && !repairGrid[ci + 1])
                            {
                                ctx.visited[ci + 1] = 1;
                                ctx.floodStack.push_back(ci + 1);
                            }
                            if (cy > 0 && !ctx.visited[ci - gridW] && !repairGrid[ci - gridW])
                            {
                                ctx.visited[ci - gridW] = 1;
                                ctx.floodStack.push_back(ci - gridW);
                            }
                            if (cy < gridH - 1 && !ctx.visited[ci + gridW] && !repairGrid[ci + gridW])
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
                            int typeId = curOrder[ki].exactId;
                            if (typeId < 0 || typeId >= n) continue;
                            if (firstIdx[typeId] < 0) firstIdx[typeId] = ki;
                            ++groupCount[typeId];
                        }

                        for (int ki = 0; ki < n; ++ki)
                        {
                            // Skip first item of a multi-item type group —
                            // it anchors contact-point clustering for its type.
                            int typeId = curOrder[ki].exactId;
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
                    move.a    = SampleBiasedIndex(rng, n, effLateBiasAlphaQ, effLateBiasUniformPct);
                    move.b    = rng.nextInt(n - 1);
                    if (move.b >= move.a) ++move.b;
                    std::swap(curOrder[move.a], curOrder[move.b]);
                }
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
            if (ctx.skylineSnapValid && ctx.skylineSnapN == (int)curOrder.size() && keptPrefix > 0 &&
                keptPrefix < ctx.skylineSnapN)
            {
                RestoreSkylineState(ctx, gridW, gridH, keptPrefix);
                startIdx = keptPrefix;
                ++diagSkylineSnapHits;
            }
#ifdef STACKSORT_PROFILE
            // Skip prefix measurement on restore — the timer in SkylinePack
            // would fire immediately and record ~0 cycles otherwise.
            ctx.profSkylinePrefixK = (startIdx > 0) ? 0 : keptPrefix;
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
                ctx.skylineSnapValid = false;
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
    if (enablePathRelinking && ctx.pathRelinkElites.size() >= 2 && !(abortFlag && *abortFlag != 0))
    {
        int poolSize   = (int)ctx.pathRelinkElites.size();
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
                prWorking            = ctx.pathRelinkElites[i];
                long long endpointSc = ctx.pathRelinkEliteScores[i];
                PathRelinkWalk(ctx, gridW, gridH, prWorking, ctx.pathRelinkElites[j], items, target, abortFlag,
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
        outDiag->profCyclesSkylinePrefix         = ctx.profSkylinePrefixCycles;
        outDiag->profCyclesSkylineWasteMap       = ctx.profCyclesSkylineWasteMap;
        outDiag->profCyclesSkylineCandidate      = ctx.profCyclesSkylineCandidate;
        outDiag->profCyclesSkylineAdjacency      = ctx.profCyclesSkylineAdjacency;
        outDiag->profCyclesSkylineCommit         = ctx.profCyclesSkylineCommit;
        outDiag->profCyclesLerHistogram          = ctx.profCyclesLerHistogram;
        outDiag->profCyclesLerStack              = ctx.profCyclesLerStack;
#endif
    }
    return result;
}
