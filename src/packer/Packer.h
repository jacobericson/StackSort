#pragma once

#include <vector>

namespace Packer
{

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

    // Grouping tier identifiers. -1 on any field = skip this tier on pair match.
    int exactId;             // same GameData pointer → same id
    int customGroupId;       // config-driven; -1 when unassigned
    int gameDataType;        // itemType enum int; -1 when == generic ITEM
    int itemFunction;        // ItemFunction enum int; -1 when == ITEM_NO_FUNCTION
    unsigned char flagsMask; // bit 0 = food_crop, bit 1 = trade_item
};

struct Placement
{
    int id;
    int x;
    int y;
    int w;
    int h;
    bool rotated; // true if packer swapped w/h relative to input
    // SkylinePack receives a permuted `items` vector (curOrder), so
    // items[p.id].exactId is the item at position p.id in the current
    // ordering, not the item with id == p.id. Carry the exact id on
    // Placement so CollectAdjacentPids can compare directly. No other
    // tier ids here: skyline tiebreaker stays exact-only; richer tier
    // matching runs only in the final scorer.
    int exactId;
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

// Tier weights (fixed-point percent, 0..100). PairWeight takes the max
// across matching tiers; weight 0 disables that tier. When all non-exact
// weights are 0, scoring is bit-equivalent to the pre-tier scheme.
static const int DEFAULT_TIER_WEIGHT_EXACT    = 100;
static const int DEFAULT_TIER_WEIGHT_CUSTOM   = 70;
static const int DEFAULT_TIER_WEIGHT_TYPE     = 50;
static const int DEFAULT_TIER_WEIGHT_FUNCTION = 40;
static const int DEFAULT_TIER_WEIGHT_FLAGS    = 10;

// Partial-similarity multipliers applied to the function tier for
// specific cross-function pairs (symmetric). 100 = full match; 0 disables.
static const int DEFAULT_FUNC_SIM_FOOD_FOOD_RESTRICTED = 50;
static const int DEFAULT_FUNC_SIM_FIRSTAID_ROBOTREPAIR = 50;

// Soft-grouping bonus scale, in percent. The grouping scorer is split
// into two tracks: (1) an exact-match track that preserves legacy
// superadditive b^1.5 clustering via union-find on exactId, and (2) a
// soft track that clusters non-exact tier-matched pairs via a separate
// union-find with b^(5/4) power. Soft contribution per component is
//   applyGroupingPower(compBorders, 5) * SOFT_GROUPING_PCT / 100
// A value of 0 disables the soft track entirely, giving
// bit-equivalence with legacy scoring.
static const int DEFAULT_SOFT_GROUPING_PCT = 50;

// Path Relinking defaults. PR runs after the multi-restart LAHC loop,
// walking transposition paths between pairs of elites captured on global-
// best improvements. Off by default to preserve baseline parity; enable via
// SearchParams::enablePathRelinking or [features] enable_path_relinking.
static const int DEFAULT_ENABLE_PATH_RELINKING     = 1;
static const int DEFAULT_PATH_RELINK_ELITE_CAP     = 8; // R² pair count dominates cost
static const int DEFAULT_PATH_RELINK_DIVERSITY_PCT = 0; // 0 = no diversity filter (admit until cap)
static const int DEFAULT_PATH_RELINK_MAX_PATH_LEN  = 0; // 0 sentinel → use N (items.size)

static const int DEFAULT_LATE_BIAS_ALPHA_Q     = 8;  // max-of-2 (α=2)
static const int DEFAULT_LATE_BIAS_UNIFORM_PCT = 35; // 35% uniform floor

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

    // Post-pack geometric moves. Default on; pass 0 in SearchParams to
    // ablate off. Both preserve cell occupancy so LER/concentration/
    // stranded are invariant — only the grouping bonus can change.
    int enableStripShift; // permute items within same-(x,w) or same-(y,h) strips
    int enableTileSwap;   // swap a single placement with a multi-placement rectangle

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

    // Grouping tier weights (0..100 percent). -1 = compiled default.
    // 0 disables that tier. Resolver clamps values above 100.
    int tierWeightExact;
    int tierWeightCustom;
    int tierWeightType;
    int tierWeightFunction;
    int tierWeightFlags;

