#pragma once

#include <vector>

class Packer
{
  public:
    // Target dimension for the free-space constraint.
    // TARGET_H: lerHeight >= target (reserve rows at bottom).
    // TARGET_W: lerWidth  >= target (reserve columns on right, internally
    //          implemented by transposing into H-mode).
    enum TargetDim
    {
        TARGET_H = 0,
        TARGET_W = 1
    };

    struct Item
    {
        int id;
        int w;
        int h;
        bool canRotate; // packer may try (h, w) orientation
        int itemTypeId; // items with same typeId are grouped during scoring
    };

    struct Placement
    {
        int id;
        int x;
        int y;
        int w;
        int h;
        bool rotated; // true if packer swapped w/h relative to input
        // CollectAdjacentPids cannot use items[p.id].itemTypeId for the
        // type compare: SkylinePack receives a permuted `items` vector
        // (curOrder), so items[p.id] is the item at position p.id in the
        // current ordering, not the item with id == p.id. Carry the type
        // in Placement instead.
        int itemTypeId;
    };

    struct Result
    {
        std::vector<Placement> placements;
        int lerArea;
        int lerWidth;
        int lerHeight;
        int lerX; // top-left corner of LER
        int lerY;
        long long score;
        double concentration;    // HHI of free-space regions (1.0 = one blob, 0 = scattered)
        int strandedCells;       // interior empty cells not in LER (unreachable waste)
        long long groupingBonus; // per-component adjacency reward (b^(quarters/4) scaling)
        bool allPlaced;
    };

    // Run the full packing pipeline: sort, MAXRECTS place, compute LER, score.
    // If abortFlag is non-NULL, checked per-item; returns partial result on abort.
    // W-mode transposes internally, so placements/LER come back in original space.
    struct PackContext; // forward decl; full definition below

    static Result Pack(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
                       const volatile long* abortFlag = NULL, PackContext* reuseCtx = NULL);

    // Compute the Largest Empty Rectangle on an occupancy grid.
    // grid is W*H unsigned chars, 0 = empty, nonzero = occupied.
    // Exposed publicly for before/after comparison.
    static void ComputeLER(const std::vector<unsigned char>& grid, int gridW, int gridH, int& outArea, int& outWidth,
                           int& outHeight, int& outX, int& outY);

    // Check that no cell is doubly-occupied and all coords are in bounds.
    static bool ValidatePlacements(int gridW, int gridH, const std::vector<Placement>& placements);

    // Scoring weight defaults. Mirror the values used by ComputeScore when
    // no SearchParams override is provided. Public so the harness can reference
    // them when resolving sentinel fields into CSV output.
    static const int DEFAULT_FRAG_WEIGHT     = 50;
    static const int DEFAULT_GROUPING_WEIGHT = 1;

    // Skyline tiebreaker default: combined = waste * coef - contact.
    // Higher coef = more waste-averse (less aggressive contact clustering).
    static const int DEFAULT_SKYLINE_WASTE_COEF = 3;

    // Grouping power exponent in quarter-steps: b^(quarters/4).
    // 6 = b^1.5 (matches legacy formula via fast-path), 7 = b^1.75, 8 = b^2.
    static const int DEFAULT_GROUPING_POWER_QUARTERS = 6;

    // Tunable LAHC search parameters. NULL = use compiled defaults.
    // Default constructor leaves all fields at sentinels so callers can
    // default-construct and override only the fields they care about.
    // Int fields: <= 0 means "use compiled default". Enable flags: -1 default,
    // 0 force off, 1 force on. rngSeed: 0 means deterministic derivation.
    struct SearchParams
    {
        int numRestarts;
        int itersPerRestart;
        int lahcHistoryLen;
        int plateauThreshold;

        unsigned int rngSeed;

        int enableBafSeed;
        int enableUnconstrainedFallback;
        int enableOptimizeGrouping;
        int enableFastConverge;
        int enableRepairMove;
        int enablePreReservation;

        // Move-type thresholds on a 0..100 roll. Must satisfy
        // moveSwapMax <= moveInsertMax <= moveRotateMax <= 100. -1 = default.
        int moveSwapMax;
        int moveInsertMax;
        int moveRotateMax;

        // Scoring weights. -1 = use DEFAULT_* compile-time constants.
        int scoringGroupingWeight;
        int scoringFragWeight;

        // Grouping power exponent in quarter-steps. -1 = default 6 (b^1.5).
        // Valid range [1, 8]; out-of-range falls back to default.
        int groupingPowerQuarters;

