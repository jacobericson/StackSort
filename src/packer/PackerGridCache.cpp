#include "Packer.h"

namespace Packer
{

namespace Cache
{

bool GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth, int& outLerHeight,
                     int& outLerX, int& outLerY, double& outConcentration, int& outStrandedCells)
{
    unsigned long long hA       = ctx.cache.curHashA;
    const unsigned char* curPtr = &ctx.grid[0];

    for (int i = 0; i < ctx.cache.count; ++i)
    {
        const GridCacheEntry& e = ctx.cache.entries[i];
        if (e.hashA != hA) continue;
        outLerArea       = e.lerArea;
        outLerWidth      = e.lerWidth;
        outLerHeight     = e.lerHeight;
        outLerX          = e.lerX;
        outLerY          = e.lerY;
        outConcentration = e.concentration;
        outStrandedCells = e.strandedCells;
        return true;
    }

    Ler::ComputeLERCtx(ctx, curPtr, gridW, gridH, outLerArea, outLerWidth, outLerHeight, outLerX, outLerY);
    outStrandedCells = 0;
    outConcentration = Ler::ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, outLerX, outLerY, outLerWidth,
                                                               outLerHeight, outStrandedCells);

    int slot            = ctx.cache.ringHead;
    GridCacheEntry& dst = ctx.cache.entries[slot];
    dst.hashA           = hA;
    dst.lerArea         = outLerArea;
    dst.lerWidth        = outLerWidth;
    dst.lerHeight       = outLerHeight;
    dst.lerX            = outLerX;
    dst.lerY            = outLerY;
    dst.concentration   = outConcentration;
    dst.strandedCells   = outStrandedCells;
    ctx.cache.ringHead  = (ctx.cache.ringHead + 1) & 63;
    if (ctx.cache.count < 64) ++ctx.cache.count;

    return false;
}

} // namespace Cache

} // namespace Packer