    // Function-similarity overrides (0..100 percent, symmetric).
    // -1 = compiled default. 0 disables the specific cross-function pair.
    int funcSimFoodFoodRestricted;
    int funcSimFirstaidRobotrepair;

    // Soft-grouping scale (0..100 percent). -1 = compiled default.
    // 0 disables the soft track; exact-match behavior is unchanged.
    int softGroupingPct;

    // Path Relinking (post-restart intensification over per-restart elites).
    // enablePathRelinking: -1 default, 0 off, 1 on.
    // pathRelinkEliteCap: pool cap; <= 0 → compiled default.
    // pathRelinkDiversityPct: exactId-Hamming diversity threshold as
    //   percent of N (100 = reject any near-duplicate). -1 = default.
    // pathRelinkMaxPathLen: hard cap on transpositions per pair;
    //   <= 0 → use items.size().
    int enablePathRelinking;
    int pathRelinkEliteCap;
    int pathRelinkDiversityPct;
    int pathRelinkMaxPathLen;

    // Late-biased move generation: sample move.a with p(i) ∝ (i/n)^(alphaQ/4)
    // then take keptPrefix = min(a, b). Pushes disturbance toward the tail
    // of the packing order so snapshot restore skips more prefix work.
    // lateBiasAlphaQ: exponent in quarter-steps (12 = α=3). 0 = disabled.
    // lateBiasUniformPct: percent of moves drawn uniformly for diversification.
    //   100 = fully uniform = disabled. -1 = compiled default (0 = disabled).
    int lateBiasAlphaQ;
    int lateBiasUniformPct;

    SearchParams()
        : numRestarts(-1), itersPerRestart(-1), lahcHistoryLen(-1), plateauThreshold(-1), rngSeed(0), enableBafSeed(-1),
          enableUnconstrainedFallback(-1), enableOptimizeGrouping(-1), enableFastConverge(-1), enableRepairMove(-1),
          enablePreReservation(-1), enableStripShift(-1), enableTileSwap(-1), moveSwapMax(-1), moveInsertMax(-1),
          moveRotateMax(-1), scoringGroupingWeight(-1), scoringFragWeight(-1), groupingPowerQuarters(-1),
          skylineWasteCoef(-1), tierWeightExact(-1), tierWeightCustom(-1), tierWeightType(-1), tierWeightFunction(-1),
          tierWeightFlags(-1), funcSimFoodFoodRestricted(-1), funcSimFirstaidRobotrepair(-1), softGroupingPct(-1),
          enablePathRelinking(-1), pathRelinkEliteCap(-1), pathRelinkDiversityPct(-1), pathRelinkMaxPathLen(-1),
          lateBiasAlphaQ(-1), lateBiasUniformPct(-1)
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

    // StripShift/TileSwap post-pack move diagnostics. Zero when the
    // corresponding feature flag is off.
    int stripShiftStripsFound;       // contiguous strip runs of length >= 2
    int stripShiftStripsImproved;    // strips where a non-identity perm won
    int tileSwapCandidatesFound;     // (X, R, G) triples that passed feasibility
    int tileSwapCandidatesCommitted; // candidates that improved grouping

    // Path Relinking diagnostics. Zero when enablePathRelinking is off.
    int pathRelinkPairsRun;                // ordered (s, g) pairs walked (both directions)
    int pathRelinkIntermediatesScored;     // total transposition steps evaluated
    int pathRelinkGlobalBestUpdates;       // PR strictly beat bestScore
    int pathRelinkAbortedPaths;            // snapshot stale / multiset mismatch / abort
    long long pathRelinkAvgPathLenSum;     // sum of steps over all paths (avg = sum / pairsRun)
    long long pathRelinkGlobalBestGainMax; // largest single-intermediate gain over its source endpoint

    // Power-independent clustering metric: Σ b per same-type connected
    // component on the final placement (no power exponent applied). The
    // harness uses this for cross-power heatmap comparisons since
    // groupingBonus's scale changes with groupingPowerQuarters.
    long long groupingBordersRaw;

