#include "Packer.h"
#include "PackerPostPackInline.h"

#include <algorithm>
#include <cstring>

// Permute items within contiguous strips. A vertical strip is a set of
// placements sharing (x, w) with heights stacked contiguously along y; a
// horizontal strip is the transposed version. Cell occupancy is preserved
// because the strip's bounding rectangle is unchanged; only which item
// occupies which slice shifts. Adjacency is re-scored via the full
// ComputeGroupingBonusAdj path so exactId/soft-tier weights apply.

void Packer::StripShift(std::vector<Placement>& placements, const std::vector<Item>& items, PackContext& ctx, int gridW,
                        int gridH, int groupingPowerQuarters, int* outStripsFound, int* outStripsImproved)
{
    (void)gridH;

    int n = (int)placements.size();
    if (n <= 1 || n > 256) return;

    int stripsFound    = 0;
    int stripsImproved = 0;

    // Scratch arrays sized for the worst strip (n <= 5).
    static const int STRIP_CAP = 5;
    int stripIdx[STRIP_CAP];
    int origH[STRIP_CAP];
    int origY[STRIP_CAP];
    int perm[STRIP_CAP];
    int bestPerm[STRIP_CAP];

    AdjGraph g;

    // Two passes: axis == 0 for vertical strips (share x+w, stack along y),
    // axis == 1 for horizontal strips (share y+h, stack along x).
    // Within one axis a placement can only belong to one strip; across axes
    // it can participate twice.
    unsigned char usedV[256];
    unsigned char usedH[256];
    std::memset(usedV, 0, sizeof(usedV));
    std::memset(usedH, 0, sizeof(usedH));

    for (int axis = 0; axis < 2; ++axis)
    {
        unsigned char* used = (axis == 0) ? usedV : usedH;

        for (int head = 0; head < n; ++head)
        {
            if (used[head]) continue;

            // Walk to the min-coord end of the strip containing `head`.
            int bot = head;
            for (bool found = true; found;)
            {
                found = false;
                for (int k = 0; k < n; ++k)
                {
                    if (k == bot || used[k]) continue;
                    const Placement& kp = placements[k];
                    const Placement& bp = placements[bot];
                    if (axis == 0)
                    {
                        if (kp.x == bp.x && kp.w == bp.w && kp.y + kp.h == bp.y)
                        {
                            bot   = k;
                            found = true;
                            break;
                        }
                    }
                    else
                    {
                        if (kp.y == bp.y && kp.h == bp.h && kp.x + kp.w == bp.x)
                        {
                            bot   = k;
                            found = true;
                            break;
                        }
                    }
                }
            }

            // Collect the strip by walking upward/right from bot.
            int stripLen         = 0;
            stripIdx[stripLen++] = bot;
            used[bot]            = 1;
            int cur              = bot;
            while (stripLen < STRIP_CAP)
            {
                int next            = -1;
                const Placement& cp = placements[cur];
                int expectCoord     = (axis == 0) ? (cp.y + cp.h) : (cp.x + cp.w);
                for (int k = 0; k < n; ++k)
                {
                    if (used[k]) continue;
                    const Placement& kp = placements[k];
                    if (axis == 0)
                    {
                        if (kp.x == cp.x && kp.w == cp.w && kp.y == expectCoord)
                        {
                            next = k;
                            break;
                        }
                    }
                    else
                    {
                        if (kp.y == cp.y && kp.h == cp.h && kp.x == expectCoord)
                        {
                            next = k;
                            break;
                        }
                    }
                }
                if (next < 0) break;
                stripIdx[stripLen++] = next;
                used[next]           = 1;
                cur                  = next;
            }

            // Skip trivial strips. Length-1 is not a strip; >STRIP_CAP items
            // would need a different enumeration strategy.
            if (stripLen < 2) continue;

            // Detect longer strips so we can mark their tail but skip
            // enumeration — conservatively leave them for a future pass.
            bool tailOverflowed = false;
            if (stripLen == STRIP_CAP)
            {
                const Placement& cp = placements[cur];
                int expectCoord     = (axis == 0) ? (cp.y + cp.h) : (cp.x + cp.w);
                for (int k = 0; k < n; ++k)
                {
                    if (used[k]) continue;
                    const Placement& kp = placements[k];
                    bool hit            = (axis == 0) ? (kp.x == cp.x && kp.w == cp.w && kp.y == expectCoord)
                                                      : (kp.y == cp.y && kp.h == cp.h && kp.x == expectCoord);
                    if (hit)
                    {
                        tailOverflowed = true;
                        break;
                    }
                }
            }
            if (tailOverflowed) continue;

            ++stripsFound;

            // Capture originals.
            int baseCoord = (axis == 0) ? placements[stripIdx[0]].y : placements[stripIdx[0]].x;
            for (int i = 0; i < stripLen; ++i)
            {
                origH[i]    = (axis == 0) ? placements[stripIdx[i]].h : placements[stripIdx[i]].w;
                origY[i]    = (axis == 0) ? placements[stripIdx[i]].y : placements[stripIdx[i]].x;
                perm[i]     = i;
                bestPerm[i] = i;
            }

            // Score identity first so we compare apples to apples.
            BuildAdjGraph(g, placements);
            long long bestScore = ComputeGroupingBonusAdj(placements, items, g, n, ctx, groupingPowerQuarters);
            long long baseScore = bestScore;

            // Enumerate remaining permutations. next_permutation starts from
            // current perm [0,1,..,m-1] and walks lex order through the rest.
            while (std::next_permutation(perm, perm + stripLen))
            {
                int cy = baseCoord;
                for (int i = 0; i < stripLen; ++i)
                {
                    Placement& p = placements[stripIdx[perm[i]]];
                    if (axis == 0) p.y = cy;
                    else p.x = cy;
                    cy += origH[perm[i]];
                }

                BuildAdjGraph(g, placements);
                long long score = ComputeGroupingBonusAdj(placements, items, g, n, ctx, groupingPowerQuarters);
                if (score > bestScore)
                {
                    bestScore = score;
                    for (int i = 0; i < stripLen; ++i)
                        bestPerm[i] = perm[i];
                }
            }

            // Restore to bestPerm layout.
            {
                int cy = baseCoord;
                for (int i = 0; i < stripLen; ++i)
                {
                    Placement& p = placements[stripIdx[bestPerm[i]]];
                    if (axis == 0) p.y = cy;
                    else p.x = cy;
                    cy += origH[bestPerm[i]];
                }
            }

            bool isIdentity = true;
            for (int i = 0; i < stripLen; ++i)
                if (bestPerm[i] != i)
                {
                    isIdentity = false;
                    break;
                }

            if (!isIdentity && bestScore > baseScore)
            {
                ++stripsImproved;
                if ((int)ctx.placementIdGrid.size() >= gridW * gridH)
                    StampPlacementCells(ctx.placementIdGrid, placements, gridW, stripIdx, stripLen);
            }
        }
    }

    if (outStripsFound) *outStripsFound = stripsFound;
    if (outStripsImproved) *outStripsImproved = stripsImproved;
}
