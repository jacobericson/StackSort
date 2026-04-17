#include "Packer.h"

#include <cstring>

static unsigned long long FnvHash64(const unsigned char* data, size_t n)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= (unsigned long long)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Second FNV-1a stream for twin-hash (see GridCacheEntry).
static unsigned long long FnvHash64_B(const unsigned char* data, size_t n)
{
    unsigned long long h = 0x84222325cbf29ce4ULL;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= (unsigned long long)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool Packer::GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth,
                             int& outLerHeight, int& outLerX, int& outLerY, double& outConcentration,
                             int& outStrandedCells)
{
    int totalCells        = gridW * gridH;
    unsigned long long hA = FnvHash64(&ctx.grid[0], (size_t)totalCells);
    unsigned long long hB = FnvHash64_B(&ctx.grid[0], (size_t)totalCells);

    for (int i = 0; i < ctx.gridCacheCount; ++i)
    {
        const GridCacheEntry& e = ctx.gridCache[i];
        if (e.hashA == hA && e.hashB == hB)
        {
            outLerArea       = e.lerArea;
            outLerWidth      = e.lerWidth;
            outLerHeight     = e.lerHeight;
            outLerX          = e.lerX;
            outLerY          = e.lerY;
            outConcentration = e.concentration;
            outStrandedCells = e.strandedCells;
            return true;
        }
    }

    ComputeLERCtx(ctx, &ctx.grid[0], gridW, gridH, outLerArea, outLerWidth, outLerHeight, outLerX, outLerY);
    outStrandedCells = 0;
    outConcentration = ComputeConcentrationAndStrandedCtx(ctx, gridW, gridH, outLerX, outLerY, outLerWidth,
                                                          outLerHeight, outStrandedCells);

    GridCacheEntry& slot = ctx.gridCache[ctx.gridCacheHead];
    slot.hashA           = hA;
    slot.hashB           = hB;
    slot.lerArea         = outLerArea;
    slot.lerWidth        = outLerWidth;
    slot.lerHeight       = outLerHeight;
    slot.lerX            = outLerX;
    slot.lerY            = outLerY;
    slot.concentration   = outConcentration;
    slot.strandedCells   = outStrandedCells;
    ctx.gridCacheHead    = (ctx.gridCacheHead + 1) & 63;
    if (ctx.gridCacheCount < 64) ++ctx.gridCacheCount;

    return false;
}
