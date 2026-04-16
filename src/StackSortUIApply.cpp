#pragma warning(push)
#pragma warning(disable : 4091)
#include <kenshi/Inventory.h>
#include <kenshi/gui/InventoryGUI.h>
#pragma warning(pop)

#include <vector>
#include <sstream>

#include "StackSortUI.h"
#include "Packer.h"
#include "ResultCache.h"
#include "SortWorker.h"
#include "StackSort.h"
#include "Log.h"

class InventoryGUIAccess : public InventoryGUI
{
  public:
    using InventoryGUI::refreshAllSections;
};

struct CallbackGuard
{
    InventorySection* section;
    RootObject* saved;

    CallbackGuard(InventorySection* s) : section(s), saved(s->callbackObject)
    {
        s->callbackObject = NULL;
    }

    ~CallbackGuard()
    {
        section->callbackObject = saved;
    }
};

struct ApplyGuard
{
    ApplyGuard()
    {
        SortWorker::SetApplying(true);
    }
    ~ApplyGuard()
    {
        SortWorker::SetApplying(false);
    }
};

static std::vector<unsigned char> snapshotOccupancyGrid(InventorySection* section)
{
    int gridW = section->width;
    int gridH = section->height;
    std::vector<unsigned char> grid(gridW * gridH, 0);

    const Ogre::vector<InventorySection::SectionItem>::type& items = section->getItems();
    for (size_t i = 0; i < items.size(); ++i)
    {
        const InventorySection::SectionItem& si = items[i];
        for (int dy = 0; dy < si.h; ++dy)
        {
            for (int dx = 0; dx < si.w; ++dx)
            {
                int cx = si.x + dx;
                int cy = si.y + dy;
                if (cx >= 0 && cx < gridW && cy >= 0 && cy < gridH) grid[cy * gridW + cx] = 1;
            }
        }
    }

    return grid;
}

static void applyLayout(InventorySection* section, const Packer::Result& result, const std::vector<Item*>& itemPtrs)
{
    ApplyGuard applyGuard;
    CallbackGuard guard(section);

    // Iterate snapshot backward (live vector mutates during removal)
    for (int i = (int)itemPtrs.size() - 1; i >= 0; --i)
        section->removeItem(itemPtrs[i]);

    // Rotation pass: ensure each item's dims match what the packer expects.
    // Compare current dims against p.w/p.h — handles both initial rotation
    // and un-rotating items changed by a previous sort.
    // All SetRotated calls happen before any placement (plan doc requirement).
    std::vector<bool> rotationFailed(result.placements.size(), false);
    if (StackSort_RotateAvailable())
    {
        for (size_t i = 0; i < result.placements.size(); ++i)
        {
            const Packer::Placement& p = result.placements[i];
            Item* item                 = itemPtrs[p.id];

            if (item->itemWidth == p.w && item->itemHeight == p.h) continue; // dims already match — no rotation needed

            bool currentlyRotated = StackSort_IsRotated(item);
            if (!StackSort_SetRotated(item, !currentlyRotated))
            {
                InfoLog("[StackSort] SetRotated failed for item " + IntToStr(p.id) + ", will auto-place");
                rotationFailed[i] = true;
            }
        }
    }

    // Placement pass: add items at computed positions.
    // Items that failed rotation fall back to auto-place at current dims.
    for (size_t i = 0; i < result.placements.size(); ++i)
    {
        const Packer::Placement& p = result.placements[i];
        if (rotationFailed[i]) section->addItem(itemPtrs[p.id], 1);
        else section->_NV__addItem(itemPtrs[p.id], p.x, p.y);
    }

    section->recalculateTotalWeight();
}