    // Exact-track contribution to groupingBonus (b^1.5 over exactId-matched
    // components only, no soft-track addition). groupingBonus - this =
    // linear soft contribution. Diagnostic so tuning can see the split.
    long long groupingBonusExact;

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
    long long profCyclesStripShift;            // post-LAHC strip permutation
    long long profCyclesTileSwap;              // post-LAHC multi-placement swap
    long long profCyclesPathRelink;            // post-LAHC path relinking over elite pool
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

    // SkylinePack sub-phase breakdown (mirrors PackContext accumulators).
    long long profCyclesSkylineWasteMap;
    long long profCyclesSkylineCandidate;
    long long profCyclesSkylineAdjacency;
    long long profCyclesSkylineCommit;

    // ComputeLERCtx sub-phase breakdown.
    long long profCyclesLerHistogram;
    long long profCyclesLerStack;
#endif

    PackDiagnostics()
        : bestFoundIter(0), bestFoundRestart(0), plateauBreaks(0), lahcItersExecuted(0), packCalls(0),
          unconstrainedFallbackWon(false), greedySeedScore(0), greedySeedLerArea(0), repairMoveRolls(0),
          repairMoveScans(0), repairMoveHits(0), repairMoveAccepts(0), stripShiftStripsFound(0),
          stripShiftStripsImproved(0), tileSwapCandidatesFound(0), tileSwapCandidatesCommitted(0),
          pathRelinkPairsRun(0), pathRelinkIntermediatesScored(0), pathRelinkGlobalBestUpdates(0),
          pathRelinkAbortedPaths(0), pathRelinkAvgPathLenSum(0), pathRelinkGlobalBestGainMax(0), groupingBordersRaw(0),
          groupingBonusExact(0), skylineSnapHits(0), skylineSnapProbes(0)
#ifdef STACKSORT_PROFILE
          ,
          profCyclesMoveGen(0), profCyclesSkylinePack(0), profCyclesLer(0), profCyclesConcentration(0),
          profCyclesGrouping(0), profCyclesStranded(0), profCyclesScore(0), profCyclesAccept(0),
          profCyclesPreReservation(0), profCyclesGreedySeed(0), profCyclesUnconstrainedFallback(0),
          profCyclesOptimizeGrouping(0), profCyclesStripShift(0), profCyclesTileSwap(0), profCyclesPathRelink(0),
          profCyclesBordersRaw(0), keptPrefixSum(0), keptPrefixCount(0), gridHashProbes(0), gridHashHits(0),
          profCyclesSkylinePrefix(0), profCyclesSkylineWasteMap(0), profCyclesSkylineCandidate(0),
          profCyclesSkylineAdjacency(0), profCyclesSkylineCommit(0), profCyclesLerHistogram(0), profCyclesLerStack(0)
#endif
    {
    }
};

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
    // 128-bit twin Zobrist key. Birthday-bound collision at realistic
    // distinct-grid counts (~10^5 per run) is ~10^-31 — authoritative
    // for bit-exact determinism without a parallel grid-blob compare.
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
    unsigned long long hashA;
    unsigned long long hashB;
};

// Skyline arena comfortably covers the 20-wide corpus worst case
// (each placement adds at most 2 segments before coalesce).
static const int SKYLINE_ARENA_CAP = 128;
static const int GRID_CACHE_CAP    = 64;
static const int EXACT_ID_CAP      = 512;

// MAXRECTS working set. Touched only by MaxRectsPack / SplitFreeRects /
// PruneFreeRects; dormant once the seed pass finishes.
struct MaxRectsScratch
{
    std::vector<Rect> freeRects;
    std::vector<Rect> newRects;
    std::vector<bool> dead;
};

// Skyline state as an intrusive singly-linked list over a fixed arena,
// plus per-item snapshot log for LAHC prefix restore. Head walks
// left-to-right by x; x-order is invariant by construction.
struct SkylineState
{
    SkylineNode nodes[SKYLINE_ARENA_CAP];
    short next[SKYLINE_ARENA_CAP];
    short head;
    short freeHead;
    short count;
    std::vector<Rect> wasteRects; // under-cliff gaps detected during pack

