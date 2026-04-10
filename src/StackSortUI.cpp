#pragma warning(push)
#pragma warning(disable : 4091)
#include <kenshi/Inventory.h>
#include <kenshi/gui/InventoryGUI.h>
#pragma warning(pop)

#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_MouseButton.h>

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

static std::string MakeCaption(int target)
{
    // Caption is dim-agnostic in the number; the dim toggle button shows H/W.
    std::stringstream ss;
    ss << "Stacksort [" << target << "]";
    return ss.str();
}

static const int MAX_GUIS = 16;

struct UIState
{
    InventoryGUI* gui;            // GUI whose ArrangeButton we hid
    InventoryGUI* ownerGUI;       // parent body GUI (== gui for body/container)
    MyGUI::Button* backButton;    // [<]
    MyGUI::Button* sortButton;    // [Stacksort [N]] — top-5 cycling + right-click revert
    MyGUI::Button* dimButton;     // [H|W]
    MyGUI::Button* forwardButton; // [>]
    int currentTarget;            // 0 = unsorted, else current target value
    bool initialized;
    bool stacksConsolidated;
    int topFiveTargets[5];
    int topFiveCount; // 0..5
    int topFiveIdx;   // -1 = not active, else 0..topFiveCount-1
};

static UIState s_uiStates[MAX_GUIS];

int StackSortUI::FindUIState(InventoryGUI* gui)
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (s_uiStates[i].initialized && s_uiStates[i].gui == gui) return i;
    }
    return -1;
}

int StackSortUI::AllocUIState(InventoryGUI* gui)
{
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (!s_uiStates[i].initialized)
        {
            UIState& ui           = s_uiStates[i];
            ui.gui                = gui;
            ui.ownerGUI           = NULL; // caller sets after alloc
            ui.backButton         = NULL;
            ui.sortButton         = NULL;
            ui.dimButton          = NULL;
            ui.forwardButton      = NULL;
            ui.currentTarget      = 0;
            ui.initialized        = false; // caller sets true after full init
            ui.stacksConsolidated = false;
            ui.topFiveCount       = 0;
            ui.topFiveIdx         = -1;
            return i;
        }
    }
    return -1;
}

void StackSortUI::PlayClickSound()
{
    StackSort_PlayClickSound();
}

void StackSortUI::UpdateCaption(int uiIdx)
{
    UIState& ui = s_uiStates[uiIdx];
    if (ui.sortButton) ui.sortButton->setCaption(MakeCaption(ui.currentTarget));
    if (ui.dimButton) ui.dimButton->setCaption((SortWorker::GetDim() == Packer::TARGET_H) ? "H" : "W");
}

void StackSortUI::RefreshAllCaptions()
{
    for (int i = 0; i < MAX_GUIS; ++i)
        if (s_uiStates[i].initialized) UpdateCaption(i);
}

bool StackSortUI::HasEligibleSections(Inventory* inv)
{
    if (!inv) return false;

    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;
        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;
        if (section->getItems().empty()) continue;
        return true;
    }
    return false;
}

int StackSortUI::FindMaxTarget(InventoryGUI* gui)
{
    Inventory* inv = gui->_NV_getInventory();
    if (!inv) return 0;

    Packer::TargetDim dim               = SortWorker::GetDim();
    int maxTarget                       = 0;
    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;
        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;
        if (section->getItems().empty()) continue;

        int sMaxTarget = SortWorker::GetMaxTarget(section);
        if (sMaxTarget == 0) sMaxTarget = ((dim == Packer::TARGET_H) ? section->height : section->width) - 1;
        if (sMaxTarget > maxTarget) maxTarget = sMaxTarget;
    }
    return maxTarget;
}

// Per-inventory scope: this GUI's own inventory sections only.
int StackSortUI::PeekAppliedLerSide(InventoryGUI* gui, int target)
{
    Inventory* inv = gui->_NV_getInventory();
    if (!inv) return 0;

    Packer::TargetDim dim = SortWorker::GetDim();
    int maxLerSide        = 0;

    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;

        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;
        if (section->getItems().empty()) continue;

        int gridSide   = (dim == Packer::TARGET_H) ? section->height : section->width;
        int sMaxTarget = gridSide - 1;

        int clamped = target;
        if (clamped > sMaxTarget) clamped = sMaxTarget;
        if (clamped < 1) clamped = 1;

        int sourceTarget = 0;
        Packer::Result r;
        if (SortWorker::GetBestResult(section, clamped, r, &sourceTarget) && r.allPlaced)
        {
            int lerSide = (dim == Packer::TARGET_H) ? r.lerHeight : r.lerWidth;
            if (lerSide > maxLerSide) maxLerSide = lerSide;
        }
    }

    return maxLerSide;
}

