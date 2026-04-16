#pragma warning(push)
#pragma warning(disable : 4091)
#include <kenshi/Inventory.h>
#include <kenshi/gui/InventoryGUI.h>
#pragma warning(pop)

#include <map>

#include "SortWorker.h"
#include "ResultCache.h"
#include "StackSort.h"
#include "StackSortConfig.h"
#include "Log.h"

static const DWORD MUTATION_DEBOUNCE_MS = 150;

// Active target dim (H or W). Runtime-toggleable via ToggleDim().
// Written only under s_queueLock to serialize with EnqueueJob.
static volatile LONG s_currentDim = (LONG)Packer::TARGET_H;

// Per-GUI tracking (main-thread-only).

struct GUITracking
{
    InventoryGUI* gui;        // NULL = free slot
    Inventory* trackedInv[2]; // body + backpack
    int numTrackedInv;
    InventorySection* sectionKeys[MAX_SECTIONS]; // keys into ResultCache
    int numSections;
    bool mutationPending;
    DWORD lastMutationTick;
};

static GUITracking s_guiTracking[MAX_GUIS];

typedef SortWorker::SectionRef SectionRef;
typedef SortWorker::Job Job;

static std::vector<Job*> s_jobQueue;
static CRITICAL_SECTION s_queueLock;

static const int NUM_WORKERS = 4;
static HANDLE s_threads[NUM_WORKERS];
static HANDLE s_workSemaphore   = NULL;
static volatile LONG s_shutdown = 0;
static volatile Job* s_activeJobs[NUM_WORKERS];
// Main-thread only: ApplyGuard writes, inventory mutation hooks read.
static bool s_applying = false;

int SortWorker::FindTracking(InventoryGUI* gui)
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (s_guiTracking[i].gui == gui) return i;
    }
    return -1;
}

int SortWorker::AllocTracking(InventoryGUI* gui)
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (s_guiTracking[i].gui == NULL)
        {
            GUITracking& t     = s_guiTracking[i];
            t.gui              = gui;
            t.numTrackedInv    = 0;
            t.numSections      = 0;
            t.mutationPending  = false;
            t.lastMutationTick = 0;
            return i;
        }
    }
    ErrorLog("[StackSort] All GUI tracking slots full (max " + IntToStr(MAX_GUIS) + ")");
    return -1;
}

void SortWorker::FreeTracking(int idx)
{
    GUITracking& t     = s_guiTracking[idx];
    t.gui              = NULL;
    t.numTrackedInv    = 0;
    t.numSections      = 0;
    t.mutationPending  = false;
    t.lastMutationTick = 0;
}

void SortWorker::ScanInventory(int trackIdx, Inventory* inv, bool* outNeedsCompute)
{
    if (!inv) return;

    GUITracking& t = s_guiTracking[trackIdx];

    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        if (t.numSections >= MAX_SECTIONS) break;

        InventorySection* section = *it;

        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;

        const Ogre::vector<InventorySection::SectionItem>::type& sectionItems = section->getItems();
        if (sectionItems.empty()) continue;

        int gridW             = section->width;
        int gridH             = section->height;
        int itemCount         = (int)sectionItems.size();
        Packer::TargetDim dim = GetDim();

        // Check if cached results are still valid for this section and dim
        if (ResultCache::IsReusable(section, itemCount, gridW, gridH, dim))
        {
            DebugLog("[StackSort] Cache hit: section=" + IntToStr((int)(intptr_t)section) + " '" + section->name + "'" +
                     " (" + IntToStr(itemCount) + " items)");
            t.sectionKeys[t.numSections] = section;
            ++t.numSections;
            continue;
        }

        // Cache miss or stale — build fresh snapshot
        std::vector<Packer::Item> packItems;
        std::vector<Item*> itemPtrs;

        std::map<GameData*, int> typeIdMap;
        int nextTypeId = 0;

        for (size_t i = 0; i < sectionItems.size(); ++i)
        {
            const InventorySection::SectionItem& si = sectionItems[i];
            if (!si.item) continue;

            Packer::Item pi;
            pi.id        = (int)packItems.size();
            pi.w         = si.item->itemWidth;
            pi.h         = si.item->itemHeight;
            pi.canRotate = StackSort_CanRotate(si.item);

            GameData* gd                              = si.item->data;
            std::map<GameData*, int>::iterator typeIt = typeIdMap.find(gd);
            if (typeIt == typeIdMap.end())
            {
                typeIdMap[gd] = nextTypeId;
                pi.itemTypeId = nextTypeId++;
            }
            else
            {
                pi.itemTypeId = typeIt->second;
            }

            packItems.push_back(pi);
            itemPtrs.push_back(si.item);
        }

        if (packItems.empty()) continue;

        ResultCache::Insert(section, gridW, gridH, dim, packItems, itemPtrs);

        DebugLog("[StackSort] Cache miss: section=" + IntToStr((int)(intptr_t)section) + " '" + section->name + "'" +
                 " " + IntToStr(gridW) + "x" + IntToStr(gridH) + " " + IntToStr((int)packItems.size()) + " items");

        t.sectionKeys[t.numSections] = section;
        ++t.numSections;
        *outNeedsCompute = true;
    }
}

