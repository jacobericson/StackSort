#pragma once

#include "Packer.h"

#include <algorithm>

namespace Packer
{

namespace Scoring
{

// Integer square root via Newton's method (no <cmath> dependency).
static inline int isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n;
    int y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// Long-long sibling. Used by applyGroupingPower's general path where the
// b^quarters intermediate can exceed int32 range.
static inline long long isqrt_ll(long long n)
{
    if (n <= 0) return 0;
    long long x = n;
    long long y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// b^(quarters/4). Fast paths keep q=4/6/8 byte-match with the legacy
// formula; general path nests two int isqrts so it always rounds down.
static inline long long applyGroupingPower(int b, int quarters)
{
    if (b <= 0 || quarters <= 0) return 0;

    switch (quarters)
    {
    case 4:
        return b; // b^1
    case 5:
        return (long long)b * isqrt(isqrt(b)); // b^1.25 — soft track
    case 6:
        return (long long)b * isqrt(b); // b^1.5 — DEFAULT, must match legacy bit-for-bit
    case 8:
        return (long long)b * b; // b^2
    default:
        break;
    }

    long long bq = 1;
    for (int i = 0; i < quarters; ++i)
        bq *= b;
    long long root2 = isqrt_ll(bq);
    return isqrt_ll(root2);
}

// 0..100 multiplier on the function tier. -1 on either side disables; equal
// returns 100; hardcoded cross-function pairs pull their value from ctx so
// ablation can zero them.
static inline int FunctionSimilarityPct(int funcA, int funcB, const PackContext& ctx)
{
    if (funcA < 0 || funcB < 0) return 0;
    if (funcA == funcB) return 100;

    int lo = funcA < funcB ? funcA : funcB;
    int hi = funcA < funcB ? funcB : funcA;

    if (lo == 3 && hi == 15) return ctx.grouping.funcSimFoodFoodRestricted;  // ITEM_FOOD ↔ ITEM_FOOD_RESTRICTED
    if (lo == 1 && hi == 12) return ctx.grouping.funcSimFirstaidRobotrepair; // ITEM_FIRSTAID ↔ ITEM_ROBOTREPAIR

    return 0;
}

// Max weight (0..100) across tiers where a and b match. Function tier is
// pre-multiplied by FunctionSimilarityPct so partial cross-function matches
// contribute less than same-function. Short-circuits on a 100-weight exact match.
static inline int PairWeight(const Item& a, const Item& b, const PackContext& ctx)
{
    int best = 0;

    if (a.exactId == b.exactId && a.exactId >= 0)
    {
        if (ctx.grouping.tierWeightExact >= 100) return ctx.grouping.tierWeightExact;
        best = std::max(best, ctx.grouping.tierWeightExact);
    }

    if (a.customGroupId >= 0 && a.customGroupId == b.customGroupId)
        best = std::max(best, ctx.grouping.tierWeightCustom);

    if (a.gameDataType >= 0 && a.gameDataType == b.gameDataType) best = std::max(best, ctx.grouping.tierWeightType);

    int simPct = FunctionSimilarityPct(a.itemFunction, b.itemFunction, ctx);
    if (simPct > 0)
    {
        int fnWeight = (ctx.grouping.tierWeightFunction * simPct + 50) / 100;
        best         = std::max(best, fnWeight);
    }

    // Any shared flag bit triggers the tier — currently bit 0 (food_crop)
    // and bit 1 (trade_item).
    if ((a.flagsMask & b.flagsMask) != 0) best = std::max(best, ctx.grouping.tierWeightFlags);

    return best;
}

static inline int uf_find(int* parent, int x)
{
    while (parent[x] != x)
    {
        parent[x] = parent[parent[x]];
        x         = parent[x];
    }
    return x;
}

static inline void uf_unite(int* parent, int a, int b)
{
    a = uf_find(parent, a);
    b = uf_find(parent, b);
    if (a != b) parent[a] = b;
}

} // namespace Scoring

namespace Geometry
{

// Corner filter suppresses 1-cell borders on large items (side > 2) so trivial
// corner-touches don't register as clustering. Flush bonus rewards full-side
// contact (the stronger hand-arranged-looking case).
static const int SHARED_BORDER_FLUSH_BONUS = 1;

inline int SharedBorder(const Placement& a, const Placement& b)
{
    // AABB early-out: if one rect's left edge is past the other's right edge
    // (or symmetric in y), the pair can't share a border. Strict > so
    // touching pairs (a.x + a.w == b.x, etc.) still reach the overlap logic.
    if (a.x > b.x + b.w || b.x > a.x + a.w || a.y > b.y + b.h || b.y > a.y + a.h) return 0;

    int overlapX1 = (a.x > b.x) ? a.x : b.x;
    int overlapX2 = ((a.x + a.w) < (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int overlapY1 = (a.y > b.y) ? a.y : b.y;
    int overlapY2 = ((a.y + a.h) < (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);

    int total = 0;

    // Horizontal border (a bottom touches b top, or vice versa)
    if (overlapX1 < overlapX2 && (a.y + a.h == b.y || b.y + b.h == a.y))
    {
        int sharedW   = overlapX2 - overlapX1;
        bool fullSide = (sharedW == a.w || sharedW == b.w);
        // Corner filter only for sides > 2: small items always count
        if (sharedW > 1 || a.w <= 2 || b.w <= 2)
        {
            total += sharedW;
            if (fullSide) total += SHARED_BORDER_FLUSH_BONUS;
        }
    }

    // Vertical border (a right touches b left, or vice versa)
    if (overlapY1 < overlapY2 && (a.x + a.w == b.x || b.x + b.w == a.x))
    {
        int sharedH   = overlapY2 - overlapY1;
        bool fullSide = (sharedH == a.h || sharedH == b.h);
        // Corner filter only for sides > 2: small items always count
        if (sharedH > 1 || a.h <= 2 || b.h <= 2)
        {
            total += sharedH;
            if (fullSide) total += SHARED_BORDER_FLUSH_BONUS;
        }
    }

    return total;
}

} // namespace Geometry

} // namespace Packer