void StackSortUI::InitializeUI(InventoryGUI* gui, MyGUI::Widget* arrangeBtn, InventoryGUI* ownerGUI)
{
    // Already initialized for this GUI
    if (FindUIState(gui) >= 0) return;

    MyGUI::Widget* parent = arrangeBtn->getParent();
    if (!parent) return;

    int idx = AllocUIState(gui);
    if (idx < 0) return;

    UIState& ui = s_uiStates[idx];
    ui.ownerGUI = ownerGUI;

    // Anchor on the vanilla ArrangeButton's own coordinates — Kenshi's layout
    // system places it near the inventory grid, so we mirror that position.
    // Parent dimensions aren't reliable: for character body panels,
    // parent->getCoord() includes sidebar elements and reports a larger width
    // than the inventory grid.
    MyGUI::IntCoord ac = arrangeBtn->getCoord();
    MyGUI::IntCoord pc = parent->getCoord();

    // Body GUIs (character inventory with a child backpack) need a small
    // leftward nudge — the vanilla ArrangeButton is centered under the wider
    // body panel, but we want the new group under the main inventory grid.
    bool isBodyGUI          = (gui == ownerGUI && gui->childInventory != NULL);
    const int BODY_X_OFFSET = -40;

    DebugLog(std::string("[StackSort] Layout anchor: arrangeBtn (") + IntToStr(ac.left) + "," + IntToStr(ac.top) + " " +
             IntToStr(ac.width) + "x" + IntToStr(ac.height) + ")" + ", parent (" + IntToStr(pc.left) + "," +
             IntToStr(pc.top) + " " + IntToStr(pc.width) + "x" + IntToStr(pc.height) + ")" +
             (isBodyGUI ? " [body]" : ""));

    // Hide the vanilla ArrangeButton — the new 4-button group replaces it.
    // MyGUI recreates the widget on inventory reopen, so no restore needed.
    arrangeBtn->setVisible(false);

    // Layout: [<] [Stacksort [N]] [H|W] [>]
    // Horizontally centered on the vanilla button's own center.
    // Vertically aligned with the vanilla button's top.
    const int btnH      = ac.height;
    const int arrowBtnW = 30;
    const int dimBtnW   = 40;
    const int sortBtnW  = 180;
    const int gap       = 2;

    const int totalW         = arrowBtnW + gap + sortBtnW + gap + dimBtnW + gap + arrowBtnW;
    const int vanillaCenterX = ac.left + ac.width / 2;
    const int startX         = vanillaCenterX - totalW / 2 + (isBodyGUI ? BODY_X_OFFSET : 0);
    const int startY         = ac.top;

    const int xBack = startX;
    const int xSort = xBack + arrowBtnW + gap;
    const int xDim  = xSort + sortBtnW + gap;
    const int xFwd  = xDim + dimBtnW + gap;

    // Back button
    ui.backButton = parent->createWidget<MyGUI::Button>(
        "Kenshi_Button1", MyGUI::IntCoord(xBack, startY, arrowBtnW, btnH), MyGUI::Align::Default, "StackSortBack");
    ui.backButton->setCaption("<");
    ui.backButton->setTextAlign(MyGUI::Align::Center);
    ui.backButton->setUserData(MyGUI::Any(idx));
    ui.backButton->eventMouseButtonClick += MyGUI::newDelegate(OnBackClick);

    // Sort button (middle — top-5 cycling + right-click revert)
    ui.sortButton = parent->createWidget<MyGUI::Button>(
        "Kenshi_Button1", MyGUI::IntCoord(xSort, startY, sortBtnW, btnH), MyGUI::Align::Default, "StackSortSort");
    ui.sortButton->setTextAlign(MyGUI::Align::Center);
    ui.sortButton->setUserData(MyGUI::Any(idx));
    ui.sortButton->eventMouseButtonClick += MyGUI::newDelegate(OnSortCycleClick);
    ui.sortButton->eventMouseButtonPressed += MyGUI::newDelegate(OnSortRightClick);

    // Dim toggle
    ui.dimButton = parent->createWidget<MyGUI::Button>("Kenshi_Button1", MyGUI::IntCoord(xDim, startY, dimBtnW, btnH),
                                                       MyGUI::Align::Default, "StackSortDim");
    ui.dimButton->setTextAlign(MyGUI::Align::Center);
    ui.dimButton->setUserData(MyGUI::Any(idx));
    ui.dimButton->eventMouseButtonClick += MyGUI::newDelegate(OnDimClick);

    // Forward button
    ui.forwardButton = parent->createWidget<MyGUI::Button>(
        "Kenshi_Button1", MyGUI::IntCoord(xFwd, startY, arrowBtnW, btnH), MyGUI::Align::Default, "StackSortForward");
    ui.forwardButton->setCaption(">");
    ui.forwardButton->setTextAlign(MyGUI::Align::Center);
    ui.forwardButton->setUserData(MyGUI::Any(idx));
    ui.forwardButton->eventMouseButtonClick += MyGUI::newDelegate(OnForwardArrowClick);

    ui.initialized = true;
    UpdateCaption(idx);

    InfoLog("[StackSort] UI initialized (target=" + IntToStr(ui.currentTarget) +
            ", dim=" + ((SortWorker::GetDim() == Packer::TARGET_H) ? "H" : "W") + ")");
}

