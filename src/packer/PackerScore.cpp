#include "Packer.h"

#include <vector>

namespace Packer
{

// Scoring constants -- strict tier separation in ComputeScore.
// Each tier's max contribution < the next tier's minimum nonzero delta,
// so lower-priority metrics never override higher ones.
// fragWeight (default 50): max 50pts, < min LER delta of 3 (2^2 - 1^2).
// ROTATION_PENALTY=1: max ~42pts. Pure tiebreaker.
// groupingWeight (default 1): max ~200pts, concentration-discounted.
// Weights live in ctx.scoring (seeded with DEFAULT_* by InitPackContext,
// overridden per-pack by PackAnnealedH from SearchParams).

namespace Scoring
{

static const int ROTATION_PENALTY = 1;

long long ComputeScore(const PackContext& ctx, size_t numPlaced, int lerArea, int lerHeight, double concentration,
                       int target, int numRotated, long long groupingBonus, int strandedCells)
{
    long long score = (long long)numPlaced * 1000000LL;
    score += (long long)lerArea * (long long)lerArea;
    // Stranded cell penalty: interior empty cells outside the LER are
    // geometrically unreachable waste. Quadratic penalty (same tier as LER)
    // provides steep gradient away from scattered configurations.
    score -= (long long)strandedCells * (long long)strandedCells;
    if (lerHeight >= target) score += TARGET_BONUS;
    score += (long long)(concentration * (double)ctx.scoring.fragWeight);
    score -= (long long)numRotated * ROTATION_PENALTY;
    // Grouping bonus discounted by concentration: clustering that fragments
    // the free space gets reduced credit. concentration=1.0 (one blob) = full
    // bonus, concentration=0.3 (scattered gaps) = 30% bonus. groupingBonus
    // is already power-applied (b^(quarters/4)) by ComputeGroupingBonus.
    score += (long long)((double)groupingBonus * concentration * ctx.scoring.groupingWeight);
    return score;
}

} // namespace Scoring

namespace Grid
{

bool ValidatePlacements(int gridW, int gridH, const std::vector<Placement>& placements)
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

} // namespace Grid

} // namespace Packer