void SortWorker::SnapshotSections(int trackIdx, InventoryGUI* gui, bool* outNeedsCompute)
{
    GUITracking& t  = s_guiTracking[trackIdx];
    t.numSections   = 0;
    t.numTrackedInv = 0;

    Inventory* bodyInv = gui->_NV_getInventory();
    if (bodyInv)
    {
        ScanInventory(trackIdx, bodyInv, outNeedsCompute);
        t.trackedInv[t.numTrackedInv++] = bodyInv;
    }

    ContainerItem* backpack = gui->getBackpack();
    if (backpack && backpack->inventory)
    {
        ScanInventory(trackIdx, backpack->inventory, outNeedsCompute);
        if (t.numTrackedInv < 2) t.trackedInv[t.numTrackedInv++] = backpack->inventory;
    }
}

void SortWorker::EnqueueJob(int trackIdx)
{
    GUITracking& t = s_guiTracking[trackIdx];
    if (t.numSections == 0) return;

    int numEnqueued       = 0;
    Packer::TargetDim dim = GetDim();

    {
        ScopedLock guard(s_queueLock);
        for (int i = 0; i < t.numSections; ++i)
        {
            CachedSection* cs = ResultCache::Find(t.sectionKeys[i]);
            if (!cs) continue;

            int maxSlots = (dim == Packer::TARGET_H) ? (cs->gridH - 1) : (cs->gridW - 1);
            if (maxSlots < 1) continue;

            // Split [1..maxSlots] into NUM_WORKERS chunks so workers
            // can run first-pass in parallel. Warm-start chain is chunk-local.
            int chunkCount = NUM_WORKERS;
            if (chunkCount > maxSlots) chunkCount = maxSlots;
            int baseSize  = maxSlots / chunkCount;
            int remainder = maxSlots % chunkCount;

            int start = 1;
            for (int c = 0; c < chunkCount; ++c)
            {
                int sz  = baseSize + (c < remainder ? 1 : 0);
                int end = start + sz - 1;

                Job* job                = new Job();
                job->sourceGUI          = t.gui;
                job->section.key        = t.sectionKeys[i];
                job->section.generation = cs->generation;
                job->abortFlag          = 0;
                job->dim                = dim;
                job->target             = 0;
                job->targetStart        = start;
                job->targetEnd          = end;
                job->priority           = 0;

                s_jobQueue.push_back(job);
                ++numEnqueued;

                start = end + 1;
            }
        }
    }

    if (numEnqueued > 0) ReleaseSemaphore(s_workSemaphore, numEnqueued, NULL);

    std::string msg = "[StackSort] Enqueued " + IntToStr(numEnqueued) + " job";
    if (numEnqueued > 1) msg += "s";
    msg += " (";
    for (int i = 0; i < t.numSections; ++i)
    {
        CachedSection* cs = ResultCache::Find(t.sectionKeys[i]);
        if (cs)
        {
            msg += IntToStr(cs->gridW) + "x" + IntToStr(cs->gridH) + "/" + IntToStr(cs->itemCount) + " items";
        }
        if (i < t.numSections - 1) msg += ", ";
    }
    msg += ")";
    InfoLog(msg);
}

void SortWorker::AbortJobsForGUI(InventoryGUI* gui)
{
    ScopedLock guard(s_queueLock);

    // Abort queued jobs
    for (int i = (int)s_jobQueue.size() - 1; i >= 0; --i)
    {
        if (s_jobQueue[i]->sourceGUI == gui)
        {
            InterlockedExchange(&s_jobQueue[i]->abortFlag, 1);
            delete s_jobQueue[i];
            s_jobQueue.erase(s_jobQueue.begin() + i);
        }
    }

    // Abort active jobs — held under s_queueLock so the worker can't
    // delete the job between our load and dereference (see WorkerProc).
    for (int w = 0; w < NUM_WORKERS; ++w)
    {
        Job* active = (Job*)s_activeJobs[w];
        if (active && active->sourceGUI == gui) InterlockedExchange(&active->abortFlag, 1);
    }
}

