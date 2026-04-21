#include "Packer.h"
#include "PackerLAHCInternals.h"

#include <algorithm>
#include <cstring>

namespace Packer
{

namespace Search
{

void TryRepairMove(PackContext& ctx, int gridW, int gridH, std::vector<Item>& curOrder, int n, Move& move, LCG& rng,
                   int bestLerX, int bestLerY, int bestLerW, int bestLerH, int bestStranded,
                   std::vector<unsigned char>& repairGrid, std::vector<unsigned char>& repairReachable,
                   std::vector<int>& repairStrandedList, bool& repairGridDirty, int totalCellsRepair,
                   int effLateBiasAlphaQ, int effLateBiasUniformPct, int& diagRepairMoveScans, int& diagRepairMoveHits)
{
    // Caller guarantees n >= 3; guard makes it visible to clang-analyzer.
    if (n < 2) return;

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
            Grid::FloodFillFromLer(ctx, gridW, gridH, bestLerX, bestLerY, bestLerW, bestLerH, &repairGrid[0]);
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
            ctx.ler.floodStack.clear();
            ctx.ler.floodStack.push_back(strandedY * gridW + strandedX);
            ctx.visited[strandedY * gridW + strandedX] = 1;
            int gapCells                               = 0;
            while (!ctx.ler.floodStack.empty() && gapCells < 16)
            {
                int ci = ctx.ler.floodStack.back();
                ctx.ler.floodStack.pop_back();
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
                    ctx.ler.floodStack.push_back(ci - 1);
                }
                if (cx < gridW - 1 && !ctx.visited[ci + 1] && !repairGrid[ci + 1])
                {
                    ctx.visited[ci + 1] = 1;
                    ctx.ler.floodStack.push_back(ci + 1);
                }
                if (cy > 0 && !ctx.visited[ci - gridW] && !repairGrid[ci - gridW])
                {
                    ctx.visited[ci - gridW] = 1;
                    ctx.ler.floodStack.push_back(ci - gridW);
                }
                if (cy < gridH - 1 && !ctx.visited[ci + gridW] && !repairGrid[ci + gridW])
                {
                    ctx.visited[ci + gridW] = 1;
                    ctx.ler.floodStack.push_back(ci + gridW);
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

                bool fitsNormal  = (curOrder[ki].w <= gapW && curOrder[ki].h <= gapH);
                bool fitsRotated = (curOrder[ki].canRotate && curOrder[ki].h <= gapW && curOrder[ki].w <= gapH);
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

} // namespace Search

} // namespace Packer
