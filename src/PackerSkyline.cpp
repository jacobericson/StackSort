#include "Packer.h"

#include <climits>
#include <cstring>

#ifdef STACKSORT_PROFILE
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

// Maximum adjacent same-type placements to track per candidate position.
// Bounded by the number of items that can physically touch a single item.
static const int MAX_ADJ = 16;

// placeW == 0 is the no-placement sentinel; boundaries must still advance
// in lock-step with idx for unfittable items so index = placements.size()+
// unfittable_count invariant holds.
static void EmitBoundary(Packer::PackContext& ctx, int gridW, int placeX, int placeY, int placeW, int placeH)
{
    Packer::SkylineBoundary b;
    b.placementsCount = (int)ctx.placements.size();
    b.wasteStart      = (int)ctx.skylineSnapWaste.size();
    b.wasteCount      = (int)ctx.wasteRects.size();
    ctx.skylineSnapWaste.insert(ctx.skylineSnapWaste.end(), ctx.wasteRects.begin(), ctx.wasteRects.end());
    b.skylineStart = (int)ctx.skylineSnapSkyline.size();
    b.skylineCount = (int)ctx.skyline.size();
    ctx.skylineSnapSkyline.insert(ctx.skylineSnapSkyline.end(), ctx.skyline.begin(), ctx.skyline.end());
    b.gridDeltaStart = (int)ctx.skylineSnapGridDelta.size();
    if (placeW > 0)
    {
        for (int dy = 0; dy < placeH; ++dy)
            for (int dx = 0; dx < placeW; ++dx)
                ctx.skylineSnapGridDelta.push_back((placeY + dy) * gridW + (placeX + dx));
    }
    b.gridDeltaCount = (int)ctx.skylineSnapGridDelta.size() - b.gridDeltaStart;
    ctx.skylineSnapBoundaries.push_back(b);
}

