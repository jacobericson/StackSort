#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <Windows.h>

#include <core/Functions.h>

#pragma warning(push)
#pragma warning(disable : 4091) // '__declspec(dllimport)' ignored (BaseLayout.h)
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Inventory.h>
#include <kenshi/gui/InventoryGUI.h>
// InventoryTraderGUI detection is via vtable pointer comparison (no header needed)
#pragma warning(pop)

#include <mygui/MyGUI_Widget.h>

#include "ResultCache.h"
#include "SortWorker.h"
#include "StackSort.h"
#include "StackSortUI.h"
#include "Log.h"

#ifdef STACKSORT_VERBOSE
#include "CatalogDump.h"
#endif

#ifdef STACKSORT_ENABLE_ROTATION
typedef int (*KR_ApiVersionFn)();
typedef int (*KR_IsRotatedFn)(void*);
typedef int (*KR_CanRotateFn)(void*);
typedef int (*KR_SetRotatedFn)(void*, int, void*);
typedef void (*KR_RefreshVisualsFn)(void*);

static KR_ApiVersionFn s_krApiVersion         = NULL;
static KR_IsRotatedFn s_krIsRotated           = NULL;
static KR_CanRotateFn s_krCanRotate           = NULL;
static KR_SetRotatedFn s_krSetRotated         = NULL;
static KR_RefreshVisualsFn s_krRefreshVisuals = NULL;
static bool s_krAvailable                     = false;
#endif

// Resolved from kenshi_x64.exe at startup.

typedef unsigned long (*AK_PostEvent_t)(const char*,      // eventName
                                        unsigned __int64, // gameObjectID
                                        unsigned long,    // flags
                                        void*,            // callback
                                        void*,            // cookie
                                        unsigned long,    // numExternals
                                        void*,            // externalSources
                                        unsigned long     // playingID
);

static AK_PostEvent_t s_akPostEvent = NULL;

// Capture base InventoryGUI vtable from first character body GUI (has
// childInventory != NULL). Any GUI with a different vtable is a subclass
// (trader). Containers share the base vtable.
// Before capture, isTraderGUI returns false (fail-open, harmless).

static void** s_baseVtable = NULL;

static bool isTraderGUI(InventoryGUI* gui)
{
    if (!s_baseVtable) return false;
    return *(void***)gui != s_baseVtable;
}

#ifdef STACKSORT_ENABLE_ROTATION
void StackSort_InitRotateAPI()
{
    HMODULE hMod = GetModuleHandleA("KenshiRotate.dll");
    if (!hMod)
    {
        LogInfo("[StackSort] KenshiRotate not detected (rotation disabled)");
        return;
    }

    s_krApiVersion     = (KR_ApiVersionFn)GetProcAddress(hMod, "KenshiRotate_ApiVersion");
    s_krIsRotated      = (KR_IsRotatedFn)GetProcAddress(hMod, "KenshiRotate_IsRotated");
    s_krCanRotate      = (KR_CanRotateFn)GetProcAddress(hMod, "KenshiRotate_CanRotate");
    s_krSetRotated     = (KR_SetRotatedFn)GetProcAddress(hMod, "KenshiRotate_SetRotated");
    s_krRefreshVisuals = (KR_RefreshVisualsFn)GetProcAddress(hMod, "KenshiRotate_RefreshVisuals");

    if (!s_krApiVersion || !s_krIsRotated || !s_krCanRotate || !s_krSetRotated)
    {
        LogError("[StackSort] KenshiRotate found but missing API exports (rotation disabled)");
        return;
    }

    int ver = s_krApiVersion();
    if (ver < 1)
    {
        LogError("[StackSort] KenshiRotate API version " + IntToStr(ver) + " < 1 (rotation disabled)");
        return;
    }

    s_krAvailable = true;
    LogInfo("[StackSort] KenshiRotate detected (API v" + IntToStr(ver) + ", rotation enabled)");
}

bool StackSort_RotateAvailable()
{
    return s_krAvailable;
}

bool StackSort_CanRotate(Item* item)
{
    return s_krAvailable && s_krCanRotate((void*)item) != 0;
}

bool StackSort_IsRotated(Item* item)
{
    return s_krAvailable && s_krIsRotated((void*)item) != 0;
}

bool StackSort_SetRotated(Item* item, bool rotated)
{
    return s_krAvailable && s_krSetRotated((void*)item, rotated ? 1 : 0, NULL) != 0;
}