// First-click setup: stack consolidation + original-position capture.
// Per-inventory scope — only touches sections of ui.gui's inventory.
void StackSortUI::FirstClickInit(int uiIdx)
{
    UIState& ui = s_uiStates[uiIdx];
    if (ui.stacksConsolidated || ui.currentTarget != 0) return;

    Inventory* inv = ui.gui->_NV_getInventory();

    bool anyConsolidated = false;
    if (inv)
    {
        lektor<InventorySection*>& sections = inv->getAllSections();
        for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
        {
            InventorySection* section = *it;
            if (section->isAnEquippedItemSection) continue;
            if (!section->enabled) continue;
            if (section->getItems().empty()) continue;
            if (ConsolidateStacks(section)) anyConsolidated = true;
        }
    }

    ui.stacksConsolidated = true;

    if (anyConsolidated)
    {
        InfoLog("[StackSort] Consolidated stacks, triggering re-snapshot");
        SortWorker::OnMutation(ui.ownerGUI);
        ((InventoryGUIAccess*)ui.gui)->refreshAllSections();
    }

    // Capture original positions for right-click revert
    CaptureOriginalPositions(ui.gui);
}

// Forward-arrow cycling: increment target with forward-skip past covered values.
// Shared between the hook fallback (OnForwardClick) and the new > button.
void StackSortUI::CycleTargetForward(int uiIdx)
{
    UIState& ui = s_uiStates[uiIdx];
    if (!ui.initialized || !ui.gui) return;

    ui.topFiveIdx = -1; // exit top-5 cycling mode

    int maxTarget = FindMaxTarget(ui.gui);
    if (maxTarget < 1)
    {
        PlayClickSound();
        return;
    }

    FirstClickInit(uiIdx);

    ui.currentTarget++;
    if (ui.currentTarget > maxTarget) ui.currentTarget = 1;

    int appliedLerSide = 0;
    int sourceTarget   = 0;
    ApplySort(ui.gui, ui.currentTarget, appliedLerSide, sourceTarget);

    if (appliedLerSide > ui.currentTarget && appliedLerSide <= maxTarget) ui.currentTarget = appliedLerSide;

    PlayClickSound();
    UpdateCaption(uiIdx);
}

bool StackSortUI::OnForwardClick(InventoryGUI* gui, MyGUI::Widget* sender)
{
    Inventory* inv = gui->_NV_getInventory();

    int idx = FindUIState(gui);

    // Initialize UI on first click from a GUI with eligible sections
    if (idx < 0 && inv && HasEligibleSections(inv))
    {
        InventoryGUI* owner = gui->ownerInventory ? gui->ownerInventory : gui;
        InitializeUI(gui, sender, owner);
        idx = FindUIState(gui);
    }

    if (idx < 0) return false; // not handled — caller should fall through to vanilla

    CycleTargetForward(idx);
    return true;
}

void StackSortUI::OnForwardArrowClick(MyGUI::Widget* sender)
{
    int* pIdx = sender->getUserData<int>(false);
    if (!pIdx) return;
    int idx = *pIdx;
    if (idx < 0 || idx >= MAX_GUIS) return;
    CycleTargetForward(idx);
}

