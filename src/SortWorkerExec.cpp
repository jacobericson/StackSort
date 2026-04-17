#include <sstream>
#include <cstring>

#include "SortWorker.h"
#include "ResultCache.h"
#include "RefineCriteria.h"
#include "Log.h"

static void AdvanceComputedUpTo(volatile LONG* counter, LONG t)
{
    LONG old;
    do
    {
        old = *counter;
        if (t <= old) break;
    } while (InterlockedCompareExchange(counter, t, old) != old);
}

// Build ASCII grid from packing result (debug only).
#ifdef STACKSORT_VERBOSE
static std::string BuildGridString(int gridW, int gridH, int reserveY, const Packer::Result& result,
                                   const std::vector<Packer::Item>& items)
{
    static const char TYPE_CHARS[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const int NUM_TYPE_CHARS = 36;

    std::vector<int> grid(gridW * gridH, 0);
    for (size_t i = 0; i < result.placements.size(); ++i)
    {
        const Packer::Placement& p = result.placements[i];
        int typeId                 = items[p.id].itemTypeId;
        for (int dy = 0; dy < p.h; ++dy)
            for (int dx = 0; dx < p.w; ++dx)
                grid[(p.y + dy) * gridW + (p.x + dx)] = typeId + 1;
    }

    std::vector<unsigned char> ler(gridW * gridH, 0);
    for (int dy = 0; dy < result.lerHeight; ++dy)
        for (int dx = 0; dx < result.lerWidth; ++dx)
            ler[(result.lerY + dy) * gridW + (result.lerX + dx)] = 1;

    std::string s;
    for (int y = 0; y < gridH; ++y)
    {
        if (y == reserveY)
        {
            for (int x = 0; x < gridW; ++x)
                s += '-';
            s += " <- reserve\n";
        }
        for (int x = 0; x < gridW; ++x)
        {
            int idx = y * gridW + x;
            if (grid[idx] > 0)
            {
                int typeId = grid[idx] - 1;
                s += (typeId < NUM_TYPE_CHARS) ? TYPE_CHARS[typeId] : '#';
            }
            else if (ler[idx]) s += '~';
            else s += '.';
        }
        s += '\n';
    }
    return s;
}
#endif

// RAII trailing log for ExecuteJob. Runs on every return path including
// exceptions (though the job body should not throw).
struct JobCleanup
{
    LARGE_INTEGER freq;
    LARGE_INTEGER totalStart;
    int workerIdx;
    volatile long* abortFlag;

    JobCleanup(int idx, volatile long* flag) : workerIdx(idx), abortFlag(flag)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&totalStart);
    }

    ~JobCleanup()
    {
        LARGE_INTEGER totalEnd;
        QueryPerformanceCounter(&totalEnd);
        double totalMs = (double)(totalEnd.QuadPart - totalStart.QuadPart) / (double)freq.QuadPart * 1000.0;
        std::stringstream summary;
        summary << "[StackSort] W" << workerIdx << " finished";
        summary.precision(0);
        summary << std::fixed << ", " << totalMs << "ms total";
        if (*abortFlag != 0) summary << " (ABORTED)";
        LogInfo(summary.str());
    }
};