        // Skyline tiebreaker waste coefficient. -1 = default 3.
        // Resolver enforces >= 1 (0 would chase contact at the cost of waste).
        int skylineWasteCoef;

        SearchParams()
            : numRestarts(-1), itersPerRestart(-1), lahcHistoryLen(-1), plateauThreshold(-1), rngSeed(0),
              enableBafSeed(-1), enableUnconstrainedFallback(-1), enableOptimizeGrouping(-1), enableFastConverge(-1),
              enableRepairMove(-1), enablePreReservation(-1), moveSwapMax(-1), moveInsertMax(-1), moveRotateMax(-1),
              scoringGroupingWeight(-1), scoringFragWeight(-1), groupingPowerQuarters(-1), skylineWasteCoef(-1)
        {
        }

        // Equivalent to default construction; explicit form for readability
        // at caller sites that override a subset of fields.
        static SearchParams defaults()
        {
            return SearchParams();
        }
    };

    // Diagnostic counters filled by PackAnnealed when outDiag is non-NULL.
    // Kept separate from Result so the worker thread's cached results[] stays lean.
    struct PackDiagnostics
    {
        int bestFoundIter;
        int bestFoundRestart;
        int plateauBreaks;
        int lahcItersExecuted;
        int packCalls;
        bool unconstrainedFallbackWon;
        long long greedySeedScore;
        int greedySeedLerArea;

        // rolls = repair bucket selected on the iter dispatch roll.
        // scans = stranded-cell scan actually ran (gated on bestStranded>0).
        // hits = scan found a fitting item and moved it to position 0.
        // accepts = LAHC accepted the repair candidate.
        int repairMoveRolls;
        int repairMoveScans;
        int repairMoveHits;
        int repairMoveAccepts;

        // Power-independent clustering metric: Σ b per same-type connected
        // component on the final placement (no power exponent applied). The
        // harness uses this for cross-power heatmap comparisons since
        // groupingBonus's scale changes with groupingPowerQuarters.
        long long groupingBordersRaw;

        int skylineSnapHits;
        int skylineSnapProbes;

#ifdef STACKSORT_PROFILE
        // Per-phase rdtsc cycle accumulators across the LAHC inner loop.
        // Only populated in profiling builds; absent in production to keep
        // the struct small and the inner loop instrumentation-free.
        long long profCyclesMoveGen;
        long long profCyclesSkylinePack;
        long long profCyclesLer;
        long long profCyclesConcentration;
        long long profCyclesGrouping;
        long long profCyclesStranded;
        long long profCyclesScore;
        long long profCyclesAccept;

        // Once-per-run phases outside the LAHC inner loop. Useful for
        // attributing total wall time on cold starts + deciding where
        // optimization effort pays off. All populated in profile builds
        // regardless of whether the phase fires (zero when skipped).
        long long profCyclesPreReservation;        // reserve-width probe loop
        long long profCyclesGreedySeed;            // BSSF + optional BAF + selection
        long long profCyclesUnconstrainedFallback; // optional fallback PackH + rescore
        long long profCyclesOptimizeGrouping;      // post-LAHC same-footprint swap
        long long profCyclesBordersRaw;            // final cross-power clustering metric

        // Sum/count instead of pre-computed rate so the harness can
        // aggregate across runs without double-averaging.
        long long keptPrefixSum;
        int keptPrefixCount;
        int gridHashProbes;
        int gridHashHits;

        // Skyline cycles spent on items [0..keptPrefix-1] — the slice a
        // partial-recompute scheme could skip. Upper bound on Phase 3
        // savings.
        long long profCyclesSkylinePrefix;
#endif

        PackDiagnostics()
            : bestFoundIter(0), bestFoundRestart(0), plateauBreaks(0), lahcItersExecuted(0), packCalls(0),
              unconstrainedFallbackWon(false), greedySeedScore(0), greedySeedLerArea(0), repairMoveRolls(0),
              repairMoveScans(0), repairMoveHits(0), repairMoveAccepts(0), groupingBordersRaw(0), skylineSnapHits(0),
              skylineSnapProbes(0)
#ifdef STACKSORT_PROFILE
              ,
              profCyclesMoveGen(0), profCyclesSkylinePack(0), profCyclesLer(0), profCyclesConcentration(0),
              profCyclesGrouping(0), profCyclesStranded(0), profCyclesScore(0), profCyclesAccept(0),
              profCyclesPreReservation(0), profCyclesGreedySeed(0), profCyclesUnconstrainedFallback(0),
              profCyclesOptimizeGrouping(0), profCyclesBordersRaw(0), keptPrefixSum(0), keptPrefixCount(0),
              gridHashProbes(0), gridHashHits(0), profCyclesSkylinePrefix(0)
#endif
        {
        }
    };