// Rank target values 1..maxTarget by aggregate GetBestResult score across
// this GUI's inventory sections. Top-5 distinct scores in descending order.
// Per-inventory scope: body button → body sections, backpack → backpack.
void StackSortUI::BuildTopFive(int uiIdx)
{
    UIState& ui     = s_uiStates[uiIdx];
    ui.topFiveCount = 0;

    Inventory* inv = ui.gui->_NV_getInventory();
    if (!inv) return;

    Packer::TargetDim dim = SortWorker::GetDim();

    const int MAX_T = 64;
    long long scoreByTarget[MAX_T];
    int validByTarget[MAX_T];
    for (int i = 0; i < MAX_T; ++i)
    {
        scoreByTarget[i] = 0;
        validByTarget[i] = 0;
    }

    int eligibleSections                = 0;
    lektor<InventorySection*>& sections = inv->getAllSections();
    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;
        if (section->isAnEquippedItemSection) continue;
        if (!section->enabled) continue;
        if (section->getItems().empty()) continue;
        ++eligibleSections;

        int sMax = ((dim == Packer::TARGET_H) ? section->height : section->width) - 1;
        if (sMax >= MAX_T) sMax = MAX_T - 1;
        for (int t = 1; t <= sMax; ++t)
        {
            Packer::Result r;
            if (SortWorker::GetBestResult(section, t, r, NULL) && r.allPlaced)
            {
                scoreByTarget[t] += r.score;
                ++validByTarget[t];
            }
        }
    }

    if (eligibleSections == 0) return;

    // Collect candidate targets where all sections returned a valid result.
    int candidateT[MAX_T];
    int numCandidates = 0;
    for (int t = 1; t < MAX_T; ++t)
    {
        if (validByTarget[t] == eligibleSections) candidateT[numCandidates++] = t;
    }

    // Insertion sort by score descending.
    for (int i = 1; i < numCandidates; ++i)
    {
        int tmp            = candidateT[i];
        long long tmpScore = scoreByTarget[tmp];
        int j              = i - 1;
        while (j >= 0 && scoreByTarget[candidateT[j]] < tmpScore)
        {
            candidateT[j + 1] = candidateT[j];
            --j;
        }
        candidateT[j + 1] = tmp;
    }

    // Take top-5 distinct scores. Adjacent targets often share the same
    // underlying best result (GetBestResult scan semantics) — dedup by score.
    long long lastScore = -1;
    for (int i = 0; i < numCandidates && ui.topFiveCount < 5; ++i)
    {
        int t       = candidateT[i];
        long long s = scoreByTarget[t];
        if (ui.topFiveCount > 0 && s == lastScore) continue;
        ui.topFiveTargets[ui.topFiveCount++] = t;
        lastScore                            = s;
    }
}

void StackSortUI::OnSortCycleClick(MyGUI::Widget* sender)
{
    int* pIdx = sender->getUserData<int>(false);
    if (!pIdx) return;
    int idx = *pIdx;
    if (idx < 0 || idx >= MAX_GUIS) return;

    UIState& ui = s_uiStates[idx];
    if (!ui.initialized || !ui.gui) return;

    FirstClickInit(idx);

    BuildTopFive(idx);
    if (ui.topFiveCount == 0)
    {
        // No precomputed results yet — fall back to forward cycling so the
        // user gets a response (ApplySort's sync fallback runs).
        CycleTargetForward(idx);
        return;
    }

    ui.topFiveIdx    = (ui.topFiveIdx + 1) % ui.topFiveCount;
    ui.currentTarget = ui.topFiveTargets[ui.topFiveIdx];

    int appliedLerSide = 0;
    int sourceTarget   = 0;
    ApplySort(ui.gui, ui.currentTarget, appliedLerSide, sourceTarget);

    PlayClickSound();
    UpdateCaption(idx);
}

void StackSortUI::OnBackClick(MyGUI::Widget* sender)
{
    int* pIdx = sender->getUserData<int>(false);
    if (!pIdx) return;

    int idx = *pIdx;
    if (idx < 0 || idx >= MAX_GUIS) return;

    UIState& ui = s_uiStates[idx];
    if (!ui.initialized || !ui.gui) return;

    ui.topFiveIdx = -1; // exit top-5 cycling mode

    int maxTarget = FindMaxTarget(ui.gui);
    if (maxTarget < 1) return;

    // Reverse of forward (which counts up): decrement with backward scan.
    // A stop is a target where PeekAppliedLerSide(t) <= t — forward skip
    // would not have jumped past it.
    int oldTarget = ui.currentTarget;
    int newTarget = ui.currentTarget - 1;
    if (newTarget < 1) newTarget = maxTarget;

    if (newTarget < oldTarget)
    {
        while (newTarget > 1)
        {
            int peekLerSide = PeekAppliedLerSide(ui.gui, newTarget);
            if (peekLerSide <= newTarget) break;
            --newTarget;
        }
        if (newTarget == 1)
        {
            int peekLerSide = PeekAppliedLerSide(ui.gui, 1);
            if (peekLerSide > 1) newTarget = maxTarget;
        }
    }

    ui.currentTarget   = newTarget;
    int appliedLerSide = 0;
    int sourceTarget   = 0;
    ApplySort(ui.gui, ui.currentTarget, appliedLerSide, sourceTarget);
    PlayClickSound();
    UpdateCaption(idx);
}

