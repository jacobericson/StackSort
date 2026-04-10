#pragma once

#include <Windows.h>
#include <vector>

#include "Packer.h"

class Inventory;
class InventoryGUI;
class InventorySection;
class Item;
struct CachedSection;

class SortWorker
{
public:
    static void Init();
    static void Shutdown();

    // Inventory opened: allocate GUI tracking, check cache for reusable
    // results, snapshot new sections into cache, enqueue job.
    static void OnInventoryOpened(InventoryGUI* gui);

    // Inventory closed: abort queued/active jobs for this GUI, free tracking.
    // Cache entries are NOT removed (persist for reuse on reopen).
    static void OnInventoryClosed(InventoryGUI* gui);

    // Item added/removed in a tracked inventory: invalidate cache entries,
    // abort jobs, arm debounced re-snapshot + re-enqueue.
    static void OnMutation(InventoryGUI* gui);

    // Called from _NV_update each frame. Fires debounced re-enqueue.
    static void PollWorkerStart();

    // Copy precomputed result for a specific section + target into outResult.
    // Reads the active dim from s_currentDim. Returns true if found.
    static bool GetResult(InventorySection* section, int target,
                          Packer::Result& outResult);

    // Scan computed results for this section and copy the one with the best
    // score whose lerSide >= target into outResult (lerSide is lerHeight in
    // H-mode, lerWidth in W-mode). If outSourceTarget is non-NULL, writes
    // which target value produced the winning result.
    // Returns true if a valid result was found.
    static bool GetBestResult(InventorySection* section, int target,
                              Packer::Result& outResult,
                              int* outSourceTarget = NULL);

    // Copy snapshot's game pointers for a specific section into outPtrs.
    // Returns true if found.
    static bool GetItemPtrs(InventorySection* section, std::vector<Item*>& outPtrs);

    static Packer::TargetDim GetDim();
    // Flip H<->W, abort all jobs, invalidate all cache entries, re-enqueue
    // first-pass for all open GUIs under the new dim.
    static void ToggleDim();

    static bool HasContext(InventoryGUI* gui);
    static bool IsTrackedInventory(Inventory* inv);
    static bool ContextMatchesInventory(InventoryGUI* gui);
    static InventoryGUI* FindContextGUIForInventory(Inventory* inv);
    static bool IsApplying();
    static void SetApplying(bool v);
    static bool IsRunning();
    // Max reachable target value for the active dim at this section.
    static int GetMaxTarget(InventorySection* section);
    // True if any queued or active job targets this section.
    static bool IsSectionActive(InventorySection* section);

    struct SectionRef
    {
        InventorySection* key;
        LONG generation;
    };

    struct Job
    {
        SectionRef section;
        InventoryGUI* sourceGUI;
        volatile LONG abortFlag;
        Packer::TargetDim dim;
        int        target;            // 0 = first-pass chunk, >0 = refine this target
        int        targetStart;       // first-pass chunk start (inclusive), unused for refinement
        int        targetEnd;         // first-pass chunk end (inclusive), 0 = unused/refinement
        int        priority;          // 0 = first-pass, 1 = refinement
    };

private:
    static DWORD WINAPI WorkerProc(LPVOID param);
    static void ExecuteJob(Job* job, int workerIdx);

    static void SnapshotSections(int trackIdx, InventoryGUI* gui,
                                 bool* outNeedsCompute);
    static void ScanInventory(int trackIdx, Inventory* inv,
                              bool* outNeedsCompute);

    static int FindTracking(InventoryGUI* gui);
    static int AllocTracking(InventoryGUI* gui);
    static void FreeTracking(int idx);

    static void EnqueueJob(int trackIdx);
    static void AbortJobsForGUI(InventoryGUI* gui);
    static void EnqueueRefinementBatch(Job** jobs, int count);
};
