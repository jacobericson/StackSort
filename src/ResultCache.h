#pragma once

#include <Windows.h>
#include <vector>

#include "Packer.h"

class InventorySection;
class Item;

struct ScopedLock
{
    CRITICAL_SECTION& cs;
    ScopedLock(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
    ~ScopedLock() { LeaveCriticalSection(&cs); }
private:
    ScopedLock(const ScopedLock&);
    ScopedLock& operator=(const ScopedLock&);
};


struct OriginalPlacement
{
    Item* item;
    int x, y, w, h;
};

// Keyed by InventorySection* (stable per-character).
// Persists across GUI close/reopen.
// Worker thread writes results[]/computedUpTo; main thread reads them.

struct CachedSection
{
    CachedSection()
        : section(NULL), gridW(0), gridH(0), dim(Packer::TARGET_H),
          itemCount(0),
          computedUpTo(0), valid(0), generation(0), snapshotTick(0),
          hasOriginalPositions(false), resultLockInit(false)
    {
        memset(&resultLock, 0, sizeof(resultLock));
    }

    InventorySection* section;      // cache key (game pointer, stable per-character)
    int gridW;
    int gridH;
    Packer::TargetDim dim;          // target dim this cache was built for
    int itemCount;                  // snapshot item count (for reopen validation)
    std::vector<Packer::Item> packItems;
    std::vector<Item*> itemPtrs;
    std::vector<Packer::Result> results;  // indexed by H-1
    std::vector< std::vector<Packer::Item> > bestOrders;  // parallel to results[], warm-start seeds
    volatile LONG computedUpTo;     // highest H published (worker writes, main reads)
    volatile LONG valid;            // 0 = stale (mutation invalidated)
    volatile LONG generation;       // incremented on re-snapshot; worker skips mismatches
    DWORD snapshotTick;             // GetTickCount() when snapshot taken (LRU eviction)
    std::vector<OriginalPlacement> originalPositions;  // pre-sort item positions
    bool hasOriginalPositions;      // true once original positions captured (first click)
    CRITICAL_SECTION resultLock;    // protects results[] and bestOrders[] access
    bool resultLockInit;            // true once resultLock is initialized

    // Do not copy after resultLockInit — CRITICAL_SECTION has self-referencing pointers.
};

// s_lock protects map structure operations.
// resultLock (per-entry) protects results[]/bestOrders[] reads and writes.
// computedUpTo uses InterlockedExchange as a publish barrier.

class ResultCache
{
public:
    static void Init();
    static void Shutdown();

    // Insert or update a cache entry with a fresh snapshot.
    // Pre-allocates results[] sized for the given dim:
    //   TARGET_H -> gridH-1 slots (targets 1..gridH-1)
    //   TARGET_W -> gridW-1 slots (targets 1..gridW-1)
    // Returns the new generation number.
    static LONG Insert(InventorySection* section, int gridW, int gridH,
                       Packer::TargetDim dim,
                       const std::vector<Packer::Item>& packItems,
                       const std::vector<Item*>& itemPtrs);

    // Lookup by section pointer. Returns NULL if not found.
    // The returned pointer is stable (std::map iterators are not
    // invalidated by insert/erase of other keys). Caller must ensure
    // the entry is not erased while using the pointer (abort protocol).
    static CachedSection* Find(InventorySection* section);

    // Set valid=0 on the entry. Does not remove it.
    static void Invalidate(InventorySection* section);

    // Remove entry from the map. Caller must ensure no worker is
    // actively writing to this entry (use abort + generation check).
    static void Remove(InventorySection* section);

    // Check if a cached entry can be reused on inventory reopen.
    // Compares item count, grid dims, and cached dim against the live
    // section state. Cache entries sized for a different dim fail.
    // Returns false if not found or if snapshot doesn't match.
    static bool IsReusable(InventorySection* section,
                           int itemCount, int gridW, int gridH,
                           Packer::TargetDim dim);

    // Evict oldest unreferenced entries when cache exceeds maxEntries.
    // referencedKeys: section pointers currently in use by open GUIs.
    // referencedCount: number of entries in referencedKeys.
    static void EvictIfNeeded(InventorySection* const* referencedKeys,
                              int referencedCount, int maxEntries);

    // Return current number of cached entries (for logging).
    static int Size();
};
