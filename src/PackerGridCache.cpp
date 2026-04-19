#include "Packer.h"

#include <cstring>

bool Packer::GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth,
                             int& outLerHeight, int& outLerX, int& outLerY, double& outConcentration,
                             int& outStrandedCells)
{
    int totalCells              = gridW * gridH;
    unsigned long long hA       = ctx.curHashA;
    unsigned long long hB       = ctx.curHashB;
    const unsigned char* curPtr = &ctx.grid[0];
    unsigned char* blobPtr      = &ctx.gridCacheGridBlob[0];

    for (int i = 0; i < ctx.gridCacheCount; ++i)
    {
        const GridCacheEntry& e = ctx.gridCache[i];
        if (e.hashA != hA || e.hashB != hB) continue;
        // Memcmp guard against 2^-128 twin-Zobrist collision so a hash-match
        // never returns a stale tuple — preserves bit-exact score regardless
        // of which cache key function is in use.
        if (std::memcmp(blobPtr + (size_t)i * (size_t)totalCells, curPtr, (size_t)totalCells) != 0) continue;
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
    std::memcpy(blobPtr + (size_t)slot * (size_t)totalCells, curPtr, (size_t)totalCells);
    ctx.gridCacheHead = (ctx.gridCacheHead + 1) & 63;
    if (ctx.gridCacheCount < 64) ++ctx.gridCacheCount;

    return false;
}