void StackSortUI::ApplySort(InventoryGUI* gui, int target, int& outAppliedLerSide, int& outSourceTarget)
{
    outAppliedLerSide = 0;
    outSourceTarget   = 0;

    Inventory* inv = gui->_NV_getInventory();
    if (!inv) return;

    Packer::TargetDim dim       = SortWorker::GetDim();
    const char* dimTag          = (dim == Packer::TARGET_H) ? "H" : "W";
    bool anySorted              = false;
    int maxSourceTargetThisCall = 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;

        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;

        const Ogre::vector<InventorySection::SectionItem>::type& sectionItems = section->getItems();
        if (sectionItems.empty()) continue;

        int gridW      = section->width;
        int gridH      = section->height;
        int sMaxTarget = ((dim == Packer::TARGET_H) ? gridH : gridW) - 1;

        int clamped = target;
        if (clamped > sMaxTarget) clamped = sMaxTarget;
        if (clamped < 1) clamped = 1;

        // Compute before-sort LER for comparison
        std::vector<unsigned char> beforeGrid = snapshotOccupancyGrid(section);
        int beforeLerArea = 0, beforeLerW = 0, beforeLerH = 0, beforeLerX = 0, beforeLerY = 0;
        Packer::ComputeLER(beforeGrid, gridW, gridH, beforeLerArea, beforeLerW, beforeLerH, beforeLerX, beforeLerY);

        // Try best precomputed result across all target values
        int sourceTarget = 0;
        Packer::Result precomputed;
        bool hasPrecomputed = SortWorker::GetBestResult(section, clamped, precomputed, &sourceTarget);

        if (hasPrecomputed && precomputed.allPlaced)
        {
            std::vector<Item*> ptrs;
            if (SortWorker::GetItemPtrs(section, ptrs) && ptrs.size() == precomputed.placements.size() &&
                Packer::ValidatePlacements(gridW, gridH, precomputed.placements))
            {
                std::string tStr = std::string("[") + dimTag + "] target=" + IntToStr(clamped);
                if (sourceTarget != clamped) tStr += " (via target=" + IntToStr(sourceTarget) + ")";

                InfoLog("[StackSort] Applying " + tStr + " for '" + section->name + "'" + " (" +
                        IntToStr((int)ptrs.size()) + " items)" + ", LER " + IntToStr(precomputed.lerWidth) + "x" +
                        IntToStr(precomputed.lerHeight) + "=" + IntToStr(precomputed.lerArea) + " at (" +
                        IntToStr(precomputed.lerX) + "," + IntToStr(precomputed.lerY) + ")" + " (was " +
                        IntToStr(beforeLerW) + "x" + IntToStr(beforeLerH) + "=" + IntToStr(beforeLerArea) + ")");

                applyLayout(section, precomputed, ptrs);
                int lerSide = (dim == Packer::TARGET_H) ? precomputed.lerHeight : precomputed.lerWidth;
                if (lerSide > outAppliedLerSide) outAppliedLerSide = lerSide;
                if (sourceTarget > maxSourceTargetThisCall) maxSourceTargetThisCall = sourceTarget;
                anySorted = true;
                continue;
            }
        }

        // Synchronous fallback
        std::vector<Packer::Item> packItems;
        std::vector<Item*> itemPtrs;

        for (size_t i = 0; i < sectionItems.size(); ++i)
        {
            const InventorySection::SectionItem& si = sectionItems[i];
            if (!si.item) continue;

            Packer::Item pi;
            pi.id         = (int)packItems.size();
            pi.w          = si.item->itemWidth;
            pi.h          = si.item->itemHeight;
            pi.canRotate  = StackSort_CanRotate(si.item);
            pi.itemTypeId = 0; // sync fallback: no grouping data
            packItems.push_back(pi);
            itemPtrs.push_back(si.item);
        }

        if (packItems.empty()) continue;

        InfoLog(std::string("[StackSort] Sync fallback [") + dimTag + "] target=" + IntToStr(clamped) + " for '" +
                section->name + "'" + " (" + IntToStr(gridW) + "x" + IntToStr(gridH) + ", " +
                IntToStr((int)packItems.size()) + " items)");

        QueryPerformanceCounter(&t0);
        Packer::Result result = Packer::Pack(gridW, gridH, packItems, dim, clamped);
        QueryPerformanceCounter(&t1);

        double ms = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart * 1000.0;

        if (!result.allPlaced)
        {
            InfoLog("[StackSort] Not all items placed (" + IntToStr((int)result.placements.size()) + "/" +
                    IntToStr((int)packItems.size()) + "), falling back to vanilla");
            section->_NV_autoArrange();
            continue;
        }

        if (!Packer::ValidatePlacements(gridW, gridH, result.placements))
        {
            ErrorLog("[StackSort] Placement validation failed, vanilla fallback");
            section->_NV_autoArrange();
            continue;
        }

        std::stringstream logMsg;
        logMsg << "[StackSort] Packed " << packItems.size() << " items in " << std::fixed;
        logMsg.precision(2);
        logMsg << ms << "ms"
               << ", LER " << result.lerWidth << "x" << result.lerHeight << "=" << result.lerArea << " (was "
               << beforeLerW << "x" << beforeLerH << "=" << beforeLerArea << ")"
               << ", score " << result.score;
        DebugLog(logMsg.str());

        applyLayout(section, result, itemPtrs);
        int lerSide = (dim == Packer::TARGET_H) ? result.lerHeight : result.lerWidth;
        if (lerSide > outAppliedLerSide) outAppliedLerSide = lerSide;
        // Sync fallback has no source slot — leave outSourceTarget at the
        // current target so the W-mode skip is a no-op.
        if (clamped > maxSourceTargetThisCall) maxSourceTargetThisCall = clamped;
        anySorted = true;
    }

    if (anySorted)
    {
        ((InventoryGUIAccess*)gui)->refreshAllSections();
        StackSort_RefreshVisuals(gui);
    }

    outSourceTarget = maxSourceTargetThisCall;
}

