#include "Packer.h"

#include <climits>

bool Packer::Overlaps(const Rect& a, const Rect& b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

bool Packer::Contains(const Rect& outer, const Rect& inner)
{
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

void Packer::SplitFreeRects(PackContext& ctx, const Rect& placed)
{
    ctx.newRects.clear();

    for (size_t i = 0; i < ctx.freeRects.size(); ++i)
    {
        const Rect& fr = ctx.freeRects[i];

        if (!Overlaps(fr, placed))
        {
            ctx.newRects.push_back(fr);
            continue;
        }

        // Left remainder
        if (placed.x > fr.x)
        {
            Rect r;
            r.x = fr.x;
            r.y = fr.y;
            r.w = placed.x - fr.x;
            r.h = fr.h;
            ctx.newRects.push_back(r);
        }

        // Right remainder
        if (placed.x + placed.w < fr.x + fr.w)
        {
            Rect r;
            r.x = placed.x + placed.w;
            r.y = fr.y;
            r.w = (fr.x + fr.w) - (placed.x + placed.w);
            r.h = fr.h;
            ctx.newRects.push_back(r);
        }

        // Top remainder
        if (placed.y > fr.y)
        {
            Rect r;
            r.x = fr.x;
            r.y = fr.y;
            r.w = fr.w;
            r.h = placed.y - fr.y;
            ctx.newRects.push_back(r);
        }

        // Bottom remainder
        if (placed.y + placed.h < fr.y + fr.h)
        {
            Rect r;
            r.x = fr.x;
            r.y = placed.y + placed.h;
            r.w = fr.w;
            r.h = (fr.y + fr.h) - (placed.y + placed.h);
            ctx.newRects.push_back(r);
        }
    }

    ctx.freeRects.swap(ctx.newRects);
}

void Packer::PruneFreeRects(PackContext& ctx)
{
    size_t n = ctx.freeRects.size();
    ctx.dead.assign(n, false);

    for (size_t i = 0; i < n; ++i)
    {
        if (ctx.dead[i]) continue;
        for (size_t j = i + 1; j < n; ++j)
        {
            if (ctx.dead[j]) continue;
            if (Contains(ctx.freeRects[i], ctx.freeRects[j]))
            {
                ctx.dead[j] = true;
            }
            else if (Contains(ctx.freeRects[j], ctx.freeRects[i]))
            {
                ctx.dead[i] = true;
                break;
            }
        }
    }

    size_t write = 0;
    for (size_t read = 0; read < n; ++read)
    {
        if (!ctx.dead[read]) ctx.freeRects[write++] = ctx.freeRects[read];
    }
    ctx.freeRects.resize(write);
}

// aboveReserveY >= 0: only rects where item fits above that Y. < 0: all rects.

// BSSF: minimize min(rw, rh), then max(rw, rh), then combing.
void Packer::FindBestBSSF(const std::vector<Rect>& freeRects, const Item& item, int numOri, int aboveReserveY,
                          int& bestShortSide, int& bestLongSide, int& bestIndex, int& bestW, int& bestH,
                          bool& bestRotated)
{
    for (size_t fi = 0; fi < freeRects.size(); ++fi)
    {
        const Rect& fr = freeRects[fi];

        for (int ori = 0; ori < numOri; ++ori)
        {
            int tryW = (ori == 0) ? item.w : item.h;
            int tryH = (ori == 0) ? item.h : item.w;

            if (fr.w < tryW || fr.h < tryH) continue;
            if (aboveReserveY >= 0 && fr.y + tryH > aboveReserveY) continue;

            int rw        = fr.w - tryW;
            int rh        = fr.h - tryH;
            int shortSide = (rw < rh) ? rw : rh;
            int longSide  = (rw > rh) ? rw : rh;

            bool isBetter =
                (shortSide < bestShortSide) || (shortSide == bestShortSide && longSide < bestLongSide) ||
                (shortSide == bestShortSide && longSide == bestLongSide && tryW >= tryH && !(bestW >= bestH));

            if (isBetter)
            {
                bestShortSide = shortSide;
                bestLongSide  = longSide;
                bestIndex     = (int)fi;
                bestW         = tryW;
                bestH         = tryH;
                bestRotated   = (ori != 0);
            }
        }
    }
}

// BAF: minimize leftover area (rw * rh), then short side, then combing.
void Packer::FindBestBAF(const std::vector<Rect>& freeRects, const Item& item, int numOri, int aboveReserveY,
                         long long& bestArea, int& bestShortSide, int& bestIndex, int& bestW, int& bestH,
                         bool& bestRotated)
{
    for (size_t fi = 0; fi < freeRects.size(); ++fi)
    {
        const Rect& fr = freeRects[fi];

        for (int ori = 0; ori < numOri; ++ori)
        {
            int tryW = (ori == 0) ? item.w : item.h;
            int tryH = (ori == 0) ? item.h : item.w;

            if (fr.w < tryW || fr.h < tryH) continue;
            if (aboveReserveY >= 0 && fr.y + tryH > aboveReserveY) continue;

            int rw         = fr.w - tryW;
            int rh         = fr.h - tryH;
            long long area = (long long)rw * rh;
            int shortSide  = (rw < rh) ? rw : rh;

            bool isBetter = (area < bestArea) || (area == bestArea && shortSide < bestShortSide) ||
                            (area == bestArea && shortSide == bestShortSide && tryW >= tryH && !(bestW >= bestH));

            if (isBetter)
            {
                bestArea      = area;
                bestShortSide = shortSide;
                bestIndex     = (int)fi;
                bestW         = tryW;
                bestH         = tryH;
                bestRotated   = (ori != 0);
            }
        }
    }
}

void Packer::MaxRectsPack(PackContext& ctx, int gridW, int gridH, const std::vector<Item>& items, int target,
                          const volatile long* abortFlag, int reserveX, int reserveW, int heuristic)
{
    ctx.placements.clear();
    ctx.freeRects.clear();

    Rect initial;
    initial.x = 0;
    initial.y = 0;
    initial.w = gridW;
    initial.h = gridH;
    ctx.freeRects.push_back(initial);

    int reserveY = gridH - target;

    // Pre-reserve: carve out the reserved LER block from free rects.
    // Items can only be placed in the L-shaped complement.
    if (reserveW > 0)
    {
        Rect reserved;
        reserved.x = reserveX;
        reserved.y = reserveY;
        reserved.w = reserveW;
        reserved.h = target;
        SplitFreeRects(ctx, reserved);
        PruneFreeRects(ctx);
    }

    for (size_t idx = 0; idx < items.size(); ++idx)
    {
        if (abortFlag && *abortFlag != 0) return;

        const Item& item = items[idx];

        int bestIndex = -1;
        int bestW = 0, bestH = 0;
        bool bestRotated = false;

        bool tryRotate = item.canRotate && item.w != item.h;
        int numOri     = tryRotate ? 2 : 1;

        if (heuristic == 1)
        {
            // BAF: Best Area Fit
            long long bestArea = LLONG_MAX;
            int bestShortSide  = INT_MAX;

            // Pass 1: above reserve only (soft constraint)
            if (reserveW <= 0)
                FindBestBAF(ctx.freeRects, item, numOri, reserveY, bestArea, bestShortSide, bestIndex, bestW, bestH,
                            bestRotated);

            // Pass 2: anywhere
            if (bestIndex < 0)
            {
                bestArea      = LLONG_MAX;
                bestShortSide = INT_MAX;
                FindBestBAF(ctx.freeRects, item, numOri, -1, bestArea, bestShortSide, bestIndex, bestW, bestH,
                            bestRotated);
            }
        }
        else
        {
            // BSSF: Best Short Side Fit (default)
            int bestShortSide = INT_MAX;
            int bestLongSide  = INT_MAX;

            // Pass 1: above reserve only (soft constraint)
            // Skipped when reserveW > 0 — free rects already exclude reservation,
            // so the reserveY check would wrongly block the side strips.
            if (reserveW <= 0)
                FindBestBSSF(ctx.freeRects, item, numOri, reserveY, bestShortSide, bestLongSide, bestIndex, bestW,
                             bestH, bestRotated);

            // Pass 2: anywhere
            if (bestIndex < 0)
            {
                bestShortSide = INT_MAX;
                bestLongSide  = INT_MAX;
                FindBestBSSF(ctx.freeRects, item, numOri, -1, bestShortSide, bestLongSide, bestIndex, bestW, bestH,
                             bestRotated);
            }
        }

        if (bestIndex < 0) continue; // Item doesn't fit — skip

        Placement p;
        p.id      = item.id;
        p.exactId = item.exactId;
        p.x       = ctx.freeRects[bestIndex].x;
        p.y       = ctx.freeRects[bestIndex].y;
        p.w       = bestW;
        p.h       = bestH;
        p.rotated = bestRotated;
        ctx.placements.push_back(p);

        Rect placed;
        placed.x = p.x;
        placed.y = p.y;
        placed.w = p.w;
        placed.h = p.h;

        SplitFreeRects(ctx, placed);
        PruneFreeRects(ctx);
    }
}