DWORD WINAPI SortWorker::WorkerProc(LPVOID param)
{
    int workerIdx = (int)(intptr_t)param;

    while (s_shutdown == 0)
    {
        WaitForSingleObject(s_workSemaphore, INFINITE);
        if (s_shutdown != 0) break;

        Job* job = NULL;
        {
            ScopedLock guard(s_queueLock);
            if (!s_jobQueue.empty())
            {
                // Priority-aware selection: lowest priority value wins, FIFO within same priority
                int bestIdx = 0;
                for (size_t i = 1; i < s_jobQueue.size(); ++i)
                {
                    if (s_jobQueue[i]->priority < s_jobQueue[bestIdx]->priority) bestIdx = (int)i;
                }
                job = s_jobQueue[bestIdx];
                s_jobQueue.erase(s_jobQueue.begin() + bestIdx);
                // Publish to s_activeJobs under the same lock so eviction's
                // IsSectionActive check always sees either the queue or the
                // active slot — never a window where the job is untracked.
                s_activeJobs[workerIdx] = job;
            }
        }

        if (!job) continue;

        ExecuteJob(job, workerIdx);
        // Null the pointer under s_queueLock so AbortJobsForGUI can't
        // dereference a freed job between our NULL and delete.
        {
            ScopedLock guard(s_queueLock);
            s_activeJobs[workerIdx] = NULL;
        }
        delete job;
    }

    return 0;
}

void SortWorker::EnqueueRefinementBatch(Job** jobs, int count)
{
    if (count <= 0) return;
    {
        ScopedLock guard(s_queueLock);
        for (int i = 0; i < count; ++i)
            s_jobQueue.push_back(jobs[i]);
    }
    ReleaseSemaphore(s_workSemaphore, count, NULL);
}

void SortWorker::Init()
{
    InitializeCriticalSection(&s_queueLock);
    s_workSemaphore = CreateSemaphore(NULL, 0, 256, NULL);
    s_shutdown      = 0;

    for (int i = 0; i < NUM_WORKERS; ++i)
    {
        s_activeJobs[i] = NULL;
        s_threads[i]    = CreateThread(NULL, 0, WorkerProc, (LPVOID)(intptr_t)i, 0, NULL);
        if (!s_threads[i]) ErrorLog("[StackSort] Failed to create worker thread " + IntToStr(i));
    }
    InfoLog("[StackSort] " + IntToStr(NUM_WORKERS) + " worker threads started");
}

void SortWorker::Shutdown()
{
    InterlockedExchange(&s_shutdown, 1);
    if (s_workSemaphore) ReleaseSemaphore(s_workSemaphore, NUM_WORKERS, NULL);

    for (int i = 0; i < NUM_WORKERS; ++i)
    {
        if (s_threads[i])
        {
            WaitForSingleObject(s_threads[i], 2000);
            CloseHandle(s_threads[i]);
            s_threads[i] = NULL;
        }
    }

    if (s_workSemaphore)
    {
        CloseHandle(s_workSemaphore);
        s_workSemaphore = NULL;
    }

    // Clean up any remaining queued jobs
    {
        ScopedLock guard(s_queueLock);
        for (size_t i = 0; i < s_jobQueue.size(); ++i)
            delete s_jobQueue[i];
        s_jobQueue.clear();
    }

    DeleteCriticalSection(&s_queueLock);
}

void SortWorker::OnInventoryOpened(InventoryGUI* gui)
{
    if (FindTracking(gui) >= 0) return;

    int idx = AllocTracking(gui);
    if (idx < 0) return;

    bool needsCompute = false;
    SnapshotSections(idx, gui, &needsCompute);

    if (s_guiTracking[idx].numSections == 0) return; // no eligible sections (keep tracking to prevent per-frame retry)

    InfoLog("[StackSort] Inventory opened (cache: " + IntToStr(ResultCache::Size()) + " entries)");

    // Evict old unreferenced cache entries if needed
    // Gather all referenced section keys across all open GUIs
    InventorySection* allRefs[MAX_GUIS * MAX_SECTIONS];
    int numRefs = 0;
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (!s_guiTracking[i].gui) continue;
        for (int j = 0; j < s_guiTracking[i].numSections; ++j)
        {
            if (numRefs < MAX_GUIS * MAX_SECTIONS) allRefs[numRefs++] = s_guiTracking[i].sectionKeys[j];
        }
    }
    ResultCache::EvictIfNeeded(allRefs, numRefs, MAX_CACHED_SECTIONS);

    if (needsCompute) EnqueueJob(idx);
}

