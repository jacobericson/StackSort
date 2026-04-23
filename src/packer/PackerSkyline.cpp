#include "Packer.h"
#include "PackerProfile.h"
#include "PackerScoringInline.h"

#include <climits>
#include <cstring>

namespace Packer
{

namespace Heuristics
{

namespace
{

// Maximum adjacent same-type placements to track per candidate position.
// Bounded by the number of items that can physically touch a single item.
const int MAX_ADJ = 16;

short SkylineAllocNode(PackContext& ctx)
{
    short idx;
    if (ctx.skyline.freeHead >= 0)
    {
        idx                  = ctx.skyline.freeHead;
        ctx.skyline.freeHead = ctx.skyline.next[idx];
    }
    else
    {
        idx = ctx.skyline.count++;
    }
    ctx.skyline.next[idx] = -1;
    return idx;
}

void SkylineFreeNode(PackContext& ctx, short idx)
{
    ctx.skyline.next[idx] = ctx.skyline.freeHead;
    ctx.skyline.freeHead  = idx;
}

void SkylineReset(PackContext& ctx, int gridW)
{
    ctx.skyline.head              = -1;
    ctx.skyline.freeHead          = -1;
    ctx.skyline.count             = 0;
    short head                    = SkylineAllocNode(ctx);
    ctx.skyline.nodes[head].x     = 0;
    ctx.skyline.nodes[head].y     = 0;
    ctx.skyline.nodes[head].width = gridW;
    ctx.skyline.head              = head;
}

// Commit-time boundary snapshot. Skyline, waste, and hash state are already
// up-to-date by the time this is called — the caller (SkylinePack commit
// paths) folds grid/zobrist updates into the same cell walk that stamps
// placementIdGrid, so EmitBoundary only copies the already-current state.
// placeX/placeY/placeW/placeH are retained in the signature for call-site
// clarity but are unused here.
void EmitBoundary(PackContext& ctx, int /*gridW*/, int /*placeX*/, int /*placeY*/, int /*placeW*/, int /*placeH*/)
{
    SkylineBoundary b;
    b.placementsCount = (int)ctx.placements.size();
    b.wasteStart      = (int)ctx.skyline.snapWaste.size();
    b.wasteCount      = (int)ctx.skyline.wasteRects.size();
    ctx.skyline.snapWaste.insert(ctx.skyline.snapWaste.end(), ctx.skyline.wasteRects.begin(),
                                 ctx.skyline.wasteRects.end());
    b.skylineStart   = (int)ctx.skyline.snapSkyline.size();
    int skylineCount = 0;
    for (short s = ctx.skyline.head; s >= 0; s = ctx.skyline.next[s])
    {
        ctx.skyline.snapSkyline.push_back(ctx.skyline.nodes[s]);
        ++skylineCount;
    }
    b.skylineCount = skylineCount;
    b.hashA        = ctx.cache.curHashA;
    b.hashB        = ctx.cache.curHashB;
    ctx.skyline.snapBoundaries.push_back(b);
}

void CollectAdjacentPids(const PackContext& ctx, const std::vector<Item>& /*items*/, int curExactId, int start,
                         int step, int count, int* adjPids, int& numAdj, int maxAdj)
{
    // -1 means "no tier"; such items don't peer with each other (matches
    // ComputeGroupingBonus's exA >= 0 test).
    if (curExactId < 0) return;
    for (int i = 0; i < count; ++i)
    {
        int pidx = (int)ctx.placementIdGrid[start + i * step];
        if (pidx == PLACEMENT_ID_EMPTY || ctx.placements[pidx].exactId != curExactId) continue;
        bool dup = false;
        for (int k = 0; k < numAdj; ++k)
            if (adjPids[k] == pidx)
            {
                dup = true;
                break;
            }
        if (!dup && numAdj < maxAdj) adjPids[numAdj++] = pidx;
    }
}

} // namespace

// Faster than MAXRECTS for the LAHC inner loop: O(n) state, O(n) per
// placement. Quality within ~2-5% per Jylänki.
// reserveW > 0: single-pass hard constraint (items can't overlap reserved rect).
// reserveW == 0: two-pass soft reserve (prefer above reserveY, fallback anywhere).
// Candidate walk computes maxY + areaUnder in one segment sweep with an
// early abort on maxY > maxAllowedY; waste is maxY * iw - areaUnder.

void SkylinePack(PackContext& ctx, const GridSpec& dims, const std::vector<Item>& items, const volatile long* abortFlag,
                 int reserveX, int reserveW, int startIdx)
{
    const int gridW  = dims.gridW;
    const int gridH  = dims.gridH;
    const int target = dims.target;
    int reserveY     = gridH - target;
    int totalCells   = gridW * gridH;

    if (startIdx == 0)
    {
        ctx.placements.clear();
        SkylineReset(ctx, gridW);

        ctx.skyline.wasteRects.clear();

        ctx.placementIdGrid.resize(totalCells);
        memset(&ctx.placementIdGrid[0], 0xFF, totalCells);

        // ctx.grid is maintained incrementally across commits + rollbacks so
        // callers (ComputeLERCtx, GridCacheLookup) can read it without a
        // full BuildOccupancyGrid rebuild. Reset on every cold start.
        ctx.grid.resize(totalCells);
        memset(&ctx.grid[0], 0, totalCells);

        ctx.skyline.snapBoundaries.clear();
        ctx.skyline.snapWaste.clear();
        ctx.skyline.snapSkyline.clear();
        memset(ctx.typeCount, 0, sizeof(ctx.typeCount));
        ctx.cache.curHashA = 0;
        ctx.cache.curHashB = 0;
        EmitBoundary(ctx, gridW, 0, 0, 0, 0);
    }
    else
    {
        memset(ctx.typeCount, 0, sizeof(ctx.typeCount));
        for (size_t i = 0; i < ctx.placements.size(); ++i)
        {
            int ex = ctx.placements[i].exactId;
            if (ex >= 0 && ex < 512) ++ctx.typeCount[ex];
        }
    }

#ifdef STACKSORT_PROFILE
    unsigned long long _sklStartTsc = __rdtsc();
    int _prefixK                    = ctx.prof.skylinePrefixK;
#endif

    for (size_t idx = (size_t)startIdx; idx < items.size(); ++idx)
    {
        if (abortFlag && *abortFlag != 0)
        {
            ctx.skyline.snapValid = false;
            return;
        }

#ifdef STACKSORT_PROFILE
        if ((int)idx == _prefixK)
        {
            unsigned long long _now = __rdtsc();
            ctx.prof.skylinePrefixCycles += (long long)(_now - _sklStartTsc);
        }
#endif

        const Item& item = items[idx];

        SUBPHASE_BEGIN(ws);
        bool tryRotateW   = item.canRotate && item.w != item.h;
        int numOriW       = tryRotateW ? 2 : 1;
        int bestWasteIdx  = -1;
        int bestWasteArea = INT_MAX;
        int wasteW = 0, wasteH = 0;
        bool wasteRotated = false;

        for (size_t wi = 0; wi < ctx.skyline.wasteRects.size(); ++wi)
        {
            const Rect& wr = ctx.skyline.wasteRects[wi];
            for (int ori = 0; ori < numOriW; ++ori)
            {
                int tw = (ori == 0) ? item.w : item.h;
                int th = (ori == 0) ? item.h : item.w;
                if (tw <= wr.w && th <= wr.h && wr.w * wr.h < bestWasteArea)
                {
                    bestWasteIdx  = (int)wi;
                    bestWasteArea = wr.w * wr.h;
                    wasteW        = tw;
                    wasteH        = th;
                    wasteRotated  = (ori != 0);
                }
            }
        }

        if (bestWasteIdx >= 0)
        {
            const Rect& wr = ctx.skyline.wasteRects[bestWasteIdx];
            Placement p;
            p.id      = item.id;
            p.exactId = item.exactId;
            p.x       = wr.x;
            p.y       = wr.y;
            p.w       = wasteW;
            p.h       = wasteH;
            p.rotated = wasteRotated;
            ctx.placements.push_back(p);
            if (p.exactId >= 0 && p.exactId < 512) ++ctx.typeCount[p.exactId];

            {
                int pidx                     = (int)ctx.placements.size() - 1;
                const unsigned long long* zA = &ctx.cache.zobristA[0];
                const unsigned long long* zB = &ctx.cache.zobristB[0];
                for (int dy = 0; dy < p.h; ++dy)
                    for (int dx = 0; dx < p.w; ++dx)
                    {
                        int cellIdx                  = (p.y + dy) * gridW + (p.x + dx);
                        ctx.placementIdGrid[cellIdx] = (unsigned char)pidx;
                        ctx.grid[cellIdx]            = 1;
                        ctx.cache.curHashA ^= zA[cellIdx];
                        ctx.cache.curHashB ^= zB[cellIdx];
                    }
            }

            int remRightW  = wr.w - wasteW;
            int remBottomH = wr.h - wasteH;
            int wrx = wr.x, wry = wr.y, wrh = wr.h;
            ctx.skyline.wasteRects.erase(ctx.skyline.wasteRects.begin() + bestWasteIdx);
            if (remRightW > 0)
            {
                Rect r;
                r.x = wrx + wasteW;
                r.y = wry;
                r.w = remRightW;
                r.h = wrh;
                ctx.skyline.wasteRects.push_back(r);
            }
            if (remBottomH > 0)
            {
                Rect r;
                r.x = wrx;
                r.y = wry + wasteH;
                r.w = wasteW;
                r.h = remBottomH;
                ctx.skyline.wasteRects.push_back(r);
            }
            EmitBoundary(ctx, gridW, p.x, p.y, p.w, p.h);
            SUBPHASE_END(ws, ctx.prof.cyclesSkylineWasteMap);
            continue;
        }
        SUBPHASE_END(ws, ctx.prof.cyclesSkylineWasteMap);

        bool tryRotate = item.canRotate && item.w != item.h;
        int numOri     = tryRotate ? 2 : 1;

        int bestY              = INT_MAX;
        long long bestCombined = LLONG_MAX;
        int bestX              = -1;
        int bestW = 0, bestH = 0;
        bool bestRotated = false;
        int curExactId   = items[idx].exactId;

        int numPasses = (reserveW > 0) ? 1 : 2;

        for (int pass = 0; pass < numPasses; ++pass)
        {
            if (pass == 1 && bestX >= 0) break;

            if (pass == 1)
            {
                bestY        = INT_MAX;
                bestCombined = LLONG_MAX;
            }

            for (int ori = 0; ori < numOri; ++ori)
            {
                int iw = (ori == 0) ? item.w : item.h;
                int ih = (ori == 0) ? item.h : item.w;

                for (short si = ctx.skyline.head; si >= 0; si = ctx.skyline.next[si])
                {
                    int x = ctx.skyline.nodes[si].x;
                    if (x + iw > gridW) continue;
                    SUBPHASE_BEGIN(cand);

                    int ceiling = gridH;
                    if (reserveW > 0)
                    {
                        if (x < reserveX + reserveW && x + iw > reserveX) ceiling = reserveY;
                    }
                    else if (pass == 0)
                    {
                        ceiling = reserveY;
                    }
                    int maxAllowedY = (ceiling - ih < bestY) ? (ceiling - ih) : bestY;
                    // Initial maxY==0 still needs the ceiling check — negative
                    // maxAllowedY means ih exceeds the per-x ceiling.
                    if (maxAllowedY < 0)
                    {
                        SUBPHASE_END(cand, ctx.prof.cyclesSkylineCandidate);
                        continue;
                    }

                    int maxY      = 0;
                    int areaUnder = 0;
                    bool aborted  = false;
                    bool covered  = false;

                    // Walk starts at sj == si where segLeft == x, and the
                    // skyline is contiguous with monotonically increasing
                    // segLeft thereafter. overlapLeft therefore always
                    // equals segLeft on every iteration, so overlapW
                    // simplifies to (overlapRight - segLeft). The inner
                    // loop runs until overlapRight covers x + iw (fit) or
                    // we walk off the skyline tail (no fit).
                    int xEnd = x + iw;
                    for (short sj = si; sj >= 0; sj = ctx.skyline.next[sj])
                    {
                        int segY = ctx.skyline.nodes[sj].y;
                        if (segY > maxY)
                        {
                            maxY = segY;
                            if (maxY > maxAllowedY)
                            {
                                aborted = true;
                                break;
                            }
                        }
                        int segLeft      = ctx.skyline.nodes[sj].x;
                        int segRight     = segLeft + ctx.skyline.nodes[sj].width;
                        int overlapRight = (xEnd < segRight) ? xEnd : segRight;
                        areaUnder += (overlapRight - segLeft) * segY;
                        if (overlapRight >= xEnd)
                        {
                            covered = true;
                            break;
                        }
                    }

                    if (aborted || !covered)
                    {
                        SUBPHASE_END(cand, ctx.prof.cyclesSkylineCandidate);
                        continue;
                    }

                    int waste = maxY * iw - areaUnder;

                    long long maxContact = 2LL * (iw + ih) + MAX_ADJ;
                    if (maxY == bestY && (long long)waste * ctx.skylineWasteCoef - maxContact > bestCombined)
                    {
                        SUBPHASE_END(cand, ctx.prof.cyclesSkylineCandidate);
                        continue;
                    }
                    SUBPHASE_END(cand, ctx.prof.cyclesSkylineCandidate);

                    SUBPHASE_BEGIN(adj);
                    int adjPids[MAX_ADJ];
                    int numAdj = 0;
                    if (curExactId >= 0 && curExactId < 512 && ctx.typeCount[curExactId] > 0)
                    {
                        if (x > 0)
                            CollectAdjacentPids(ctx, items, curExactId, maxY * gridW + (x - 1), gridW, ih, adjPids,
                                                numAdj, MAX_ADJ);
                        if (x + iw < gridW)
                            CollectAdjacentPids(ctx, items, curExactId, maxY * gridW + (x + iw), gridW, ih, adjPids,
                                                numAdj, MAX_ADJ);
                        if (maxY > 0)
                            CollectAdjacentPids(ctx, items, curExactId, (maxY - 1) * gridW + x, 1, iw, adjPids, numAdj,
                                                MAX_ADJ);
                        if (maxY + ih < gridH)
                            CollectAdjacentPids(ctx, items, curExactId, (maxY + ih) * gridW + x, 1, iw, adjPids, numAdj,
                                                MAX_ADJ);
                    }

                    Placement cand;
                    cand.id      = item.id;
                    cand.x       = x;
                    cand.y       = maxY;
                    cand.w       = iw;
                    cand.h       = ih;
                    cand.rotated = false;

                    int contact = 0;
                    for (int k = 0; k < numAdj; ++k)
                        contact += Geometry::SharedBorder(cand, ctx.placements[adjPids[k]]);

                    long long combined = (long long)waste * ctx.skylineWasteCoef - contact;

                    bool isBetter = (maxY < bestY) || (maxY == bestY && combined < bestCombined) ||
                                    (maxY == bestY && combined == bestCombined && iw >= ih && !(bestW >= bestH));

                    if (isBetter)
                    {
                        bestY        = maxY;
                        bestCombined = combined;
                        bestX        = x;
                        bestW        = iw;
                        bestH        = ih;
                        bestRotated  = (ori != 0);
                    }
                    SUBPHASE_END(adj, ctx.prof.cyclesSkylineAdjacency);
                }
            }
        }

        if (bestX < 0)
        {
            SUBPHASE_BEGIN(cmt);
            EmitBoundary(ctx, gridW, 0, 0, 0, 0);
            SUBPHASE_END(cmt, ctx.prof.cyclesSkylineCommit);
            continue;
        }

        SUBPHASE_BEGIN(cmt);
        Placement p;
        p.id      = item.id;
        p.exactId = item.exactId;
        p.x       = bestX;
        p.y       = bestY;
        p.w       = bestW;
        p.h       = bestH;
        p.rotated = bestRotated;
        ctx.placements.push_back(p);
        if (p.exactId >= 0 && p.exactId < 512) ++ctx.typeCount[p.exactId];

        {
            int pidx                     = (int)ctx.placements.size() - 1;
            const unsigned long long* zA = &ctx.cache.zobristA[0];
            const unsigned long long* zB = &ctx.cache.zobristB[0];
            for (int dy = 0; dy < bestH; ++dy)
                for (int dx = 0; dx < bestW; ++dx)
                {
                    int cellIdx                  = (bestY + dy) * gridW + (bestX + dx);
                    ctx.placementIdGrid[cellIdx] = (unsigned char)pidx;
                    ctx.grid[cellIdx]            = 1;
                    ctx.cache.curHashA ^= zA[cellIdx];
                    ctx.cache.curHashB ^= zB[cellIdx];
                }
        }

        int placeLeft  = bestX;
        int placeRight = bestX + bestW;
        int placeTop   = bestY + bestH;

        for (short sj = ctx.skyline.head; sj >= 0; sj = ctx.skyline.next[sj])
        {
            int segLeft  = ctx.skyline.nodes[sj].x;
            int segRight = segLeft + ctx.skyline.nodes[sj].width;
            if (segRight <= placeLeft || segLeft >= placeRight) continue;

            int overlapLeft  = (placeLeft > segLeft) ? placeLeft : segLeft;
            int overlapRight = (placeRight < segRight) ? placeRight : segRight;
            int overlapW     = overlapRight - overlapLeft;
            int gapH         = bestY - ctx.skyline.nodes[sj].y;
            if (overlapW > 0 && gapH > 0)
            {
                Rect waste;
                waste.x = overlapLeft;
                waste.y = ctx.skyline.nodes[sj].y;
                waste.w = overlapW;
                waste.h = gapH;
                ctx.skyline.wasteRects.push_back(waste);
            }
        }

        // In-place splice: walk overlapping nodes, shrink/split/unlink as
        // needed, then insert itemSeg at its x-order position and coalesce
        // with same-y neighbours. Prev walks with cur so unlinks relink
        // prev->next in O(1).
        short insertAfter = -1;
        short cur         = ctx.skyline.head;
        short prev        = -1;
        while (cur >= 0)
        {
            int segLeft  = ctx.skyline.nodes[cur].x;
            int segRight = segLeft + ctx.skyline.nodes[cur].width;
            short next   = ctx.skyline.next[cur];

            if (segRight <= placeLeft)
            {
                insertAfter = cur;
                prev        = cur;
                cur         = next;
                continue;
            }
            if (segLeft >= placeRight)
            {
                break;
            }

            bool hasLeft  = segLeft < placeLeft;
            bool hasRight = segRight > placeRight;

            if (hasLeft && hasRight)
            {
                ctx.skyline.nodes[cur].width      = placeLeft - segLeft;
                short rightIdx                    = SkylineAllocNode(ctx);
                ctx.skyline.nodes[rightIdx].x     = placeRight;
                ctx.skyline.nodes[rightIdx].y     = ctx.skyline.nodes[cur].y;
                ctx.skyline.nodes[rightIdx].width = segRight - placeRight;
                ctx.skyline.next[rightIdx]        = next;
                ctx.skyline.next[cur]             = rightIdx;
                insertAfter                       = cur;
                prev                              = rightIdx;
                cur                               = next;
            }
            else if (hasLeft)
            {
                ctx.skyline.nodes[cur].width = placeLeft - segLeft;
                insertAfter                  = cur;
                prev                         = cur;
                cur                          = next;
            }
            else if (hasRight)
            {
                ctx.skyline.nodes[cur].x     = placeRight;
                ctx.skyline.nodes[cur].width = segRight - placeRight;
                prev                         = cur;
                cur                          = next;
            }
            else
            {
                if (prev < 0) ctx.skyline.head = next;
                else ctx.skyline.next[prev] = next;
                SkylineFreeNode(ctx, cur);
                cur = next;
            }
        }

        short itemSeg                    = SkylineAllocNode(ctx);
        ctx.skyline.nodes[itemSeg].x     = placeLeft;
        ctx.skyline.nodes[itemSeg].y     = placeTop;
        ctx.skyline.nodes[itemSeg].width = bestW;

        if (insertAfter < 0)
        {
            ctx.skyline.next[itemSeg] = ctx.skyline.head;
            ctx.skyline.head          = itemSeg;
        }
        else
        {
            ctx.skyline.next[itemSeg]     = ctx.skyline.next[insertAfter];
            ctx.skyline.next[insertAfter] = itemSeg;
        }

        if (insertAfter >= 0 && ctx.skyline.nodes[insertAfter].y == placeTop &&
            ctx.skyline.nodes[insertAfter].x + ctx.skyline.nodes[insertAfter].width == placeLeft)
        {
            ctx.skyline.nodes[insertAfter].width += ctx.skyline.nodes[itemSeg].width;
            ctx.skyline.next[insertAfter] = ctx.skyline.next[itemSeg];
            SkylineFreeNode(ctx, itemSeg);
            itemSeg = insertAfter;
        }

        short succ = ctx.skyline.next[itemSeg];
        if (succ >= 0 && ctx.skyline.nodes[succ].y == ctx.skyline.nodes[itemSeg].y &&
            ctx.skyline.nodes[itemSeg].x + ctx.skyline.nodes[itemSeg].width == ctx.skyline.nodes[succ].x)
        {
            ctx.skyline.nodes[itemSeg].width += ctx.skyline.nodes[succ].width;
            ctx.skyline.next[itemSeg] = ctx.skyline.next[succ];
            SkylineFreeNode(ctx, succ);
        }

        EmitBoundary(ctx, gridW, bestX, bestY, bestW, bestH);
        SUBPHASE_END(cmt, ctx.prof.cyclesSkylineCommit);
    }

    ctx.skyline.snapN     = (int)items.size();
    ctx.skyline.snapValid = true;
}

} // namespace Heuristics

} // namespace Packer