void StackSort_RefreshVisuals(InventoryGUI* gui)
{
    if (s_krAvailable && s_krRefreshVisuals) s_krRefreshVisuals((void*)gui);
}
#endif

void StackSort_PlayClickSound()
{
    if (s_akPostEvent) s_akPostEvent("General_Click", 0x6E, 0, NULL, NULL, 0, NULL, 0);
}

typedef void (*autoArrangeButton_t)(InventoryGUI*, MyGUI::Widget*);
static autoArrangeButton_t orig_autoArrangeButton = NULL;

static void hook_autoArrangeButton(InventoryGUI* thisptr, MyGUI::Widget* sender)
{
    // Trader: fall through to vanilla sort.
    // Only check for non-child GUIs (child backpack GUIs share the base
    // InventoryGUI vtable but we don't want to risk misclassification).
    if (thisptr->ownerInventory == NULL && isTraderGUI(thisptr))
    {
        if (orig_autoArrangeButton) orig_autoArrangeButton(thisptr, sender);
        return;
    }

    Inventory* inv = thisptr->_NV_getInventory();
    if (!inv)
    {
        if (orig_autoArrangeButton) orig_autoArrangeButton(thisptr, sender);
        return;
    }

    // OnForwardClick returns false if it can't handle the click
    // (no UIState, no eligible sections) — fall through to vanilla
    if (!StackSortUI::OnForwardClick(thisptr, sender))
    {
        if (orig_autoArrangeButton) orig_autoArrangeButton(thisptr, sender);
    }
}

typedef void (*show_t)(InventoryGUI*, bool);
static show_t orig_show = NULL;

static void hook_show(InventoryGUI* thisptr, bool on)
{
    if (!on)
    {
        if (SortWorker::HasContext(thisptr))
        {
            LogInfo("[StackSort] Inventory closed");
            SortWorker::OnInventoryClosed(thisptr);
            StackSortUI::OnClose(thisptr);
        }
        // Safety net: also close child's UI directly (OnClose's ownerGUI
        // scan handles this, but childInventory may still be valid here).
        if (thisptr->childInventory) StackSortUI::OnClose(thisptr->childInventory);
    }

    orig_show(thisptr, on);
}

typedef void (*update_t)(InventoryGUI*);
static update_t orig_update = NULL;

static void hook_update(InventoryGUI* thisptr)
{
    // Let the game process needItemsUpdate first — this populates sections
    // with items on the frame the inventory opens. Must run before we snapshot.
    orig_update(thisptr);

    // Skip child GUIs (same pattern as KenshiRotate)
    if (thisptr->ownerInventory != NULL) return;

    // Skip non-visible GUIs. During rapid inventory cycling, _NV_update can
    // fire for a GUI on the same frame as show(false), or on the frame after
    // before the game stops calling update. Gating on visible prevents
    // creating stale contexts or initializing UI on closing GUIs.
    if (!thisptr->visible) return;

#ifdef STACKSORT_VERBOSE
    // Runs BEFORE the trader skip below so trader stock contributes to the
    // catalog. Dedup makes per-frame cost negligible.
    CatalogDump::Scan(thisptr);
#endif

    // Capture base InventoryGUI vtable from first character body GUI
    // (character body has childInventory != NULL, traders/containers don't)
    if (!s_baseVtable && thisptr->childInventory != NULL) s_baseVtable = *(void***)thisptr;

    // Skip trader GUIs (different vtable than base InventoryGUI)
    if (isTraderGUI(thisptr)) return;

    // Detect stale context: Kenshi reuses InventoryGUI objects when switching
    // characters without calling show(false)/show(true). The underlying
    // inventory changes but our context still references the old one.
    if (SortWorker::HasContext(thisptr) && !SortWorker::ContextMatchesInventory(thisptr))
    {
        LogInfo("[StackSort] Stale context detected, resetting");
        SortWorker::OnInventoryClosed(thisptr);
        StackSortUI::OnClose(thisptr);
        if (thisptr->childInventory) StackSortUI::OnClose(thisptr->childInventory);
    }

    // Create context for this GUI if it doesn't have one yet
    if (!SortWorker::HasContext(thisptr)) SortWorker::OnInventoryOpened(thisptr);

    // Check if a debounced worker spawn is due
    SortWorker::PollWorkerStart();

    // Attempt early UI initialization (caption + back button)
    StackSortUI::OnUpdate(thisptr);
}

typedef void (*sectionAddCb_t)(Inventory*, Item*);
static sectionAddCb_t orig_sectionAddCb = NULL;