    // snapValid must be false whenever the log does not correspond to
    // the current curOrder — restart, abort, or cross-target ctx reuse.
    std::vector<SkylineBoundary> snapBoundaries;
    std::vector<Rect> snapWaste;
    std::vector<SkylineNode> snapSkyline;
    int snapN;
    bool snapValid;
};

// LER + concentration/stranded scratch. `visited` stays at the outer
// PackContext level because both LER and the repair-move path share it.
struct LerScratch
{
    std::vector<int> heights;                // ComputeLER histogram
    std::vector<LEREntry> lerStack;          // ComputeLER monotonic stack
    std::vector<int> floodStack;             // Concentration+Stranded DFS
    std::vector<int> regionAreas;            // per-region area
    std::vector<int> regionInterior;         // per-region interior-cell count
    std::vector<unsigned char> regionHasLer; // per-region LER-connectivity flag
};

// Zobrist keying + FIFO grid-cache ring for GridCacheLookup. Tables are
// pure functions of (gridW, gridH) seeded via splitmix64 with independent
// constants per table, so the twin-hash retains 128-bit entropy.
// curHashA/B is XOR-maintained incrementally via EmitBoundary; each
// SkylineBoundary snapshots the post-placement hash so RestoreSkylineState
// can reload it in O(1). `count == 0` is logically empty — stale array
// contents never matter, only overwritten on insert.
struct GridCache
{
    std::vector<unsigned long long> zobristA;
    std::vector<unsigned long long> zobristB;
    int tableW;
    int tableH;
    unsigned long long curHashA;
    unsigned long long curHashB;
    GridCacheEntry entries[GRID_CACHE_CAP];
    int count;
    int ringHead;
};

// Grouping tier weights, function-similarity overrides, soft-track scale,
// and the memoized N*N pair-weight table. Resolved per-pack from
// SearchParams; read by PairWeight in the final scorer.
// pairWeightMatrixN == 0 → matrix not populated → scoring falls back to
// recomputing PairWeight per pair.
struct GroupingConfig
{
    int tierWeightExact;
    int tierWeightCustom;
    int tierWeightType;
    int tierWeightFunction;
    int tierWeightFlags;
    int funcSimFoodFoodRestricted;
    int funcSimFirstaidRobotrepair;
    int softGroupingPct;
    std::vector<unsigned char> pairWeightMatrix;
    int pairWeightMatrixN;
};

// Path Relinking elite pool. Orderings captured on global-best
// improvements inside the multi-restart LAHC loop; walked pairwise
// after the loop exits. Items are normalized on capture (w/h/canRotate
// reset from the input items vector) to eliminate MOVE_ROTATE pollution.
// eliteScores[i] is the bestScore at capture time — used for diversity
// admission and for PR's strict-improvement gate.
struct PathRelinkPool
{
    std::vector<std::vector<Item> > elites;
    std::vector<long long> eliteScores;
};

#ifdef STACKSORT_PROFILE
// Per-run rdtsc counters copied into PackDiagnostics at run end.
// skylinePrefixK is caller-written: SkylinePack stamps TSC at entry
// and accumulates (tsc_at_item_k - tsc_start) into skylinePrefixCycles.
// 0 or negative skylinePrefixK skips the measurement (cold seed calls).
// Sub-phase accumulators identify which inner component dominates.
struct ProfileCounters
{
    int skylinePrefixK;
    long long skylinePrefixCycles;
    long long cyclesSkylineWasteMap;  // waste-rect scan + placement
    long long cyclesSkylineCandidate; // segment walk (maxY + waste)
    long long cyclesSkylineAdjacency; // CollectAdjacentPids + SharedBorder
    long long cyclesSkylineCommit;    // place + waste-detect + skyline rebuild
    long long cyclesLerHistogram;     // per-row heights update
    long long cyclesLerStack;         // monotonic-stack sweep
};
#endif

