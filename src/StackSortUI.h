#pragma once

#include <mygui/MyGUI_MouseButton.h>

class InventoryGUI;
class Inventory;
class InventorySection;

namespace MyGUI { class Widget; }

class StackSortUI
{
public:
    // Called from hook_autoArrangeButton. Initializes UI on first eligible
    // call, applies sort at current H, increments H, plays sound.
    // Returns true if handled, false if caller should fall through to vanilla.
    static bool OnForwardClick(InventoryGUI* gui, MyGUI::Widget* sender);

    // Called from hook_update each frame. Attempts early UI init via
    // childInventory->layoutMgr->getWidget("ArrangeButton") or
    // gui->layoutMgr->getWidget("ArrangeButton") for containers.
    static void OnUpdate(InventoryGUI* gui);

    // Called from hook_show on close. Clears cached widget pointers for
    // the specified GUI.
    static void OnClose(InventoryGUI* gui);

    // Public so the SortWorker dim toggle (or any future caller) can refresh
    // captions on all open UIs after changing the active dim.
    static void RefreshAllCaptions();

private:
    static void InitializeUI(InventoryGUI* gui, MyGUI::Widget* arrangeBtn,
                             InventoryGUI* ownerGUI);
    static void OnBackClick(MyGUI::Widget* sender);
    static void OnDimClick(MyGUI::Widget* sender);
    static void OnForwardArrowClick(MyGUI::Widget* sender);
    static void OnSortCycleClick(MyGUI::Widget* sender);
    static void OnSortRightClick(MyGUI::Widget* sender, int left, int top,
                                 MyGUI::MouseButton id);
    static void CaptureOriginalPositions(InventoryGUI* gui);

    // Revert all eligible sections to pre-sort positions. Returns true if
    // any sections were reverted. Caller handles UIState reset + caption.
    static bool RevertInventory(InventoryGUI* gui);

    static void FirstClickInit(int uiIdx);
    static void CycleTargetForward(int uiIdx);
    static void BuildTopFive(int uiIdx);

    // Apply sort at target value for all eligible sections under the active
    // dim. Sets outAppliedLerSide to max lerSide across eligible sections
    // (for forward-skip) and outSourceTarget to the source slot for W-mode skip.
    static void ApplySort(InventoryGUI* gui, int target,
                          int& outAppliedLerSide, int& outSourceTarget);

    static int FindMaxTarget(InventoryGUI* gui);
    static int PeekAppliedLerSide(InventoryGUI* gui, int target);
    static bool HasEligibleSections(Inventory* inv);
    static void PlayClickSound();
    static void UpdateCaption(int uiIdx);

    static int FindUIState(InventoryGUI* gui);
    static int AllocUIState(InventoryGUI* gui);

    // Merge stackable items in a section. Returns true if any merged.
    // Backpack sections only, main thread, wrapped in ApplyGuard+CallbackGuard.
    static bool ConsolidateStacks(InventorySection* section);
};