void SortWorker::OnInventoryClosed(InventoryGUI* gui)
{
    int idx = FindTracking(gui);
    if (idx < 0) return;

    AbortJobsForGUI(gui);

    s_guiTracking[idx].mutationPending = false;
    FreeTracking(idx);

    InfoLog("[StackSort] Inventory closed (cache: " + IntToStr(ResultCache::Size()) + " entries)");
}

void SortWorker::OnMutation(InventoryGUI* gui)
{
    int idx = FindTracking(gui);
    if (idx < 0) return;

    GUITracking& t = s_guiTracking[idx];

    // Invalidate all cached sections for this GUI
    for (int i = 0; i < t.numSections; ++i)
        ResultCache::Invalidate(t.sectionKeys[i]);

    // Abort queued and active jobs for this GUI
    AbortJobsForGUI(gui);

    // Arm debounced re-snapshot + re-enqueue
    t.mutationPending  = true;
    t.lastMutationTick = GetTickCount();
}

void SortWorker::PollWorkerStart()
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        GUITracking& t = s_guiTracking[i];
        if (!t.gui || !t.mutationPending) continue;

        DWORD elapsed = GetTickCount() - t.lastMutationTick;
        if (elapsed < MUTATION_DEBOUNCE_MS) continue;

        // Re-snapshot sections into cache
        bool needsCompute = false;
        t.numSections     = 0; // reset before re-scan

        Inventory* bodyInv = t.gui->_NV_getInventory();
        if (bodyInv) ScanInventory(i, bodyInv, &needsCompute);

        ContainerItem* backpack = t.gui->getBackpack();
        if (backpack && backpack->inventory) ScanInventory(i, backpack->inventory, &needsCompute);

        if (needsCompute) EnqueueJob(i);

        t.mutationPending = false;
        break; // one per frame
    }
}

bool SortWorker::GetResult(InventorySection* section, int target, Packer::Result& outResult)
{
    CachedSection* cs = ResultCache::Find(section);
    if (!cs || cs->valid == 0) return false;

    int maxSlots = (cs->dim == Packer::TARGET_H) ? (cs->gridH - 1) : (cs->gridW - 1);
    if (target < 1 || target > maxSlots) return false;

    LONG computed = cs->computedUpTo;
    if (target > computed) return false;

    {
        ScopedLock rl(cs->resultLock);
        outResult = cs->results[target - 1];
    }
    return true;
}

bool SortWorker::GetBestResult(InventorySection* section, int target, Packer::Result& outResult, int* outSourceTarget)
{
    CachedSection* cs = ResultCache::Find(section);
    if (!cs || cs->valid == 0) return false;

    LONG computed = cs->computedUpTo;
    if (computed == 0) return false;

    bool found          = false;
    int bestTarget      = 0;
    long long bestScore = 0;

    {
        ScopedLock rl(cs->resultLock);
        int scanLimit = (target < computed) ? target : computed;
        for (int t = 1; t <= scanLimit; ++t)
        {
            const Packer::Result& r = cs->results[t - 1];
            if (!r.allPlaced) continue;
            int lerSide = (cs->dim == Packer::TARGET_H) ? r.lerHeight : r.lerWidth;
            if (lerSide < target) continue;
            if (!found || r.score > bestScore)
            {
                outResult  = r;
                bestScore  = r.score;
                bestTarget = t;
                found      = true;
            }
        }
    }

    if (outSourceTarget) *outSourceTarget = bestTarget;
    return found;
}

bool SortWorker::GetItemPtrs(InventorySection* section, std::vector<Item*>& outPtrs)
{
    CachedSection* cs = ResultCache::Find(section);
    if (!cs) return false;
    {
        ScopedLock rl(cs->resultLock);
        outPtrs = cs->itemPtrs;
    }
    return true;
}

bool SortWorker::HasContext(InventoryGUI* gui)
{
    return FindTracking(gui) >= 0;
}

