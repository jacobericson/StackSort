#include "Packer.h"

bool Packer::GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth,
                             int& outLerHeight, int& outLerX, int& outLerY, double& outConcentration,
                             int& outStrandedCells)
{
    unsigned long long hA       = ctx.curHashA;
    unsigned long long hB       = ctx.curHashB;
    const unsigned char* curPtr = &ctx.grid[0];

    // Bit-exactness rests on the 128-bit twin-Zobrist key. Birthday-bound
    // collision probability at 10^4-10^5 distinct grids per run is ~10^-31,
    // orders of magnitude below any other noise source — no memcmp guard.
    for (int i = 0; i < ctx.gridCacheCount; ++i)
    {
        const GridCacheEntry& e = ctx.gridCache[i];
        if (e.hashA != hA || e.hashB != hB) continue;
        outLerArea       = e.lerArea;
        outLerWidth      = e.lerWidth;
        outLerHeight     = e.lerHeight;
        outLerX          = e.lerX;
        outLerY          = e.lerY;
        outConcentration = e.concentration;
        outStrandedCells = e.strandedCells;
        return true;
    }

    ComputeLERCtx(ctx, curPtr, gridW, gridH, outLerArea, outLerWidth, outLerHeight, outLerX, outLerY);
    outStrandedCells = 0;
    outConcentration = ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, outLerX, outLerY, outLerWidth,
                                                          outLerHeight, outStrandedCells);

    int slot            = ctx.gridCacheHead;
    GridCacheEntry& dst = ctx.gridCache[slot];
    dst.hashA           = hA;
    dst.hashB           = hB;
    dst.lerArea         = outLerArea;
    dst.lerWidth        = outLerWidth;
    dst.lerHeight       = outLerHeight;
    dst.lerX            = outLerX;
    dst.lerY            = outLerY;
    dst.concentration   = outConcentration;
    dst.strandedCells   = outStrandedCells;
    ctx.gridCacheHead   = (ctx.gridCacheHead + 1) & 63;
    if (ctx.gridCacheCount < 64) ++ctx.gridCacheCount;

    return false;
}