// Reusable scratch buffers for the packing hot path. Construct once
// per job; pass via Pack/PackAnnealed's reuseCtx to amortize
// allocations. Not thread-safe — each worker needs its own.
struct PackContext
{
    // Cross-cutting outputs and grids — accessed from most TUs.
    std::vector<Placement> placements;  // packing output
    std::vector<Placement> bssfPl;      // LAHC best-so-far
    std::vector<Placement> seedPl;      // LAHC greedy seed
    std::vector<Placement> bestPl;      // LAHC best result
    std::vector<unsigned char> grid;    // BuildOccupancyGrid
    std::vector<unsigned char> visited; // flood-fill marker (LER + repair move)
    std::vector<int> placementIdGrid;   // SkylinePack: placement index per cell (-1 = empty)

    // Per-exactId placement count. EXACT_ID_CAP is a soft limit —
    // exactIds at or above fall back to full scan (correctness
    // preserved; corpora stay well under).
    int typeCount[EXACT_ID_CAP];

    // Algorithm state, grouped by responsibility.
    MaxRectsScratch maxRects;
    SkylineState skyline;
    LerScratch ler;
    GridCache cache;
    GroupingConfig grouping;
    PathRelinkPool pathRelink;

    // Skyline tiebreaker knob (waste * coef - contact). Not grouping-
    // related; resolved per-pack from SearchParams.skylineWasteCoef.
    int skylineWasteCoef;

#ifdef STACKSORT_PROFILE
    ProfileCounters prof;
#endif
};

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

static const int TARGET_BONUS = 10000;

// Pure rect/placement math. Stateless primitives shared by scoring + heuristics.
namespace Geometry
{
// Shared border between two placements: shared edge length with corner
// filter and flush bonus. Returns 0 if not adjacent or filtered out.
// Defined inline in PackerScoringInline.h.
inline int SharedBorder(const Placement& a, const Placement& b);

// Count rotated placements (shared between PackH and PackAnnealedH).
int CountRotated(const std::vector<Placement>& placements);
} // namespace Geometry

// Occupancy grid + flood-fill plumbing. Writes/reads ctx.grid and ctx.visited.
namespace Grid
{
// Build occupancy grid into ctx.grid (0=empty, 1=occupied).
void BuildOccupancyGrid(PackContext& ctx, int gridW, int gridH);

// Check that no cell is doubly-occupied and all coords are in bounds.
bool ValidatePlacements(int gridW, int gridH, const std::vector<Placement>& placements);

// Flood-fill ctx.visited from cells inside the LER rect through
// 4-connected empty space. Shared between the fused scorer and the
// repair-move branch in PackAnnealedH. extGrid != NULL reads occupancy
// from the caller's buffer instead of ctx.grid — required when the
// caller needs ctx.grid to stay untouched for incremental maintenance.
// Output still lands in ctx.visited.
void FloodFillFromLer(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW, int lerH,
                      const unsigned char* extGrid = NULL);
} // namespace Grid

// Largest Empty Rectangle + concentration/stranded scoring primitives.
namespace Ler
{
// Compute the Largest Empty Rectangle on an occupancy grid.
// grid is W*H unsigned chars, 0 = empty, nonzero = occupied.
// Exposed publicly for before/after comparison.
void ComputeLER(const std::vector<unsigned char>& grid, int gridW, int gridH, int& outArea, int& outWidth,
                int& outHeight, int& outX, int& outY);

// ComputeLER using ctx scratch buffers. Grid passed explicitly to
// avoid copying — public ComputeLER passes external grid directly.
void ComputeLERCtx(PackContext& ctx, const unsigned char* grid, int gridW, int gridH, int& outArea, int& outWidth,
                   int& outHeight, int& outX, int& outY);

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
double ComputeConcentrationAndStrandedCtx(PackContext& ctx, int gridW, int gridH, int lerX, int lerY, int lerW,
                                          int lerH, int& outStrandedCells);
} // namespace Ler

// Zobrist-keyed FIFO cache for seen-grid short-circuit.
namespace Cache
{
// Hashes ctx.grid (caller must have built it via BuildOccupancyGrid),
// returns cached LER + Concentration on hit or computes and inserts on
// miss. Returns true iff hit.
bool GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth, int& outLerHeight,
                     int& outLerX, int& outLerY, double& outConcentration, int& outStrandedCells);
} // namespace Cache