void Packer::CollectAdjacentPids(const PackContext& ctx, const std::vector<Item>& items, int curType, int start,
                                 int step, int count, int* adjPids, int& numAdj, int maxAdj)
{
    for (int i = 0; i < count; ++i)
    {
        int pidx = ctx.placementIdGrid[start + i * step];
        if (pidx < 0 || ctx.placements[pidx].itemTypeId != curType) continue;
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

// Faster than MAXRECTS for the LAHC inner loop: O(n) state, O(n) per
// placement. Quality within ~2-5% per Jylänki.
// reserveW > 0: single-pass hard constraint (items can't overlap reserved rect).
// reserveW == 0: two-pass soft reserve (prefer above reserveY, fallback anywhere).

void Packer::SkylinePack(PackContext& ctx, int gridW, int gridH, const std::vector<Item>& items, int target,
                         volatile long* abortFlag, int reserveX, int reserveW, int startIdx)
{
    int reserveY   = gridH - target;
    int totalCells = gridW * gridH;

    if (startIdx == 0)
    {
        ctx.placements.clear();
        ctx.skyline.clear();

        SkylineNode initial;
        initial.x     = 0;
        initial.y     = 0;
        initial.width = gridW;
        ctx.skyline.push_back(initial);

        ctx.wasteRects.clear();

        // Empty sentinel is -1 via 0xFF memset; CollectAdjacentPids treats
        // any non-negative value as a real placement index.
        ctx.placementIdGrid.resize(totalCells);
        memset(&ctx.placementIdGrid[0], 0xFF, totalCells * sizeof(int));

        ctx.skylineSnapBoundaries.clear();
        ctx.skylineSnapWaste.clear();
        ctx.skylineSnapSkyline.clear();
        ctx.skylineSnapGridDelta.clear();
        EmitBoundary(ctx, gridW, 0, 0, 0, 0);
    }

#ifdef STACKSORT_PROFILE
    // Measures cycles spent on items [0..profSkylinePrefixK-1]. Those would
    // be skipped by a Phase-3-style partial recompute. Set prefixK<=0 to
    // skip the measurement (cold seed / restart seed calls).
    unsigned long long _sklStartTsc = __rdtsc();
    int _prefixK                    = ctx.profSkylinePrefixK;
#endif

    for (size_t idx = (size_t)startIdx; idx < items.size(); ++idx)
    {
        if (abortFlag && *abortFlag != 0)
        {
            ctx.skylineSnapValid = false;
            return;
        }

#ifdef STACKSORT_PROFILE
        if ((int)idx == _prefixK)
        {
            unsigned long long _now = __rdtsc();
            ctx.profSkylinePrefixCycles += (long long)(_now - _sklStartTsc);
        }
#endif

        const Item& item = items[idx];

        // Waste map: try placing in under-cliff gaps first
        bool tryRotateW   = item.canRotate && item.w != item.h;
        int numOriW       = tryRotateW ? 2 : 1;
        int bestWasteIdx  = -1;
        int bestWasteArea = INT_MAX;
        int wasteW = 0, wasteH = 0;
        bool wasteRotated = false;

        for (size_t wi = 0; wi < ctx.wasteRects.size(); ++wi)
        {
            const Rect& wr = ctx.wasteRects[wi];
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
            const Rect& wr = ctx.wasteRects[bestWasteIdx];
            Placement p;
            p.id         = item.id;
            p.itemTypeId = item.itemTypeId;
            p.x          = wr.x;
            p.y          = wr.y;
            p.w          = wasteW;
            p.h          = wasteH;
            p.rotated    = wasteRotated;
            ctx.placements.push_back(p);

            // Fill placement ID grid for contact-point scoring
            {
                int pidx = (int)ctx.placements.size() - 1;
                for (int dy = 0; dy < p.h; ++dy)
                    for (int dx = 0; dx < p.w; ++dx)
                        ctx.placementIdGrid[(p.y + dy) * gridW + (p.x + dx)] = pidx;
            }

            // Guillotine split: right remainder (full height), bottom (item width)
            int remRightW  = wr.w - wasteW;
            int remBottomH = wr.h - wasteH;
            int wrx = wr.x, wry = wr.y, wrw = wr.w, wrh = wr.h;
            ctx.wasteRects.erase(ctx.wasteRects.begin() + bestWasteIdx);
            if (remRightW > 0)
            {
                Rect r;
                r.x = wrx + wasteW;
                r.y = wry;
                r.w = remRightW;
                r.h = wrh;
                ctx.wasteRects.push_back(r);
            }
            if (remBottomH > 0)
            {
                Rect r;
                r.x = wrx;
                r.y = wry + wasteH;
                r.w = wasteW;
                r.h = remBottomH;
                ctx.wasteRects.push_back(r);
            }
            EmitBoundary(ctx, gridW, p.x, p.y, p.w, p.h);
            continue; // placed in waste — skip skyline search
        }

        bool tryRotate = item.canRotate && item.w != item.h;
        int numOri     = tryRotate ? 2 : 1;

        // Find best position: minimize y (bottom-left), then minimize waste.
        // When reserveW > 0: single pass with per-position ceiling constraint.
        // When reserveW == 0: two-pass (pass 0 = above reserve, pass 1 = anywhere).
        int bestY              = INT_MAX;
        long long bestCombined = LLONG_MAX;
        int bestX              = -1;
        int bestSi             = -1;
        int bestW = 0, bestH = 0;
        bool bestRotated = false;
        int curType      = items[idx].itemTypeId;

        int numPasses = (reserveW > 0) ? 1 : 2;

        for (int pass = 0; pass < numPasses; ++pass)
        {
            if (pass == 1 && bestX >= 0) break; // pass 0 found something

            if (pass == 1)
            {
                bestY        = INT_MAX;
                bestCombined = LLONG_MAX;
            }

            for (int ori = 0; ori < numOri; ++ori)
            {
                int iw = (ori == 0) ? item.w : item.h;
                int ih = (ori == 0) ? item.h : item.w;

                // Try each skyline node as a candidate left edge
                for (size_t si = 0; si < ctx.skyline.size(); ++si)
                {
                    int x = ctx.skyline[si].x;
                    if (x + iw > gridW) continue;

                    // Find the maximum Y across all skyline segments this item spans
                    int maxY         = 0;
                    int waste        = 0;
                    int widthCovered = 0;
                    bool fits        = true;

                    for (size_t sj = si; sj < ctx.skyline.size() && widthCovered < iw; ++sj)
                    {
                        int segY = ctx.skyline[sj].y;
                        if (segY > maxY) maxY = segY;

                        int segLeft      = ctx.skyline[sj].x;
                        int segRight     = segLeft + ctx.skyline[sj].width;
                        int overlapLeft  = (x > segLeft) ? x : segLeft;
                        int overlapRight = ((x + iw) < segRight) ? (x + iw) : segRight;
                        int overlapW     = overlapRight - overlapLeft;
                        if (overlapW <= 0) break;

                        widthCovered += overlapW;
                    }

                    if (widthCovered < iw) continue;

                    // Height constraint: hard ceiling depends on reservation
                    if (reserveW > 0)
                    {
                        // Items overlapping the reserved columns are capped at reserveY
                        int ceiling = gridH;
                        if (x < reserveX + reserveW && x + iw > reserveX) ceiling = reserveY;
                        if (maxY + ih > ceiling) continue;
                    }
                    else
                    {
                        // No reservation: check grid bounds + soft reserve
                        if (maxY + ih > gridH) continue;
                        if (pass == 0 && maxY + ih > reserveY) continue;
                    }

                    // Compute waste: gap area between skyline and item bottom
                    waste        = 0;
                    widthCovered = 0;
                    for (size_t sj = si; sj < ctx.skyline.size() && widthCovered < iw; ++sj)
                    {
                        int segLeft      = ctx.skyline[sj].x;
                        int segRight     = segLeft + ctx.skyline[sj].width;
                        int overlapLeft  = (x > segLeft) ? x : segLeft;
                        int overlapRight = ((x + iw) < segRight) ? (x + iw) : segRight;
                        int overlapW     = overlapRight - overlapLeft;
                        if (overlapW <= 0) break;

                        waste += overlapW * (maxY - ctx.skyline[sj].y);
                        widthCovered += overlapW;
                    }

                    // Contact scoring: find adjacent same-type placements
                    // via grid, then compute SharedBorder sum (corner filter
                    // + flush bonus). Used at 1/4 value in waste tradeoff.
                    int adjPids[MAX_ADJ];
                    int numAdj = 0;
                    if (x > 0)
                        CollectAdjacentPids(ctx, items, curType, maxY * gridW + (x - 1), gridW, ih, adjPids, numAdj,
                                            MAX_ADJ);
                    if (x + iw < gridW)
                        CollectAdjacentPids(ctx, items, curType, maxY * gridW + (x + iw), gridW, ih, adjPids, numAdj,
                                            MAX_ADJ);
                    if (maxY > 0)
                        CollectAdjacentPids(ctx, items, curType, (maxY - 1) * gridW + x, 1, iw, adjPids, numAdj,
                                            MAX_ADJ);
                    if (maxY + ih < gridH)
                        CollectAdjacentPids(ctx, items, curType, (maxY + ih) * gridW + x, 1, iw, adjPids, numAdj,
                                            MAX_ADJ);

                    Placement cand;
                    cand.id      = item.id;
                    cand.x       = x;
                    cand.y       = maxY;
                    cand.w       = iw;
                    cand.h       = ih;
                    cand.rotated = false;

                    int contact = 0;
                    for (int k = 0; k < numAdj; ++k)
                        contact += SharedBorder(cand, ctx.placements[adjPids[k]]);

                    // Best = lowest Y, then combined waste-contact score,
                    // then prefer wide orientation.
                    // Contact = SharedBorder sum. coef contact points offset
                    // 1 waste cell. coef defaults to 3 (1/3 value); tunable
                    // via SearchParams.skylineWasteCoef → ctx.skylineWasteCoef.
                    // long long so (waste * coef) can't overflow int; bestCombined uses LLONG_MAX.
                    long long combined = (long long)waste * ctx.skylineWasteCoef - contact;

                    bool isBetter = (maxY < bestY) || (maxY == bestY && combined < bestCombined) ||
                                    (maxY == bestY && combined == bestCombined && iw >= ih && !(bestW >= bestH));

                    if (isBetter)
                    {
                        bestY        = maxY;
                        bestCombined = combined;
                        bestX        = x;
                        bestSi       = (int)si;
                        bestW        = iw;
                        bestH        = ih;
                        bestRotated  = (ori != 0);
                    }
                }
            }
        }

        if (bestX < 0)
        {
            EmitBoundary(ctx, gridW, 0, 0, 0, 0);
            continue; // Item doesn't fit
        }

        // Place item
        Placement p;
        p.id         = item.id;
        p.itemTypeId = item.itemTypeId;
        p.x          = bestX;
        p.y          = bestY;
        p.w          = bestW;
        p.h          = bestH;
        p.rotated    = bestRotated;
        ctx.placements.push_back(p);

        // Fill placement ID grid for contact-point scoring
        {
            int pidx = (int)ctx.placements.size() - 1;
            for (int dy = 0; dy < bestH; ++dy)
                for (int dx = 0; dx < bestW; ++dx)
                    ctx.placementIdGrid[(bestY + dy) * gridW + (bestX + dx)] = pidx;
        }

        // Detect under-cliff waste: gaps between the item bottom (bestY)
        // and the skyline segments it covers. These become secondary free rects.
        int placeLeft  = bestX;
        int placeRight = bestX + bestW;
        int placeTop   = bestY + bestH;

        for (size_t sj = 0; sj < ctx.skyline.size(); ++sj)
        {
            int segLeft  = ctx.skyline[sj].x;
            int segRight = segLeft + ctx.skyline[sj].width;
            if (segRight <= placeLeft || segLeft >= placeRight) continue;

            int overlapLeft  = (placeLeft > segLeft) ? placeLeft : segLeft;
            int overlapRight = (placeRight < segRight) ? placeRight : segRight;
            int overlapW     = overlapRight - overlapLeft;
            int gapH         = bestY - ctx.skyline[sj].y;
            if (overlapW > 0 && gapH > 0)
            {
                Rect waste;
                waste.x = overlapLeft;
                waste.y = ctx.skyline[sj].y;
                waste.w = overlapW;
                waste.h = gapH;
                ctx.wasteRects.push_back(waste);
            }
        }

        // Update skyline: replace covered segments with one new segment at
        // the top of the placed item, then re-add any partial segments at
        // the edges.

        // Rebuild skyline using ctx.skylineTmp as scratch
        ctx.skylineTmp.clear();

        for (size_t si = 0; si < ctx.skyline.size(); ++si)
        {
            int segLeft  = ctx.skyline[si].x;
            int segRight = segLeft + ctx.skyline[si].width;

            if (segRight <= placeLeft || segLeft >= placeRight)
            {
                ctx.skylineTmp.push_back(ctx.skyline[si]);
            }
            else
            {
                if (segLeft < placeLeft)
                {
                    SkylineNode left;
                    left.x     = segLeft;
                    left.y     = ctx.skyline[si].y;
                    left.width = placeLeft - segLeft;
                    ctx.skylineTmp.push_back(left);
                }
                if (segRight > placeRight)
                {
                    SkylineNode right;
                    right.x     = placeRight;
                    right.y     = ctx.skyline[si].y;
                    right.width = segRight - placeRight;
                    ctx.skylineTmp.push_back(right);
                }
            }
        }

        // Insert the placed item's top edge (maintain x-sorted order)
        SkylineNode itemSeg;
        itemSeg.x     = placeLeft;
        itemSeg.y     = placeTop;
        itemSeg.width = bestW;

        size_t insertPos = 0;
        while (insertPos < ctx.skylineTmp.size() && ctx.skylineTmp[insertPos].x < placeLeft)
            ++insertPos;
        ctx.skylineTmp.insert(ctx.skylineTmp.begin() + insertPos, itemSeg);

        // Merge adjacent segments with equal Y
        ctx.skyline.clear();
        for (size_t si = 0; si < ctx.skylineTmp.size(); ++si)
        {
            if (!ctx.skyline.empty() && ctx.skyline.back().y == ctx.skylineTmp[si].y &&
                ctx.skyline.back().x + ctx.skyline.back().width == ctx.skylineTmp[si].x)
            {
                ctx.skyline.back().width += ctx.skylineTmp[si].width;
            }
            else
            {
                ctx.skyline.push_back(ctx.skylineTmp[si]);
            }
        }

        EmitBoundary(ctx, gridW, bestX, bestY, bestW, bestH);
    }

    ctx.skylineSnapN     = (int)items.size();
    ctx.skylineSnapValid = true;
}