    // Greedy seed + LAHC (Late Acceptance Hill Climbing). Used by worker thread.
    // seedOrder: if non-NULL, use as restart 0's initial ordering (warm start).
    // outBestOrder: if non-NULL, receives the ordering that produced the best result.
    // skipLAHCIfAreaBelow: if > 0 and pre-reservation upperBound < this, skip LAHC.
    // params: if non-NULL, overrides LAHC constants and ablation flags.
    // outDiag: if non-NULL, receives diagnostic counters for the harness/tuner.
    // W-mode transposes internally.
    static Result PackAnnealed(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
                               const volatile long* abortFlag = NULL, const std::vector<Item>* seedOrder = NULL,
                               std::vector<Item>* outBestOrder = NULL, int skipLAHCIfAreaBelow = 0,
                               const SearchParams* params = NULL, PackDiagnostics* outDiag = NULL,
                               PackContext* reuseCtx = NULL);

    // Scratch-buffer support types. Public only so callers can declare a
    // PackContext for reuseCtx — treat members as opaque.
    struct Rect
    {
        int x;
        int y;
        int w;
        int h;
    };

    struct SkylineNode
    {
        int x;
        int y; // height of this segment (0 = bottom of grid)
        int width;
    };

    struct LEREntry
    {
        int startIdx;
        int height;
    };

    struct GridCacheEntry
    {
        // Twin 64-bit hash defeats birthday collisions that a single
        // 64-bit would see ~1% over a full corpus run — required for
        // bit-exact determinism across cache hits vs misses.
        unsigned long long hashA;
        unsigned long long hashB;
        int lerArea;
        int lerWidth;
        int lerHeight;
        int lerX;
        int lerY;
        double concentration;
        int strandedCells;
    };

    // Entry[k] = state AFTER items[0..k-1] have been processed. placementsCount
    // may be < k when some of those items were unfittable — the restore path
    // must use placementsCount, not k, to resize ctx.placements.
    struct SkylineBoundary
    {
        int placementsCount;
        int wasteStart;
        int wasteCount;
        int skylineStart;
        int skylineCount;
        int gridDeltaStart;
        int gridDeltaCount;
    };

    // Reusable scratch buffers for the packing hot path. Construct once
    // per job; pass via Pack/PackAnnealed's reuseCtx to amortize
    // allocations. Not thread-safe — each worker needs its own.
    struct PackContext
    {
        std::vector<Rect> freeRects;
        std::vector<Rect> newRects;              // SplitFreeRects scratch
        std::vector<bool> dead;                  // PruneFreeRects scratch
        std::vector<Placement> placements;       // packing output
        std::vector<unsigned char> grid;         // BuildOccupancyGrid
        std::vector<unsigned char> visited;      // ComputeConcentration
        std::vector<int> heights;                // ComputeLER histogram
        std::vector<LEREntry> lerStack;          // ComputeLER monotonic stack
        std::vector<int> floodStack;             // Concentration+Stranded DFS
        std::vector<int> regionAreas;            // Concentration+Stranded: area per region
        std::vector<int> regionInterior;         // Concentration+Stranded: interior-cell count per region
        std::vector<unsigned char> regionHasLer; // Concentration+Stranded: LER-connectivity flag per region
        std::vector<SkylineNode> skyline;        // SkylinePack state
        std::vector<SkylineNode> skylineTmp;     // SkylinePack update scratch
        std::vector<Rect> wasteRects;            // Skyline waste map (under-cliff gaps)
        std::vector<int> placementIdGrid;        // SkylinePack: placement index per cell (-1 = empty)

        // LAHC scratch: greedy-seed and best-so-far placements.
        std::vector<Placement> bssfPl;
        std::vector<Placement> seedPl;
        std::vector<Placement> bestPl;

        // Tunables resolved per-pack from SearchParams. Populated in
        // PackAnnealedH/PackH before calling SkylinePack so the inner loop
        // doesn't take an extra parameter.
        int skylineWasteCoef;

        // FIFO ring. gridCacheCount == 0 is logically empty — stale
        // array contents never matter, only overwritten on insert.
        GridCacheEntry gridCache[64];
        int gridCacheCount;
        int gridCacheHead;