static void hook_sectionAddItemCallback(Inventory* thisptr, Item* item)
{
    orig_sectionAddCb(thisptr, item);
    if (!SortWorker::IsApplying() && SortWorker::IsTrackedInventory(thisptr))
    {
        InventoryGUI* gui = SortWorker::FindContextGUIForInventory(thisptr);
        if (gui) SortWorker::OnMutation(gui);
    }
}

typedef void (*sectionRemoveCb_t)(Inventory*, Item*);
static sectionRemoveCb_t orig_sectionRemoveCb = NULL;

static void hook_sectionRemoveItemCallback(Inventory* thisptr, Item* item)
{
    orig_sectionRemoveCb(thisptr, item);
    if (!SortWorker::IsApplying() && SortWorker::IsTrackedInventory(thisptr))
    {
        InventoryGUI* gui = SortWorker::FindContextGUIForInventory(thisptr);
        if (gui) SortWorker::OnMutation(gui);
    }
}

__declspec(dllexport) void startPlugin()
{
    LogInfo("[StackSort] Starting plugin v0.7.0...");

    // Hook 1: autoArrangeButton — replace vanilla sort
    if (KenshiLib::SUCCESS != KenshiLib::AddHook((void*)KenshiLib::GetRealAddress(&InventoryGUI::autoArrangeButton),
                                                 (void*)hook_autoArrangeButton, (void**)&orig_autoArrangeButton))
    {
        LogError("[StackSort] FATAL: Failed to hook autoArrangeButton");
        return;
    }
    LogInfo("[StackSort] Hooked autoArrangeButton OK");

    // Hook 2: show — inventory close detection
    if (KenshiLib::SUCCESS != KenshiLib::AddHook((void*)KenshiLib::GetRealAddress(&InventoryGUI::_NV_show),
                                                 (void*)hook_show, (void**)&orig_show))
    {
        LogError("[StackSort] WARNING: Failed to hook show (non-fatal)");
    }
    else
    {
        LogInfo("[StackSort] Hooked show OK");
    }

    // Hook 3: _NV_update — open detection + worker poll
    if (KenshiLib::SUCCESS != KenshiLib::AddHook((void*)KenshiLib::GetRealAddress(&InventoryGUI::_NV_update),
                                                 (void*)hook_update, (void**)&orig_update))
    {
        LogError("[StackSort] WARNING: Failed to hook _NV_update (non-fatal)");
    }
    else
    {
        LogInfo("[StackSort] Hooked _NV_update OK");
    }

    // Hook 4: _sectionAddItemCallback — mutation detection
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook((void*)KenshiLib::GetRealAddress(&Inventory::_NV__sectionAddItemCallback),
                           (void*)hook_sectionAddItemCallback, (void**)&orig_sectionAddCb))
    {
        LogError("[StackSort] WARNING: Failed to hook sectionAddItemCallback (non-fatal)");
    }
    else
    {
        LogInfo("[StackSort] Hooked sectionAddItemCallback OK");
    }

    // Hook 5: _sectionRemoveItemCallback — mutation detection
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook((void*)KenshiLib::GetRealAddress(&Inventory::_NV__sectionRemoveItemCallback),
                           (void*)hook_sectionRemoveItemCallback, (void**)&orig_sectionRemoveCb))
    {
        LogError("[StackSort] WARNING: Failed to hook sectionRemoveItemCallback (non-fatal)");
    }
    else
    {
        LogInfo("[StackSort] Hooked sectionRemoveItemCallback OK");
    }

    LogInfo("[StackSort] Plugin loaded successfully (5 hooks)");

    // Initialize subsystems
    ResultCache::Init();
    SortWorker::Init();
#ifdef STACKSORT_VERBOSE
    CatalogDump::Init();
#endif
    // No-op unless STACKSORT_ENABLE_ROTATION is defined at compile time.
    StackSort_InitRotateAPI();

    // Resolve AK::SoundEngine::PostEvent from kenshi_x64.exe
    {
        HMODULE hGame = GetModuleHandleA(NULL);
        if (hGame)
        {
            s_akPostEvent =
                (AK_PostEvent_t)GetProcAddress(hGame, "?PostEvent@SoundEngine@AK@@YAKPEBD_KKP6AXW4AkCallbackType@@"
                                                      "PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z");
            if (s_akPostEvent)
            {
                LogInfo("[StackSort] AK::SoundEngine::PostEvent resolved");
            }
            else
            {
                LogError("[StackSort] WARNING: AK::SoundEngine::PostEvent not found (no click sound)");
            }
        }
    }
}