// Pointer snapshot only -- target->quantity is read LIVE during the merge
// loop so the inner break catches mid-loop increments. Snapshotting the
// pointers (not quantities) just keeps the iteration stable as addQuantity
// removes entries from the live items vector.
bool StackSortUI::ConsolidateStacks(InventorySection* section)
{
    ApplyGuard applyGuard;
    CallbackGuard guard(section);

    const Ogre::vector<InventorySection::SectionItem>::type& liveItems = section->getItems();
    std::vector<Item*> snapshot;
    snapshot.reserve(liveItems.size());
    for (size_t i = 0; i < liveItems.size(); ++i)
    {
        if (liveItems[i].item) snapshot.push_back(liveItems[i].item);
    }

    bool anyMerged = false;

    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        Item* target = snapshot[i];

        // addQuantity only removes source, never target, so the pointer holds.
        Inventory* inv = section->parentInventory;
        if (!inv || !inv->hasItem(target)) continue;

        int maxStack = target->isStackable(section);
        if (maxStack <= 1) continue;
        if (target->quantity >= maxStack) continue;

        for (size_t j = i + 1; j < snapshot.size(); ++j)
        {
            if (target->quantity >= maxStack) break;

            Item* source = snapshot[j];
            if (!inv->hasItem(source)) continue;
            if (!target->canStackWith(source)) continue;

            int amount = source->quantity;
            target->addQuantity(amount, source, section);
            anyMerged = true;
        }
    }

    return anyMerged;
}

// Called on first click (currentTarget==0), after consolidation but before
// ApplySort. Captures the human-arranged layout for right-click revert.
// Per-inventory scope: only captures sections of gui->_NV_getInventory().
void StackSortUI::CaptureOriginalPositions(InventoryGUI* gui)
{
    Inventory* inv = gui->_NV_getInventory();
    if (!inv) return;

    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;
        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;
        if (section->getItems().empty()) continue;

        CachedSection* cs = ResultCache::Find(section);
        if (!cs || cs->hasOriginalPositions) continue;

        const Ogre::vector<InventorySection::SectionItem>::type& sectionItems = section->getItems();
        cs->originalPositions.clear();
        cs->originalPositions.reserve(sectionItems.size());
        for (size_t i = 0; i < sectionItems.size(); ++i)
        {
            const InventorySection::SectionItem& si = sectionItems[i];
            if (!si.item) continue;
            OriginalPlacement op;
            op.item = si.item;
            op.x    = si.x;
            op.y    = si.y;
            op.w    = si.w;
            op.h    = si.h;
            cs->originalPositions.push_back(op);
        }
        cs->hasOriginalPositions = true;
    }
}

// Revert all eligible sections under gui's inventory to their original
// pre-sort positions. Returns true if any sections were reverted.
bool StackSortUI::RevertInventory(InventoryGUI* gui)
{
    Inventory* inv = gui->_NV_getInventory();
    if (!inv) return false;

    bool anySorted = false;

    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;
        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;
        if (section->getItems().empty()) continue;

        CachedSection* cs = ResultCache::Find(section);
        if (!cs || !cs->hasOriginalPositions) continue;

        // Build set of current items for matching
        const Ogre::vector<InventorySection::SectionItem>::type& currentItems = section->getItems();
        std::vector<Item*> currentPtrs;
        currentPtrs.reserve(currentItems.size());
        for (size_t i = 0; i < currentItems.size(); ++i)
        {
            if (currentItems[i].item) currentPtrs.push_back(currentItems[i].item);
        }

        // Build synthetic result from original positions
        Packer::Result result;
        result.lerArea       = 0;
        result.lerWidth      = 0;
        result.lerHeight     = 0;
        result.lerX          = 0;
        result.lerY          = 0;
        result.score         = 0;
        result.concentration = 0.0;
        result.strandedCells = 0;
        result.groupingBonus = 0;
        result.allPlaced     = false;

        std::vector<Item*> itemPtrs;
        std::vector<Item*> stragglers; // items not in original snapshot

        // Mark which current items are matched
        std::vector<bool> matched(currentPtrs.size(), false);

        for (size_t i = 0; i < cs->originalPositions.size(); ++i)
        {
            const OriginalPlacement& op = cs->originalPositions[i];

            // Find this item in current inventory
            bool found = false;
            for (size_t j = 0; j < currentPtrs.size(); ++j)
            {
                if (!matched[j] && currentPtrs[j] == op.item)
                {
                    Packer::Placement p;
                    p.id      = (int)itemPtrs.size();
                    p.x       = op.x;
                    p.y       = op.y;
                    p.w       = op.w;
                    p.h       = op.h;
                    p.rotated = false;
                    result.placements.push_back(p);
                    itemPtrs.push_back(op.item);
                    matched[j] = true;
                    found      = true;
                    break;
                }
            }
        }

        // Collect unmatched current items (added after original snapshot)
        for (size_t j = 0; j < currentPtrs.size(); ++j)
        {
            if (!matched[j]) stragglers.push_back(currentPtrs[j]);
        }

        if (result.placements.empty()) continue;

        result.allPlaced = stragglers.empty();

        // Apply the original layout for matched items
        applyLayout(section, result, itemPtrs);

        // Auto-place stragglers (items added after snapshot)
        if (!stragglers.empty())
        {
            for (size_t s = 0; s < stragglers.size(); ++s)
                section->addItem(stragglers[s], 1);
        }

        anySorted = true;
    }

    if (anySorted)
    {
        ((InventoryGUIAccess*)gui)->refreshAllSections();
        StackSort_RefreshVisuals(gui);
    }

    return anySorted;
}