// Greedy placers: MAXRECTS seed + Skyline Bottom-Left (LAHC hot path).
namespace Heuristics
{
// MAXRECTS placement with selectable heuristic.
// target reserves the bottom `target` rows; items prefer placement above.
// When reserveW > 0, pre-places a virtual obstacle at (reserveX, reserveY)
// and uses single-pass placement (hard constraint). reserveW == 0 = soft two-pass.
// heuristic: 0 = BSSF (Best Short Side Fit), 1 = BAF (Best Area Fit).
// Writes results into ctx.placements.
void MaxRectsPack(PackContext& ctx, int gridW, int gridH, const std::vector<Item>& items, int target,
                  const volatile long* abortFlag = NULL, int reserveX = 0, int reserveW = 0, int heuristic = 0);

// Skyline Bottom-Left packer — faster than MAXRECTS for annealing.
// When reserveW > 0, items cannot overlap the reserved rectangle at
// (reserveX, reserveY, reserveW, target) — hard constraint.
// reserveW == 0 = existing soft two-pass reserve.
// Writes results into ctx.placements.
// startIdx > 0 requires caller to have restored ctx state from a prior
// run's snapshot at boundary[startIdx].
void SkylinePack(PackContext& ctx, int gridW, int gridH, const std::vector<Item>& items, int target,
                 const volatile long* abortFlag = NULL, int reserveX = 0, int reserveW = 0, int startIdx = 0);
} // namespace Heuristics

// Final-pack scoring + grouping-bonus machinery.
namespace Scoring
{
// Unified scoring function — used by annealing, Pack(), and result comparison.
// Internally H-mode: checks lerHeight >= target. W-mode arrives here
// already transposed, so the H-mode semantics remain correct.
// groupingWeight / fragWeight have no defaults — callers must pass the
// resolved effective values (DEFAULT_* or SearchParams override).
// groupingBonus is long long because the power exponent (b^(quarters/4))
// can grow past int32 at higher quarters.
long long ComputeScore(size_t numPlaced, int lerArea, int lerHeight, double concentration, int target, int numRotated,
                       long long groupingBonus, int strandedCells, int groupingWeight, int fragWeight);

// Populate ctx.grouping.pairWeightMatrix before any per-iter scoring call.
// Guard on softGroupingPct > 0 — the matrix is unused when the soft track
// is off.
void BuildPairWeightMatrix(PackContext& ctx, const std::vector<Item>& items);

// Split-track grouping bonus. Exact track: union-find on exactId equality,
// per-component b^(quarters/4) via applyGroupingPower (legacy). Soft track:
// union-find on PairWeight > 0 (non-exact), per-component b^(5/4).
// softGroupingPct == 0 skips the soft track entirely.
// If outExactOnly != NULL, writes just the exact-track contribution there.
long long ComputeGroupingBonus(const std::vector<Placement>& placements, const std::vector<Item>& items,
                               const PackContext& ctx, int groupingPowerQuarters, long long* outExactOnly = NULL);

// Power-independent companion: returns Σ b per component with no exponent
// applied. Uses the same tier-weighted accumulator as ComputeGroupingBonus.
// The harness uses this as the cross-power clustering metric so
// grouping_bonus heatmaps stay comparable across configs with different
// groupingPowerQuarters. Called once at the final result, not per-iter.
long long ComputeGroupingBordersRaw(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                    const PackContext& ctx);

void BuildAdjGraph(AdjGraph& g, const std::vector<Placement>& placements);

long long ComputeGroupingBonusAdj(const std::vector<Placement>& placements, const std::vector<Item>& items,
                                  const AdjGraph& g, int n, const PackContext& ctx, int groupingPowerQuarters);
} // namespace Scoring

