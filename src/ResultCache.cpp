#include <map>

#include "ResultCache.h"
#include "SortWorker.h"

static std::map<InventorySection*, CachedSection> s_cache;
static CRITICAL_SECTION s_lock;

void ResultCache::Init()
{
    InitializeCriticalSection(&s_lock);
}

void ResultCache::Shutdown()
{
    ScopedLock guard(s_lock);
    for (std::map<InventorySection*, CachedSection>::iterator it = s_cache.begin();
         it != s_cache.end(); ++it)
    {
        if (it->second.resultLockInit)
            DeleteCriticalSection(&it->second.resultLock);
    }
    s_cache.clear();
}

LONG ResultCache::Insert(InventorySection* section, int gridW, int gridH,
                         Packer::TargetDim dim,
                         const std::vector<Packer::Item>& packItems,
                         const std::vector<Item*>& itemPtrs)
{
    ScopedLock guard(s_lock);

    CachedSection& cs = s_cache[section];
    if (!cs.resultLockInit)
    {
        InitializeCriticalSection(&cs.resultLock);
        cs.resultLockInit = true;
    }

    // resultLock also covers packItems/dim writes -- workers snapshot
    // these under the same lock, so serializing prevents a torn read.
    int maxSlots = (dim == Packer::TARGET_H) ? (gridH - 1) : (gridW - 1);
    {
        ScopedLock rl(cs.resultLock);
        cs.section = section;
        cs.gridW = gridW;
        cs.gridH = gridH;
        cs.dim = dim;
        cs.itemCount = (int)itemPtrs.size();
        cs.packItems = packItems;
        cs.itemPtrs = itemPtrs;
        cs.snapshotTick = GetTickCount();
        cs.results.clear();
        cs.bestOrders.clear();
        if (maxSlots > 0)
        {
            cs.results.resize(maxSlots);
            cs.bestOrders.resize(maxSlots);
        }
    }

    // Preserve original positions across re-inserts (only captured on first click).
    // New entries start without originals; existing entries keep theirs.
    if (cs.generation == 0)
        cs.hasOriginalPositions = false;

    // Generation: increment if entry already existed, else start at 1
    LONG newGen = cs.generation + 1;
    if (newGen <= 0) newGen = 1;
    InterlockedExchange(&cs.computedUpTo, 0);
    InterlockedExchange(&cs.valid, 0);  // not valid until worker publishes
    InterlockedExchange(&cs.generation, newGen);

    return newGen;
}

CachedSection* ResultCache::Find(InventorySection* section)
{
    ScopedLock guard(s_lock);
    std::map<InventorySection*, CachedSection>::iterator it = s_cache.find(section);
    return (it != s_cache.end()) ? &it->second : NULL;
}

// Mark stale without removing.
void ResultCache::Invalidate(InventorySection* section)
{
    ScopedLock guard(s_lock);
    std::map<InventorySection*, CachedSection>::iterator it = s_cache.find(section);
    if (it != s_cache.end())
        InterlockedExchange(&it->second.valid, 0);
}

void ResultCache::Remove(InventorySection* section)
{
    ScopedLock guard(s_lock);
    std::map<InventorySection*, CachedSection>::iterator it = s_cache.find(section);
    if (it != s_cache.end())
    {
        if (it->second.resultLockInit)
            DeleteCriticalSection(&it->second.resultLock);
        s_cache.erase(it);
    }
}

bool ResultCache::IsReusable(InventorySection* section,
                             int itemCount, int gridW, int gridH,
                             Packer::TargetDim dim)
{
    ScopedLock guard(s_lock);
    std::map<InventorySection*, CachedSection>::iterator it = s_cache.find(section);
    if (it == s_cache.end())
        return false;

    const CachedSection& cs = it->second;
    return cs.valid != 0
        && cs.dim == dim
        && cs.itemCount == itemCount
        && cs.gridW == gridW
        && cs.gridH == gridH
        && cs.computedUpTo > 0;
}

void ResultCache::EvictIfNeeded(InventorySection* const* referencedKeys,
                                int referencedCount, int maxEntries)
{
    ScopedLock guard(s_lock);

    while ((int)s_cache.size() > maxEntries)
    {
        // Find the oldest unreferenced entry
        std::map<InventorySection*, CachedSection>::iterator oldest = s_cache.end();
        DWORD oldestTick = 0xFFFFFFFF;

        for (std::map<InventorySection*, CachedSection>::iterator it = s_cache.begin();
             it != s_cache.end(); ++it)
        {
            // Skip entries referenced by open GUIs
            bool referenced = false;
            for (int i = 0; i < referencedCount; ++i)
            {
                if (referencedKeys[i] == it->first)
                {
                    referenced = true;
                    break;
                }
            }
            if (referenced)
                continue;

            // Skip sections a worker might still be touching -- erasing
            // would delete the resultLock out from under an active job.
            if (SortWorker::IsSectionActive(it->first))
                continue;

            if (it->second.snapshotTick < oldestTick)
            {
                oldestTick = it->second.snapshotTick;
                oldest = it;
            }
        }

        if (oldest == s_cache.end())
            break;  // all entries are referenced, can't evict

        // TryEnter stays manual — conditional acquire not compatible with ScopedLock
        if (oldest->second.resultLockInit)
        {
            if (!TryEnterCriticalSection(&oldest->second.resultLock))
                break;  // worker still holds lock, retry next time
            LeaveCriticalSection(&oldest->second.resultLock);
            DeleteCriticalSection(&oldest->second.resultLock);
        }
        s_cache.erase(oldest);
    }
}

int ResultCache::Size()
{
    ScopedLock guard(s_lock);
    return (int)s_cache.size();
}