void SortWorker::ExecuteJob(Job* job, int workerIdx)
{
    JobCleanup cleanup(workerIdx, &job->abortFlag);

    // Shared across every Pack call in this job so scratch allocations amortize.
    Packer::PackContext sharedCtx;

    SectionRef& ref = job->section;

    CachedSection* cs = ResultCache::Find(ref.key);
    if (!cs || cs->generation != ref.generation) return;

    // Snapshot packItems + dims under resultLock. Insert serializes on
    // the same lock, so the locals stay stable for the rest of the job.
    std::vector<Packer::Item> packItemsLocal;
    int gridWLocal             = 0;
    int gridHLocal             = 0;
    Packer::TargetDim dimLocal = Packer::TARGET_H;
    {
        ScopedLock rl(cs->resultLock);
        if (cs->generation != ref.generation) return;
        packItemsLocal = cs->packItems;
        gridWLocal     = cs->gridW;
        gridHLocal     = cs->gridH;
        dimLocal       = cs->dim;
    }

    if (job->target == 0)
    {
        // First-pass chunk: compute targets [targetStart, targetEnd]
        int gridW             = gridWLocal;
        int gridH             = gridHLocal;
        Packer::TargetDim dim = dimLocal;
        int maxSlots          = (dim == Packer::TARGET_H) ? (gridH - 1) : (gridW - 1);
        const char* dimTag    = (dim == Packer::TARGET_H) ? "H" : "W";

        int targetStart = (job->targetEnd > 0) ? job->targetStart : 1;
        int targetEnd   = (job->targetEnd > 0) ? job->targetEnd : maxSlots;
        if (targetStart < 1) targetStart = 1;
        if (targetEnd > maxSlots) targetEnd = maxSlots;

        InterlockedExchange(&cs->valid, 1);

        std::vector<Packer::Item> lastBestOrder;
        int globalBestLERArea = 0;
        int maxLerSideCovered = 0;

        // Track target=1 skip destinations -- excluded from refinement since
        // the unconstrained result at target=1 is the widest possible LER.
        // Only meaningful for the chunk containing target=1.
        bool skipFromLoosest[64];
        memset(skipFromLoosest, 0, sizeof(skipFromLoosest));

        for (int t = targetStart; t <= targetEnd; ++t)
        {
            if (job->abortFlag != 0) break;

            if (cs->generation != ref.generation) break;

            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);

            int skipThreshold = (t <= maxLerSideCovered) ? globalBestLERArea : 0;

            std::vector<Packer::Item> newBestOrder;
            Packer::Result result = Packer::PackAnnealed(gridW, gridH, packItemsLocal, dim, t, &job->abortFlag,
                                                         lastBestOrder.empty() ? NULL : &lastBestOrder, &newBestOrder,
                                                         skipThreshold, NULL, NULL, &sharedCtx);

            QueryPerformanceCounter(&t1);

            if (job->abortFlag != 0) break;
            if (cs->generation != ref.generation) break;

            if (!newBestOrder.empty()) lastBestOrder.swap(newBestOrder);

            int lerSide = (dim == Packer::TARGET_H) ? result.lerHeight : result.lerWidth;

            if (result.allPlaced)
            {
                if (result.lerArea > globalBestLERArea) globalBestLERArea = result.lerArea;
                if (lerSide > maxLerSideCovered) maxLerSideCovered = lerSide;
            }

            double ms = (double)(t1.QuadPart - t0.QuadPart) / (double)cleanup.freq.QuadPart * 1000.0;

            std::stringstream msg;
            msg << "[StackSort] W" << workerIdx << " [" << dimTag << "] target=" << t << ": LER " << result.lerWidth
                << "x" << result.lerHeight << "=" << result.lerArea << " at (" << result.lerX << "," << result.lerY
                << ")";
            msg.precision(1);
            msg << std::fixed << ", " << ms << "ms"
                << ", conc ";
            msg.precision(2);
            msg << result.concentration << ", str " << result.strandedCells << ", grp " << result.groupingBonus
                << (result.allPlaced ? "" : " PARTIAL");
            LogDebug(msg.str());
#ifdef STACKSORT_PACKER_GRID_LOG
            {
                int reserveLine = (dim == Packer::TARGET_H) ? (gridH - t) : gridH;
                LogDebug("\n" + BuildGridString(gridW, gridH, reserveLine, result, packItemsLocal));
            }
#endif

            {
                ScopedLock rl(cs->resultLock);
                cs->results[t - 1] = result;
                if (t - 1 < (int)cs->bestOrders.size())
                {
                    if (!newBestOrder.empty()) cs->bestOrders[t - 1] = newBestOrder;
                    else cs->bestOrders[t - 1] = lastBestOrder;
                }
            }
            AdvanceComputedUpTo(&cs->computedUpTo, (LONG)t);

            // Forward skip: copy result to intervening slots in this chunk.
            // Clamped to targetEnd so chunks don't trample each other.
            if (result.allPlaced && lerSide > t && lerSide <= maxSlots)
            {
                int skipTo = lerSide;
                if (skipTo > targetEnd) skipTo = targetEnd;
                for (int skip = t + 1; skip <= skipTo; ++skip)
                {
                    {
                        ScopedLock rl(cs->resultLock);
                        cs->results[skip - 1] = result;
                        if (skip - 1 < (int)cs->bestOrders.size()) cs->bestOrders[skip - 1] = cs->bestOrders[t - 1];
                    }
                    AdvanceComputedUpTo(&cs->computedUpTo, (LONG)skip);

                    if (t == 1 && skip - 1 < 64) skipFromLoosest[skip - 1] = true;
                }
                if (skipTo > t)
                {
                    LogDebug("[StackSort] W" + IntToStr(workerIdx) + " [" + dimTag + "] skipping target=" +
                             IntToStr(t + 1) + ".." + IntToStr(skipTo) + " (covered by target=" + IntToStr(t) + ")");
                    t = skipTo;
                }
            }
        }

        // Refinement requeue: enqueue suboptimal targets at low priority
        if (job->abortFlag == 0 && cs->generation == ref.generation)
        {
            struct RefineCandidate
            {
                int t;
                long long score;
            };
            RefineCandidate candidates[64];
            int numCandidates = 0;

            int perpendicular = (dim == Packer::TARGET_H) ? gridW : gridH;

            int totalFreeCells = gridW * gridH;
            for (size_t pi = 0; pi < packItemsLocal.size(); ++pi)
                totalFreeCells -= packItemsLocal[pi].w * packItemsLocal[pi].h;

            // candidates[] and skipFromLoosest[] are both sized 64 --
            // safe for any plausible Kenshi grid (max ~20 per side).
            {
                ScopedLock rl(cs->resultLock);
                for (int rt = targetStart; rt <= targetEnd; ++rt)
                {
                    if (rt - 1 < 64 && skipFromLoosest[rt - 1]) continue;
                    if (NeedsRefinement(cs->results[rt - 1], dim, perpendicular, rt, totalFreeCells,
                                        (int)packItemsLocal.size()))
                    {
                        if (numCandidates >= 64)
                        {
                            LogError("[StackSort] refinement candidate overflow"
                                     " (>64); truncating. Bump candidates[] if"
                                     " Kenshi ever ships grids > 64 per side.");
                            break;
                        }
                        candidates[numCandidates].t     = rt;
                        candidates[numCandidates].score = cs->results[rt - 1].score;
                        ++numCandidates;
                    }
                }
            }

            // Sort worst-first (ascending score)
            for (int si = 1; si < numCandidates; ++si)
            {
                RefineCandidate tmp = candidates[si];
                int sj              = si - 1;
                while (sj >= 0 && candidates[sj].score > tmp.score)
                {
                    candidates[sj + 1] = candidates[sj];
                    --sj;
                }
                candidates[sj + 1] = tmp;
            }

            if (numCandidates > 0)
            {
                Job* refJobs[64];
                for (int ci = 0; ci < numCandidates; ++ci)
                {
                    Job* rjob                = new Job();
                    rjob->sourceGUI          = job->sourceGUI;
                    rjob->section.key        = ref.key;
                    rjob->section.generation = ref.generation;
                    rjob->abortFlag          = 0;
                    rjob->dim                = dim;
                    rjob->target             = candidates[ci].t;
                    rjob->targetStart        = 0;
                    rjob->targetEnd          = 0;
                    rjob->priority           = 1;
                    refJobs[ci]              = rjob;
                }
                EnqueueRefinementBatch(refJobs, numCandidates);

                LogInfo("[StackSort] W" + IntToStr(workerIdx) + " [" + dimTag + "] requeued " +
                        IntToStr(numCandidates) + " targets for refinement");
            }
        }
    }
    else if (job->target > 0)
    {
        // Refinement: re-run LAHC for a single H with amplified parameters
        int gridW             = gridWLocal;
        int gridH             = gridHLocal;
        Packer::TargetDim dim = dimLocal;
        int target            = job->target;
        int maxSlots          = (dim == Packer::TARGET_H) ? (gridH - 1) : (gridW - 1);
        const char* dimTag    = (dim == Packer::TARGET_H) ? "H" : "W";

        if (target < 1 || target > maxSlots) return;

        // Refinement inherits first-pass params; only the amplified budget differs.
        Packer::SearchParams firstPassParams = Packer::SearchParams::defaults();

        Packer::SearchParams refineParams = firstPassParams;
        refineParams.numRestarts          = REFINE_RESTARTS;
        refineParams.itersPerRestart      = REFINE_ITERS;
        refineParams.lahcHistoryLen       = REFINE_LAHC_HIST;
        refineParams.plateauThreshold     = REFINE_PLATEAU;

        // Copy seed and oldScore under resultLock -- the vectors can be
        // resized by Insert() on the main thread at any time.
        long long oldScore;
        std::vector<Packer::Item> seedCopy;
        bool hasSeed = false;
        {
            ScopedLock rl(cs->resultLock);
            oldScore = cs->results[target - 1].score;
            if (target - 1 < (int)cs->bestOrders.size() && !cs->bestOrders[target - 1].empty())
            {
                seedCopy = cs->bestOrders[target - 1];
                hasSeed  = true;
            }
        }

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);

        std::vector<Packer::Item> newBestOrder;
        Packer::Result refined =
            Packer::PackAnnealed(gridW, gridH, packItemsLocal, dim, target, &job->abortFlag, hasSeed ? &seedCopy : NULL,
                                 &newBestOrder, 0, &refineParams, NULL, &sharedCtx);

        QueryPerformanceCounter(&t1);

        if (job->abortFlag != 0 || cs->generation != ref.generation) return;

        double ms = (double)(t1.QuadPart - t0.QuadPart) / (double)cleanup.freq.QuadPart * 1000.0;

        if (refined.allPlaced && refined.score > oldScore)
        {
            {
                ScopedLock rl(cs->resultLock);
                cs->results[target - 1] = refined;
                if (target - 1 < (int)cs->bestOrders.size() && !newBestOrder.empty())
                    cs->bestOrders[target - 1] = newBestOrder;
            }

            std::stringstream rmsg;
            rmsg << "[StackSort] W" << workerIdx << " [" << dimTag << "] REFINED target=" << target << ": score "
                 << oldScore << " -> " << refined.score;
            rmsg.precision(1);
            rmsg << std::fixed << " (" << ms << "ms)";
            LogDebug(rmsg.str());
#ifdef STACKSORT_PACKER_GRID_LOG
            {
                int reserveLine = (dim == Packer::TARGET_H) ? (gridH - target) : gridH;
                LogDebug("\n" + BuildGridString(gridW, gridH, reserveLine, refined, packItemsLocal));
            }
#endif
        }
        else
        {
            std::stringstream rmsg;
            rmsg << "[StackSort] W" << workerIdx << " [" << dimTag << "] refine target=" << target
                 << ": no improvement";
            rmsg.precision(1);
            rmsg << std::fixed << " (" << ms << "ms)";
            LogDebug(rmsg.str());
        }
    }
}