// Layout-invariant post-pack moves: cell occupancy preserved, only grouping
// bonus can change.
namespace PostPack
{
// Swap same-footprint items between positions to improve grouping.
// Physical layout is unchanged since occupied cells are identical.
void OptimizeGrouping(std::vector<Placement>& placements, const std::vector<Item>& items, const PackContext& ctx,
                      int groupingPowerQuarters);

// Permute items within strips (contiguous runs of same-(x,w)
// or same-(y,h) placements). Cell occupancy preserved (strip footprint
// unchanged). Updates placementIdGrid when committing a non-identity perm.
void StripShift(std::vector<Placement>& placements, const std::vector<Item>& items, PackContext& ctx, int gridW,
                int gridH, int groupingPowerQuarters, int* outStripsFound = NULL, int* outStripsImproved = NULL);

// Swap a single placement X with a rectangular union of multiple
// placements G elsewhere. Cell occupancy preserved (both regions share
// the same footprint). For rotated swaps, verifies G can tile X's
// destination via corner-first backtracking.
void TileSwap(std::vector<Placement>& placements, const std::vector<Item>& items, PackContext& ctx, int gridW,
              int gridH, int groupingPowerQuarters, int* outCandidatesFound = NULL, int* outCandidatesCommitted = NULL);
} // namespace PostPack

// Public entry points + LAHC/PR orchestration. Callers reach the packer
// through Search::Pack / Search::PackAnnealed; everything else is internal.
namespace Search
{
// Run the full packing pipeline: sort, MAXRECTS place, compute LER, score.
// If abortFlag is non-NULL, checked per-item; returns partial result on abort.
// W-mode transposes internally, so placements/LER come back in original space.
Result Pack(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
            const volatile long* abortFlag = NULL, PackContext* reuseCtx = NULL);

// Greedy seed + LAHC (Late Acceptance Hill Climbing). Used by worker thread.
// seedOrder: if non-NULL, use as restart 0's initial ordering (warm start).
// outBestOrder: if non-NULL, receives the ordering that produced the best result.
// skipLAHCIfAreaBelow: if > 0 and pre-reservation upperBound < this, skip LAHC.
// params: if non-NULL, overrides LAHC constants and ablation flags.
// outDiag: if non-NULL, receives diagnostic counters for the harness/tuner.
// W-mode transposes internally.
Result PackAnnealed(int gridW, int gridH, const std::vector<Item>& items, TargetDim dim, int target,
                    const volatile long* abortFlag = NULL, const std::vector<Item>* seedOrder = NULL,
                    std::vector<Item>* outBestOrder = NULL, int skipLAHCIfAreaBelow = 0,
                    const SearchParams* params = NULL, PackDiagnostics* outDiag = NULL, PackContext* reuseCtx = NULL);

// H-mode implementations of Pack/PackAnnealed. The public Pack/PackAnnealed
// dispatch here directly for TARGET_H and via a transpose wrapper for TARGET_W.
Result PackH(int gridW, int gridH, const std::vector<Item>& items, int target, const volatile long* abortFlag = NULL,
             PackContext* reuseCtx = NULL);

Result PackAnnealedH(int gridW, int gridH, const std::vector<Item>& items, int target,
                     const volatile long* abortFlag = NULL, const std::vector<Item>* seedOrder = NULL,
                     std::vector<Item>* outBestOrder = NULL, int skipLAHCIfAreaBelow = 0,
                     const SearchParams* params = NULL, PackDiagnostics* outDiag = NULL, PackContext* reuseCtx = NULL);

// Path Relinking: walk a transposition path from `s` toward `goalOrder`,
// re-packing via SkylinePack (with startIdx = min swap position) and
// scoring every intermediate. Commits any score > bestScore as the new
// global best. Mutates `s` in place.
bool PathRelinkWalk(PackContext& ctx, int gridW, int gridH, std::vector<Item>& s, const std::vector<Item>& goalOrder,
                    const std::vector<Item>& originalItems, int target, const volatile long* abortFlag,
                    int bestReserveX, int bestReserveW, int effGroupingWeight, int effFragWeight, int effGroupingPower,
                    long long& bestScore, int& bestLerA, int& bestLerW, int& bestLerH, int& bestLerX, int& bestLerY,
                    double& bestConc, int& bestStranded, bool& repairGridDirty, std::vector<Item>* outBestOrder,
                    long long endpointScore, int maxPathLen, int& diagIntermediatesScored, int& diagBestUpdates,
                    int& diagAbortedPaths, long long& diagGainMax, long long& diagAvgPathLenSum);

void InitPackContext(PackContext& ctx, int gridW, int gridH, int numItems);

// Sort items by max(w,h) desc, then area desc.
void SortItems(std::vector<Item>& items);
} // namespace Search

} // namespace Packer
