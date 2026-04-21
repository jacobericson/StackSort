#include "Packer.h"
#include "PackerPostPackInline.h"

#include <cstring>

// Swap a single placement X with a rectangular union of multiple placements G
// elsewhere. Same-orientation cases translate G en masse into X's old footprint
// (tiling trivially preserved). Rotated cases run a corner-first backtracking
// tiler over the destination rectangle; at k ≤ 10 pieces this outperforms DLX
// (no sparse-matrix setup). Cell occupancy preserved by construction.

namespace
{
struct TileItem
{
    int placementIdx; // index into outer placements[] (group member)
    int w;
    int h;
    int rotw; // placed w (may equal h if rotated)
    int roth;
    int destX; // destination top-left in X's old footprint
    int destY;
    bool canRotate;
    bool placed;
};

struct TileCtx
{
    int regionW;
    int regionH;
    unsigned char* filled; // regionW*regionH, 1 = covered
    TileItem* pieces;
    int numPieces;
};

bool TileBacktrack(TileCtx& tc)
{
    // Find top-left empty cell. Row-major scan; row 0 = min y in region frame.
    int cx = -1, cy = -1;
    for (int ry = 0; ry < tc.regionH && cx < 0; ++ry)
    {
        for (int rx = 0; rx < tc.regionW; ++rx)
        {
            if (!tc.filled[ry * tc.regionW + rx])
            {
                cx = rx;
                cy = ry;
                break;
            }
        }
    }
    if (cx < 0) return true; // fully covered

    for (int i = 0; i < tc.numPieces; ++i)
    {
        if (tc.pieces[i].placed) continue;
        // Try up to two orientations.
        int orients[2][2];
        int numOri    = 1;
        orients[0][0] = tc.pieces[i].w;
        orients[0][1] = tc.pieces[i].h;
        if (tc.pieces[i].canRotate && tc.pieces[i].w != tc.pieces[i].h)
        {
            orients[1][0] = tc.pieces[i].h;
            orients[1][1] = tc.pieces[i].w;
            numOri        = 2;
        }

        for (int oi = 0; oi < numOri; ++oi)
        {
            int pw = orients[oi][0];
            int ph = orients[oi][1];
            if (cx + pw > tc.regionW || cy + ph > tc.regionH) continue;

            // Test all cells free.
            bool ok = true;
            for (int dy = 0; dy < ph && ok; ++dy)
            {
                int rowOff = (cy + dy) * tc.regionW;
                for (int dx = 0; dx < pw; ++dx)
                {
                    if (tc.filled[rowOff + (cx + dx)])
                    {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) continue;

            // Commit + recurse.
            for (int dy = 0; dy < ph; ++dy)
            {
                int rowOff = (cy + dy) * tc.regionW;
                for (int dx = 0; dx < pw; ++dx)
                    tc.filled[rowOff + (cx + dx)] = 1;
            }
            tc.pieces[i].placed = true;
            tc.pieces[i].rotw   = pw;
            tc.pieces[i].roth   = ph;
            tc.pieces[i].destX  = cx;
            tc.pieces[i].destY  = cy;

            if (TileBacktrack(tc)) return true;

            // Undo.
            for (int dy = 0; dy < ph; ++dy)
            {
                int rowOff = (cy + dy) * tc.regionW;
                for (int dx = 0; dx < pw; ++dx)
                    tc.filled[rowOff + (cx + dx)] = 0;
            }
            tc.pieces[i].placed = false;
        }
    }
    return false;
}
} // namespace

void Packer::TileSwap(std::vector<Placement>& placements, const std::vector<Item>& items, PackContext& ctx, int gridW,
                      int gridH, int groupingPowerQuarters, int* outCandidatesFound, int* outCandidatesCommitted)
{
    int n = (int)placements.size();
    if (n <= 1 || n > 256) return;
    if ((int)ctx.placementIdGrid.size() < gridW * gridH) return;

    int candFound     = 0;
    int candCommitted = 0;

    AdjGraph g;
    BuildAdjGraph(g, placements);
    long long curScore = ComputeGroupingBonusAdj(placements, items, g, n, ctx, groupingPowerQuarters);

    // For each placement X, try to find a rectangular region R elsewhere on
    // the grid with X's footprint (or rotated) such that R is covered
    // exactly by placements other than X.
    for (int xi = 0; xi < (int)placements.size(); ++xi)
    {
        const Placement& xp = placements[xi];
        bool tryRotations   = items[xp.id].canRotate && xp.w != xp.h;

        for (int ori = 0; ori < (tryRotations ? 2 : 1); ++ori)
        {
            int tw = (ori == 0) ? xp.w : xp.h;
            int th = (ori == 0) ? xp.h : xp.w;

            for (int ty = 0; ty + th <= gridH; ++ty)
            {
                for (int tx = 0; tx + tw <= gridW; ++tx)
                {
                    if (tx == xp.x && ty == xp.y && ori == 0) continue; // identity

                    // Collect placement ids covering R.
                    int members[64];
                    int numMembers = 0;
                    bool allFilled = true;
                    bool containsX = false;
                    bool bleeds    = false;

                    for (int dy = 0; dy < th && !bleeds; ++dy)
                    {
                        int rowOff = (ty + dy) * gridW;
                        for (int dx = 0; dx < tw; ++dx)
                        {
                            int pidx = ctx.placementIdGrid[rowOff + (tx + dx)];
                            if (pidx < 0)
                            {
                                allFilled = false;
                                bleeds    = true;
                                break;
                            }
                            if (pidx == xi)
                            {
                                containsX = true;
                                bleeds    = true;
                                break;
                            }
                            bool seen = false;
                            for (int k = 0; k < numMembers; ++k)
                                if (members[k] == pidx)
                                {
                                    seen = true;
                                    break;
                                }
                            if (!seen)
                            {
                                if (numMembers >= 64)
                                {
                                    bleeds = true;
                                    break;
                                }
                                members[numMembers++] = pidx;
                            }
                        }
                    }
                    if (!allFilled || containsX || bleeds) continue;
                    if (numMembers < 2) continue;

                    // Verify each member lies fully inside R.
                    bool memberBleeds = false;
                    for (int k = 0; k < numMembers && !memberBleeds; ++k)
                    {
                        const Placement& mp = placements[members[k]];
                        if (mp.x < tx || mp.y < ty || mp.x + mp.w > tx + tw || mp.y + mp.h > ty + th)
                            memberBleeds = true;
                    }
                    if (memberBleeds) continue;

                    // Verify the member footprints fit into X's old spot.
                    // Same-orientation case is trivial (R and X share dims).
                    // Rotated case needs the backtracking tiler on an
                    // X.w * X.h region.
                    int destW = xp.w;
                    int destH = xp.h;

                    TileItem pieces[64];
                    for (int k = 0; k < numMembers; ++k)
                    {
                        const Placement& mp    = placements[members[k]];
                        pieces[k].placementIdx = members[k];
                        pieces[k].w            = mp.w;
                        pieces[k].h            = mp.h;
                        pieces[k].canRotate    = items[mp.id].canRotate;
                        pieces[k].rotw         = mp.w;
                        pieces[k].roth         = mp.h;
                        pieces[k].destX        = mp.x - tx;
                        pieces[k].destY        = mp.y - ty;
                        pieces[k].placed       = false;
                    }

                    bool tilingOk = false;
                    if (ori == 0)
                    {
                        // Translate G verbatim — original tiling is still valid.
                        for (int k = 0; k < numMembers; ++k)
                        {
                            pieces[k].rotw = pieces[k].w;
                            pieces[k].roth = pieces[k].h;
                            // destX/destY already set to offsets inside R.
                        }
                        tilingOk = true;
                    }
                    else
                    {
                        // Rotated — run the backtracker.
                        unsigned char filled[32 * 32];
                        if (destW * destH > 32 * 32) continue;
                        std::memset(filled, 0, (size_t)destW * (size_t)destH);
                        TileCtx tc;
                        tc.regionW   = destW;
                        tc.regionH   = destH;
                        tc.filled    = filled;
                        tc.pieces    = pieces;
                        tc.numPieces = numMembers;
                        tilingOk     = TileBacktrack(tc);
                    }
                    if (!tilingOk) continue;

                    ++candFound;

                    // Build hypothetical placements by mutating then scoring then reverting.
                    // Save originals for X and each member.
                    Placement savedX = xp;
                    Placement savedMembers[64];
                    for (int k = 0; k < numMembers; ++k)
                        savedMembers[k] = placements[members[k]];

                    // Apply swap: X moves to (tx, ty) with its original dims; members
                    // move to X's old position with their (rotw, roth, destX, destY).
                    placements[xi].x       = tx;
                    placements[xi].y       = ty;
                    placements[xi].w       = tw;
                    placements[xi].h       = th;
                    placements[xi].rotated = (tw != items[xp.id].w);

                    for (int k = 0; k < numMembers; ++k)
                    {
                        Placement& mp = placements[members[k]];
                        mp.x          = savedX.x + pieces[k].destX;
                        mp.y          = savedX.y + pieces[k].destY;
                        mp.w          = pieces[k].rotw;
                        mp.h          = pieces[k].roth;
                        mp.rotated    = (pieces[k].rotw != items[mp.id].w);
                    }

                    BuildAdjGraph(g, placements);
                    long long newScore = ComputeGroupingBonusAdj(placements, items, g, n, ctx, groupingPowerQuarters);

                    if (newScore > curScore)
                    {
                        // Commit: update placementIdGrid for R and X's old footprint.
                        // Clear old cells of X and all members; stamp new ones.
                        for (int dy = 0; dy < th; ++dy)
                        {
                            int rowOff = (ty + dy) * gridW;
                            for (int dx = 0; dx < tw; ++dx)
                                ctx.placementIdGrid[rowOff + (tx + dx)] = xi;
                        }
                        int memberList[64];
                        for (int k = 0; k < numMembers; ++k)
                            memberList[k] = members[k];
                        StampPlacementCells(ctx.placementIdGrid, placements, gridW, memberList, numMembers);

                        curScore = newScore;
                        ++candCommitted;
                    }
                    else
                    {
                        // Revert.
                        placements[xi] = savedX;
                        for (int k = 0; k < numMembers; ++k)
                            placements[members[k]] = savedMembers[k];
                    }
                }
            }
        }
    }

    if (outCandidatesFound) *outCandidatesFound = candFound;
    if (outCandidatesCommitted) *outCandidatesCommitted = candCommitted;
}