bool SortWorker::IsTrackedInventory(Inventory* inv)
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        const GUITracking& t = s_guiTracking[i];
        if (!t.gui) continue;
        for (int ti = 0; ti < t.numTrackedInv; ++ti)
        {
            if (t.trackedInv[ti] == inv) return true;
        }
    }
    return false;
}

bool SortWorker::ContextMatchesInventory(InventoryGUI* gui)
{
    int idx = FindTracking(gui);
    if (idx < 0) return false;

    Inventory* currentInv = gui->_NV_getInventory();
    if (!currentInv) return false;

    const GUITracking& t = s_guiTracking[idx];
    for (int ti = 0; ti < t.numTrackedInv; ++ti)
    {
        if (t.trackedInv[ti] == currentInv) return true;
    }
    return false;
}

InventoryGUI* SortWorker::FindContextGUIForInventory(Inventory* inv)
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        const GUITracking& t = s_guiTracking[i];
        if (!t.gui) continue;
        for (int ti = 0; ti < t.numTrackedInv; ++ti)
        {
            if (t.trackedInv[ti] == inv) return t.gui;
        }
    }
    return NULL;
}

bool SortWorker::IsApplying()
{
    return s_applying;
}

void SortWorker::SetApplying(bool v)
{
    s_applying = v;
}

bool SortWorker::IsRunning()
{
    for (int i = 0; i < NUM_WORKERS; ++i)
    {
        if (s_activeJobs[i] != NULL) return true;
    }
    return false;
}

bool SortWorker::IsSectionActive(InventorySection* section)
{
    ScopedLock guard(s_queueLock);

    for (size_t i = 0; i < s_jobQueue.size(); ++i)
    {
        if (s_jobQueue[i]->section.key == section) return true;
    }
    for (int w = 0; w < NUM_WORKERS; ++w)
    {
        Job* j = (Job*)s_activeJobs[w];
        if (j && j->section.key == section) return true;
    }
    return false;
}

Packer::TargetDim SortWorker::GetDim()
{
    return (Packer::TargetDim)s_currentDim;
}

void SortWorker::ToggleDim()
{
    Packer::TargetDim newDim;
    {
        ScopedLock guard(s_queueLock);

        newDim = (GetDim() == Packer::TARGET_H) ? Packer::TARGET_W : Packer::TARGET_H;
        InterlockedExchange(&s_currentDim, (LONG)newDim);

        // Abort all queued jobs
        for (int i = (int)s_jobQueue.size() - 1; i >= 0; --i)
        {
            InterlockedExchange(&s_jobQueue[i]->abortFlag, 1);
            delete s_jobQueue[i];
            s_jobQueue.erase(s_jobQueue.begin() + i);
        }

        // Abort all active jobs (worker will notice abortFlag and skip writes)
        for (int w = 0; w < NUM_WORKERS; ++w)
        {
            Job* active = (Job*)s_activeJobs[w];
            if (active) InterlockedExchange(&active->abortFlag, 1);
        }
    }

    // Invalidate + re-snapshot + re-enqueue for all open GUIs under the new dim
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (!s_guiTracking[i].gui) continue;
        for (int j = 0; j < s_guiTracking[i].numSections; ++j)
            ResultCache::Invalidate(s_guiTracking[i].sectionKeys[j]);

        bool needsCompute            = false;
        s_guiTracking[i].numSections = 0;
        SnapshotSections(i, s_guiTracking[i].gui, &needsCompute);
        if (needsCompute) EnqueueJob(i);
    }

    InfoLog(std::string("[StackSort] Target dim toggled to ") + (newDim == Packer::TARGET_H ? "H" : "W"));
}

int SortWorker::GetMaxTarget(InventorySection* section)
{
    CachedSection* cs = ResultCache::Find(section);
    if (!cs) return 0;

    int maxSlots  = (cs->dim == Packer::TARGET_H) ? (cs->gridH - 1) : (cs->gridW - 1);
    LONG computed = cs->computedUpTo;

    if (computed == 0) return maxSlots;

    int maxLerSide = 0;
    {
        ScopedLock rl(cs->resultLock);
        for (int t = 1; t <= computed; ++t)
        {
            const Packer::Result& r = cs->results[t - 1];
            if (!r.allPlaced) continue;
            int lerSide = (cs->dim == Packer::TARGET_H) ? r.lerHeight : r.lerWidth;
            if (lerSide > maxLerSide) maxLerSide = lerSide;
        }
    }

    return maxLerSide > 0 ? maxLerSide : maxSlots;
}
