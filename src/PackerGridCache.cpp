#include "Packer.h"

#include <cstring>

static inline unsigned long long rotl64(unsigned long long x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static inline unsigned long long fmix64(unsigned long long k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

// Austin Appleby's MurmurHash3_x64_128 (public domain). x64 little-endian
// assumed; memcpy loads stay UB-free on unaligned storage.
static void MurmurHash3_x64_128(const unsigned char* key, size_t len, unsigned long long* outA,
                                unsigned long long* outB)
{
    const unsigned long long c1 = 0x87c37b91114253d5ULL;
    const unsigned long long c2 = 0x4cf5ad432745937fULL;
    unsigned long long h1       = 0;
    unsigned long long h2       = 0;

    const size_t nblocks = len / 16;
    for (size_t i = 0; i < nblocks; ++i)
    {
        unsigned long long k1, k2;
        std::memcpy(&k1, key + i * 16, 8);
        std::memcpy(&k2, key + i * 16 + 8, 8);

        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729ULL;

        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5ULL;
    }

    const unsigned char* tail = key + nblocks * 16;
    unsigned long long k1     = 0;
    unsigned long long k2     = 0;
    switch (len & 15)
    {
    case 15:
        k2 ^= (unsigned long long)tail[14] << 48; // fallthrough
    case 14:
        k2 ^= (unsigned long long)tail[13] << 40; // fallthrough
    case 13:
        k2 ^= (unsigned long long)tail[12] << 32; // fallthrough
    case 12:
        k2 ^= (unsigned long long)tail[11] << 24; // fallthrough
    case 11:
        k2 ^= (unsigned long long)tail[10] << 16; // fallthrough
    case 10:
        k2 ^= (unsigned long long)tail[9] << 8; // fallthrough
    case 9:
        k2 ^= (unsigned long long)tail[8];
        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        // fallthrough
    case 8:
        k1 ^= (unsigned long long)tail[7] << 56; // fallthrough
    case 7:
        k1 ^= (unsigned long long)tail[6] << 48; // fallthrough
    case 6:
        k1 ^= (unsigned long long)tail[5] << 40; // fallthrough
    case 5:
        k1 ^= (unsigned long long)tail[4] << 32; // fallthrough
    case 4:
        k1 ^= (unsigned long long)tail[3] << 24; // fallthrough
    case 3:
        k1 ^= (unsigned long long)tail[2] << 16; // fallthrough
    case 2:
        k1 ^= (unsigned long long)tail[1] << 8; // fallthrough
    case 1:
        k1 ^= (unsigned long long)tail[0];
        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        break;
    default:
        break;
    }

    h1 ^= (unsigned long long)len;
    h2 ^= (unsigned long long)len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    *outA = h1;
    *outB = h2;
}

bool Packer::GridCacheLookup(PackContext& ctx, int gridW, int gridH, int& outLerArea, int& outLerWidth,
                             int& outLerHeight, int& outLerX, int& outLerY, double& outConcentration,
                             int& outStrandedCells)
{
    int totalCells = gridW * gridH;
    unsigned long long hA, hB;
    MurmurHash3_x64_128(&ctx.grid[0], (size_t)totalCells, &hA, &hB);

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