        // skylineSnapValid must be false whenever the log does not
        // correspond to the current curOrder — restart, abort, or
        // cross-target ctx reuse.
        std::vector<SkylineBoundary> skylineSnapBoundaries;
        std::vector<Rect> skylineSnapWaste;
        std::vector<SkylineNode> skylineSnapSkyline;
        std::vector<int> skylineSnapGridDelta;
        int skylineSnapN;
        bool skylineSnapValid;

#ifdef STACKSORT_PROFILE
        // SkylinePack prefix-cycle measurement: caller writes the would-be
        // Phase 3 kept-prefix into profSkylinePrefixK before each call;
        // SkylinePack stamps TSC at entry and accumulates (tsc_at_item_k -
        // tsc_start) into profSkylinePrefixCycles. 0 or negative values
        // skip the measurement (cold seed / restart seed calls).
        int profSkylinePrefixK;
        long long profSkylinePrefixCycles;
#endif
    };

  private:
    // H-mode implementations of Pack/PackAnnealed. The public Pack/PackAnnealed
    // dispatch here directly for TARGET_H and via a transpose wrapper for TARGET_W.
    static Result PackH(int gridW, int gridH, const std::vector<Item>& items, int target,
                        const volatile long* abortFlag = NULL, PackContext* reuseCtx = NULL);

    static Result PackAnnealedH(int gridW, int gridH, const std::vector<Item>& items, int target,
                                const volatile long* abortFlag = NULL, const std::vector<Item>* seedOrder = NULL,
                                std::vector<Item>* outBestOrder = NULL, int skipLAHCIfAreaBelow = 0,
                                const SearchParams* params = NULL, PackDiagnostics* outDiag = NULL,
                                PackContext* reuseCtx = NULL);

    static void InitPackContext(PackContext& ctx, int gridW, int gridH, int numItems);

    // Sort items by max(w,h) desc, then area desc.
    static void SortItems(std::vector<Item>& items);

    // MAXRECTS placement with selectable heuristic.
    // target reserves the bottom `target` rows; items prefer placement above.
    // When reserveW > 0, pre-places a virtual obstacle at (reserveX, reserveY)
    // and uses single-pass placement (hard constraint). reserveW == 0 = soft two-pass.
    // heuristic: 0 = BSSF (Best Short Side Fit), 1 = BAF (Best Area Fit).
    // Writes results into ctx.placements.
    static void MaxRectsPack(PackContext& ctx, int gridW, int gridH, const std::vector<Item>& items, int target,
                             const volatile long* abortFlag = NULL, int reserveX = 0, int reserveW = 0,
                             int heuristic = 0);

    // Split all free rects overlapping the placed rect, generating sub-rects.
    static void SplitFreeRects(PackContext& ctx, const Rect& placed);

    // Remove any free rect fully contained within another.
    static void PruneFreeRects(PackContext& ctx);

    // Skyline Bottom-Left packer — faster than MAXRECTS for annealing.
    // When reserveW > 0, items cannot overlap the reserved rectangle at
    // (reserveX, reserveY, reserveW, target) — hard constraint.
    // reserveW == 0 = existing soft two-pass reserve.
    // Writes results into ctx.placements.
    // startIdx > 0 requires caller to have restored ctx state from a prior
    // run's snapshot at boundary[startIdx].
    static void SkylinePack(PackContext& ctx, int gridW, int gridH, const std::vector<Item>& items, int target,
                            const volatile long* abortFlag = NULL, int reserveX = 0, int reserveW = 0,
                            int startIdx = 0);

    // Build occupancy grid into ctx.grid (0=empty, 1=occupied).
    static void BuildOccupancyGrid(PackContext& ctx, int gridW, int gridH);

    // ComputeLER using ctx scratch buffers. Grid passed explicitly to
    // avoid copying — public ComputeLER passes external grid directly.
    static void ComputeLERCtx(PackContext& ctx, const unsigned char* grid, int gridW, int gridH, int& outArea,
                              int& outWidth, int& outHeight, int& outX, int& outY);

    // Fused flood-fill: computes HHI concentration AND stranded-cell count in
    // a single pass over ctx.grid. Returns the concentration HHI; writes
    // stranded count via outStrandedCells. Replaces the old separate
    // ComputeConcentrationCtx + ComputeStrandedCells call pair.
    //
    // Semantics preserved bit-for-bit from the legacy functions:
    //   - HHI summation order is the legacy per-region loop (share² sum)
    //   - Stranded counts interior cells (x∈[1,W-2], y∈[1,H-2]) not
    //     4-connected to the LER rect
    //   - When lerW==0 || lerH==0, no region is LER-connected, so all
    //     interior empty cells count as stranded
    static double ComputeConcentrationAndStrandedCtx(PackContext& ctx, int gridW, int gridH, int lerX, int lerY,
                                                     int lerW, int lerH, int& outStrandedCells);

