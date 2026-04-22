#include "Packer.h"
#include "PackerLAHCInternals.h"

#include <algorithm>

namespace Packer
{

namespace Search
{

namespace
{

// Count positions where two orderings' exactIds disagree. Used as the
// diversity metric for Path Relinking elite admission — raw id-Hamming
// would false-reject pairs that differ only by swapping same-exactId
// siblings, which are packing-equivalent.
int PathRelinkExactIdHamming(const std::vector<Item>& a, const std::vector<Item>& b)
{
    int d    = 0;
    size_t n = a.size();
    if (b.size() != n) return (int)n;
    for (size_t i = 0; i < n; ++i)
        if (a[i].exactId != b[i].exactId) ++d;
    return d;
}

} // namespace

// Capture curOrder into the elite pool. Two safety nets:
//   - Normalize w/h/canRotate from originalItems so MOVE_ROTATE's in-place
//     mutation of curOrder entries doesn't produce elites whose geometry
//     diverges from their id.
//   - Reject near-duplicates via exactId-Hamming < diversityThreshold.
// Pool policy: append until cap, then replace weakest-scoring entry (only
// if the new score is strictly higher).
void CapturePathRelinkElite(PackContext& ctx, const std::vector<Item>& curOrder, const std::vector<Item>& originalItems,
                            long long score, int eliteCap, int diversityThreshold)
{
    std::vector<Item> normalized(curOrder);
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

    for (size_t i = 0; i < ctx.pathRelink.elites.size(); ++i)
    {
        if (PathRelinkExactIdHamming(normalized, ctx.pathRelink.elites[i]) < diversityThreshold) return;
    }

    if ((int)ctx.pathRelink.elites.size() < eliteCap)
    {
        ctx.pathRelink.elites.push_back(normalized);
        ctx.pathRelink.eliteScores.push_back(score);
        return;
    }

    size_t weakest      = 0;
    long long weakestSc = ctx.pathRelink.eliteScores[0];
    for (size_t i = 1; i < ctx.pathRelink.eliteScores.size(); ++i)
    {
        if (ctx.pathRelink.eliteScores[i] < weakestSc)
        {
            weakest   = i;
            weakestSc = ctx.pathRelink.eliteScores[i];
        }
    }
    if (score > weakestSc)
    {
        ctx.pathRelink.elites[weakest]      = normalized;
        ctx.pathRelink.eliteScores[weakest] = score;
    }
}

// Walk a transposition path from s toward goalOrder, scoring every
// intermediate. Any sc > bestScore commits as the new global best. Starts
// with a cold SkylinePack to establish the snapshot log, then each
// transposition reuses keptPrefix = leftmost swap position. Snapshot gate
// mirrors the LAHC loop's so PR degrades gracefully when the restore is
// unsafe (full cold re-pack instead).
bool PathRelinkWalk(PackContext& ctx, const GridSpec& dims, std::vector<Item>& s, const std::vector<Item>& goalOrder,
                    const std::vector<Item>& originalItems, const volatile long* abortFlag, int bestReserveX,
                    int bestReserveW, PackState& bestState, std::vector<Item>* outBestOrder, long long endpointScore,
                    int maxPathLen, PathRelinkDiag& diag)
{
    const int gridW  = dims.gridW;
    const int gridH  = dims.gridH;
    const int target = dims.target;

    int n = (int)s.size();
    if (n != (int)goalOrder.size()) return false;

    ctx.skyline.snapValid = false;
    Heuristics::SkylinePack(ctx, dims, s, abortFlag, bestReserveX, bestReserveW);
    if (abortFlag && *abortFlag != 0) return false;
    if (!ctx.skyline.snapValid || ctx.skyline.snapN != n)
    {
        ++diag.abortedPaths;
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
            ++diag.abortedPaths;
            break;
        }

        std::swap(s[p], s[q]);
        int keptPrefix = p;
        ++steps;

        bool canRestore =
            ctx.skyline.snapValid && ctx.skyline.snapN == n && keptPrefix > 0 && keptPrefix < ctx.skyline.snapN;
        int startIdx = canRestore ? keptPrefix : 0;
        if (canRestore) RestoreSkylineState(ctx, gridW, gridH, keptPrefix);
        Heuristics::SkylinePack(ctx, dims, s, abortFlag, bestReserveX, bestReserveW, startIdx);
        if (abortFlag && *abortFlag != 0) break;
        ++diag.intermediatesScored;

        PackState candState;
        candState.strandedCells = 0;
        candState.concentration = 0.0;
        Cache::GridCacheLookup(ctx, gridW, gridH, candState.lerArea, candState.lerWidth, candState.lerHeight,
                               candState.lerX, candState.lerY, candState.concentration, candState.strandedCells);
        long long grp   = Scoring::ComputeGroupingBonus(ctx.placements, originalItems, ctx);
        candState.score = Scoring::ComputeScore(ctx, ctx.placements.size(), candState.lerArea, candState.lerHeight,
                                                candState.concentration, target, Geometry::CountRotated(ctx.placements),
                                                grp, candState.strandedCells);

        if (candState.score > bestState.score)
        {
            long long gain = candState.score - endpointScore;
            diag.gainMax   = std::max(gain, diag.gainMax);
            UpdateBestFromCurrent(ctx, bestState, candState, outBestOrder, s);
            ++diag.globalBestUpdates;
            improved = true;
        }
    }

    diag.avgPathLenSum += steps;
    return improved;
}

} // namespace Search

} // namespace Packer
