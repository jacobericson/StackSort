#include "Packer.h"
#include "PackerLAHCInternals.h"

#include <algorithm>

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
void CapturePathRelinkElite(Packer::PackContext& ctx, const std::vector<Packer::Item>& curOrder,
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

    ctx.skyline.snapValid = false;
    SkylinePack(ctx, gridW, gridH, s, target, abortFlag, bestReserveX, bestReserveW);
    if (abortFlag && *abortFlag != 0) return false;
    if (!ctx.skyline.snapValid || ctx.skyline.snapN != n)
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
            ctx.skyline.snapValid && ctx.skyline.snapN == n && keptPrefix > 0 && keptPrefix < ctx.skyline.snapN;
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