    // Flood-fill ctx.visited from cells inside the LER rect through
    // 4-connected empty space. Shared between the fused scorer above and
    // the repair_move branch in PackAnnealedH.
    static void FloodFillFromLer(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW, int lerH);

    // Hashes ctx.grid (caller must have built it via BuildOccupancyGrid),
    // returns cached LER + Concentration on hit or computes and inserts on
    // miss. Returns true iff hit.
    static bool GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth,
                                int& outLerHeight, int& outLerX, int& outLerY, double& outConcentration,
                                int& outStrandedCells);

    // Unified scoring function — used by annealing, Pack(), and result comparison.
    // Internally H-mode: checks lerHeight >= target. W-mode arrives here
    // already transposed, so the H-mode semantics remain correct.
    // groupingWeight / fragWeight have no defaults — callers must pass the
    // resolved effective values (DEFAULT_* or SearchParams override).
    // groupingBonus is long long because the power exponent (b^(quarters/4))
    // can grow past int32 at higher quarters.
    static long long ComputeScore(size_t numPlaced, int lerArea, int lerHeight, double concentration, int target,
                                  int numRotated, long long groupingBonus, int strandedCells, int groupingWeight,
                                  int fragWeight);

    // Per-component grouping bonus: each connected same-type cluster's shared
    // border total b is raised to b^(quarters/4) via applyGroupingPower.
    // quarters=6 (default) reproduces the legacy b^1.5 = b * isqrt(b) formula.
    static long long ComputeGroupingBonus(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                          int groupingPowerQuarters);

    // Power-independent companion: returns Σ b per component with no exponent
    // applied. The harness uses this as the cross-power clustering metric so
    // grouping_bonus heatmaps stay comparable across configs with different
    // groupingPowerQuarters. Called once at the final result, not per-iter.
    static long long ComputeGroupingBordersRaw(const std::vector<Placement>& placements,
                                               const std::vector<Item>& items);

    // Post-process: swap same-footprint items between positions to improve
    // grouping. Physical layout is unchanged since occupied cells are identical.
    static void OptimizeGrouping(std::vector<Placement>& placements, const std::vector<Item>& items,
                                 int groupingPowerQuarters);

    // Count rotated placements (shared between PackH and PackAnnealedH).
    static int CountRotated(const std::vector<Placement>& placements);

    static bool Overlaps(const Rect& a, const Rect& b);
    static bool Contains(const Rect& outer, const Rect& inner);

    // Shared border between two placements: shared edge length with corner
    // filter and flush bonus. Returns 0 if not adjacent or filtered out.
    static int SharedBorder(const Placement& a, const Placement& b);

    // Scan border cells for unique adjacent same-type placement IDs.
    static void CollectAdjacentPids(const PackContext& ctx, const std::vector<Item>& items, int curType, int start,
                                    int step, int count, int* adjPids, int& numAdj, int maxAdj);

    // MAXRECTS placement heuristic helpers (used by MaxRectsPack).
    // aboveReserveY >= 0: only rects where item fits above that Y.
    static void FindBestBSSF(const std::vector<Rect>& freeRects, const Item& item, int numOri, int aboveReserveY,
                             int& bestShortSide, int& bestLongSide, int& bestIndex, int& bestW, int& bestH,
                             bool& bestRotated);
    static void FindBestBAF(const std::vector<Rect>& freeRects, const Item& item, int numOri, int aboveReserveY,
                            long long& bestArea, int& bestShortSide, int& bestIndex, int& bestW, int& bestH,
                            bool& bestRotated);

    // Adjacency graph for OptimizeGrouping — precomputed SharedBorder values.
    // Kenshi grids are at most ~20x20; items are at least 1x1.
    struct AdjEntry
    {
        int neighbor; // placement index
        int border;   // SharedBorder value
    };

    struct AdjGraph
    {
        AdjEntry adj[256][24];
        int count[256];
    };

    static void BuildAdjGraph(AdjGraph& g, const std::vector<Placement>& placements);

    static long long ComputeGroupingBonusAdj(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                             const AdjGraph& g, int n, int groupingPowerQuarters);

    static const int TARGET_BONUS = 10000;
};