void StackSortUI::OnDimClick(MyGUI::Widget* sender)
{
    int* pIdx = sender->getUserData<int>(false);
    if (!pIdx) return;

    int idx = *pIdx;
    if (idx < 0 || idx >= MAX_GUIS) return;

    // Toggle global dim. Aborts all jobs and re-enqueues under the new dim.
    SortWorker::ToggleDim();

    // Reset currentTarget and top-5 state on all initialized UIs.
    // Do NOT reset stacksConsolidated — consolidation is per-inventory-open.
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        if (s_uiStates[i].initialized)
        {
            s_uiStates[i].currentTarget = 0;
            s_uiStates[i].topFiveIdx    = -1;
            s_uiStates[i].topFiveCount  = 0;
        }
    }

    RefreshAllCaptions();
    PlayClickSound();
}

void StackSortUI::OnSortRightClick(MyGUI::Widget* sender, int, int, MyGUI::MouseButton id)
{
    if (id != MyGUI::MouseButton(MyGUI::MouseButton::Right)) return;

    int* pIdx = sender->getUserData<int>(false);
    if (!pIdx) return;
    int idx = *pIdx;
    if (idx < 0 || idx >= MAX_GUIS) return;

    UIState& ui = s_uiStates[idx];
    if (!ui.initialized || !ui.gui) return;

    RevertInventory(ui.gui);

    ui.currentTarget      = 0;
    ui.stacksConsolidated = false;
    ui.topFiveIdx         = -1;
    ui.topFiveCount       = 0;
    UpdateCaption(idx);
    PlayClickSound();
}

void StackSortUI::OnUpdate(InventoryGUI* gui)
{
    // Already initialized for this GUI (or its child)
    if (FindUIState(gui) >= 0) return;

    if (!SortWorker::HasContext(gui)) return;

    // Path 1 (character): find ArrangeButton via childInventory
    if (gui->childInventory != NULL)
    {
        InventoryGUI* bpGUI = gui->childInventory;
        if (FindUIState(bpGUI) < 0 && bpGUI->layoutMgr)
        {
            MyGUI::Widget* btn = bpGUI->layoutMgr->getWidget("ArrangeButton");
            if (btn) InitializeUI(bpGUI, btn, gui);
        }

        // Path 1b (body): if body inventory itself has eligible non-equipped
        // sections, init its ArrangeButton too. Handles the 'main' section.
        if (FindUIState(gui) < 0 && gui->layoutMgr)
        {
            Inventory* bodyInv = gui->_NV_getInventory();
            if (bodyInv && HasEligibleSections(bodyInv))
            {
                MyGUI::Widget* bodyBtn = gui->layoutMgr->getWidget("ArrangeButton");
                if (bodyBtn) InitializeUI(gui, bodyBtn, gui);
            }
        }
        return;
    }

    // Path 2 (container): find ArrangeButton on the GUI itself.
    // Guard with HasEligibleSections — the body GUI can arrive here on the
    // first frame before childInventory is attached, and its sections are
    // all equipped. Without this guard we'd init on the body's ArrangeButton,
    // blocking the child path on subsequent frames.
    if (gui->layoutMgr)
    {
        Inventory* inv = gui->_NV_getInventory();
        if (inv && HasEligibleSections(inv))
        {
            MyGUI::Widget* btn = gui->layoutMgr->getWidget("ArrangeButton");
            if (btn) InitializeUI(gui, btn, gui);
        }
    }
}

void StackSortUI::OnClose(InventoryGUI* gui)
{
    if (!gui) return;

    // Scan all slots: clear any where gui matches directly (body/container)
    // OR where ownerGUI matches (child backpack owned by the closing parent).
    for (int i = 0; i < MAX_GUIS; ++i)
    {
        UIState& ui = s_uiStates[i];
        if (!ui.initialized) continue;
        if (ui.gui != gui && ui.ownerGUI != gui) continue;

        ui.backButton         = NULL;
        ui.sortButton         = NULL;
        ui.dimButton          = NULL;
        ui.forwardButton      = NULL;
        ui.gui                = NULL;
        ui.ownerGUI           = NULL;
        ui.initialized        = false;
        ui.currentTarget      = 0;
        ui.stacksConsolidated = false;
        ui.topFiveCount       = 0;
        ui.topFiveIdx         = -1;
    }
}
