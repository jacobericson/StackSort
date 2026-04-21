#include "CatalogDump.h"

#ifdef STACKSORT_VERBOSE

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <set>
#include <string>
#include <fstream>

#pragma warning(push)
#pragma warning(disable : 4091)
#include <kenshi/Inventory.h>
#include <kenshi/GameData.h>
#include <kenshi/RootObject.h>
#include <kenshi/gui/InventoryGUI.h>
#pragma warning(pop)

#include "Log.h"
#include "StackSort.h" // StackSort_CanRotate

static std::set<std::string> s_seen;
static std::ofstream s_out;
static bool s_inited = false;

// Dedup for the diagnostic log lines. Scan fires once per GUI update
// frame (~60Hz), which floods the log otherwise. We only need to see the
// Scan/ScanInv topology once per (GUI, primaryInv) pair — subsequent
// frames on the same GUI skip the diagnostic emission but still run the
// actual catalog walk (catalog writes are dedup'd by s_seen already).
static std::set<void*> s_loggedContexts;
static bool s_diagThisCall = false;

static void DiagLog(const std::string& msg)
{
    if (s_diagThisCall) LogInfo(msg);
}

static std::string GetDLLDirectory()
{
    char path[MAX_PATH];
    HMODULE hm = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&GetDLLDirectory, &hm);
    GetModuleFileNameA(hm, path, sizeof(path));
    std::string dir(path);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir;
}

// Tab/newline/CR → space so names can't break TSV parsing.
static std::string Sanitize(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i)
    {
        char c = out[i];
        if (c == '\t' || c == '\n' || c == '\r') out[i] = ' ';
    }
    return out;
}

// Hex pointer formatting for diagnostics — keeps log lines short and lets
// us compare pointers across calls to see when the same Inventory* is
// returned by two different accessors.
static std::string PtrHex(const void* p)
{
    if (!p) return std::string("0x0");
    char buf[32];
    std::sprintf(buf, "0x%p", p);
    return std::string(buf);
}

static const char* CATALOG_HEADER_V1 = "name\tw\th\tcanRotate\tgameDataType\titemFunction\tfoodCrop\ttradeItem"
                                       "\tprice\tsellPrice\tweight";

void CatalogDump::Init()
{
    if (s_inited) return;

    std::string path = GetDLLDirectory() + "stacksort_catalog.tsv";

    // If the existing file's header doesn't match the current schema, delete
    // it and start fresh — mixed-schema files break downstream TSV parsing.
    // Matching-header files preseed the dedup set so reruns accumulate.
    bool fileExists = false;
    {
        std::ifstream in(path.c_str());
        if (in.is_open())
        {
            std::string header;
            std::getline(in, header);
            if (header == CATALOG_HEADER_V1)
            {
                fileExists = true;
                std::string line;
                while (std::getline(in, line))
                {
                    if (line.empty()) continue;
                    size_t tab = line.find('\t');
                    if (tab == std::string::npos) continue;
                    s_seen.insert(line.substr(0, tab));
                }
            }
            else
            {
                LogInfo("[CatalogDump] stale schema detected, resetting catalog");
                in.close();
                DeleteFileA(path.c_str());
            }
        }
    }

    s_out.open(path.c_str(), std::ios::out | std::ios::app);
    if (!s_out.is_open())
    {
        LogError("[CatalogDump] Failed to open " + path);
        return;
    }

    if (!fileExists)
    {
        s_out << CATALOG_HEADER_V1 << "\n";
        s_out.flush();
    }

    s_inited = true;

    LogInfo("[CatalogDump] initialized (" + IntToStr((int)s_seen.size()) + " known, " + path + ")");
}

// Depth limit guards against pathological container-in-container loops (not
// expected in legitimate Kenshi saves, but defensive). 4 is more than enough
// for the deepest real case: trader → shop container → for-sale backpack → contents.
static const int MAX_CATALOG_RECURSION_DEPTH = 4;

static void ScanInv(Inventory* inv, int depth)
{
    if (!inv || depth > MAX_CATALOG_RECURSION_DEPTH) return;

    lektor<InventorySection*>& sections = inv->getAllSections();
    if (s_diagThisCall)
    {
        int totalItems = 0;
        for (lektor<InventorySection*>::iterator sIt = sections.begin(); sIt != sections.end(); ++sIt)
        {
            if (*sIt) totalItems += (int)(*sIt)->getItems().size();
        }
        DiagLog("[CatalogDump.ScanInv] depth=" + IntToStr(depth) + " inv=" + PtrHex(inv) +
                " sections=" + IntToStr((int)sections.size()) + " totalItems=" + IntToStr(totalItems));
    }

    for (lektor<InventorySection*>::iterator it = sections.begin(); it != sections.end(); ++it)
    {
        InventorySection* section = *it;
        if (!section) continue;

        const Ogre::vector<InventorySection::SectionItem>::type& items = section->getItems();

        for (size_t i = 0; i < items.size(); ++i)
        {
            const InventorySection::SectionItem& si = items[i];
            if (!si.item || !si.item->data) continue;

            const std::string& name = si.item->data->name;
            if (name.empty()) continue;

            // Recurse into container items first, so we walk a trader's
            // storage-chest contents and a character's backpack-inside-a-
            // backpack before potentially skipping duplicates via s_seen.
            // ContainerItem::inventory holds the child inventory (Item.h:311).
            if (si.item->data->type == CONTAINER)
            {
                ContainerItem* cnt = (ContainerItem*)si.item;
                if (cnt->inventory) ScanInv(cnt->inventory, depth + 1);
            }

            if (s_seen.insert(name).second)
            {
                GameData* gd = si.item->data;

                // Tier 3: GameData::type enum. See Enums.h itemType.
                int gameDataType = (int)gd->type;

                // Tier 4: Item::itemFunction enum.
                int itemFunction = (int)si.item->itemFunction;

                // Tier 5: CDATA bdata flag. Plan specifies key "food crop"
                // (lowercase, space-separated — matches every other bdata
                // key seen in sibling plugins: "is public", "unique",
                // "town name override", "hide hair", etc.). Key string
                // UNVERIFIED by static analysis — not referenced in any
                // other Kenshi plugin; relies on dev-build dump to
                // confirm. The companion log below enumerates every
                // bdata key for each new item so the user can grep for
                // the real spelling after a full catalog scan.
                typedef boost::unordered::unordered_map<
                    std::string, bool, boost::hash<std::string>, std::equal_to<std::string>,
                    Ogre::STLAllocator<std::pair<std::string const, bool>, Ogre::GeneralAllocPolicy> >
                    BdataMap;

                int foodCrop = 0;
                {
                    BdataMap::const_iterator bit = gd->bdata.find("food crop");
                    if (bit != gd->bdata.end() && bit->second) foodCrop = 1;
                }

                int tradeItem = 0;
                {
                    BdataMap::const_iterator bit = gd->bdata.find("trade item");
                    if (bit != gd->bdata.end() && bit->second) tradeItem = 1;
                }

                int price     = si.item->getAvgPrice();
                int sellPrice = si.item->getValueSingle(true);
                float weight  = si.item->getItemWeightSingle();

                char weightBuf[32];
                std::sprintf(weightBuf, "%.2f", weight);

                s_out << Sanitize(name) << '\t' << si.item->itemWidth << '\t' << si.item->itemHeight << '\t'
                      << (StackSort_CanRotate(si.item) ? 1 : 0) << '\t' << gameDataType << '\t' << itemFunction << '\t'
                      << foodCrop << '\t' << tradeItem << '\t' << price << '\t' << sellPrice << '\t' << weightBuf
                      << '\n';
                s_out.flush();

                // One-shot bdata-key enumeration per distinct item —
                // verifies the "food crop" key string and surfaces any
                // other tier-useful flags (e.g. "trade item"). Logged at
                // Info so dev builds capture it without verbose spam.
                if (!gd->bdata.empty())
                {
                    std::string keys;
                    for (BdataMap::const_iterator it2 = gd->bdata.begin(); it2 != gd->bdata.end(); ++it2)
                    {
                        if (!keys.empty()) keys += ", ";
                        keys += it2->first;
                        keys += "=";
                        keys += (it2->second ? "1" : "0");
                    }
                    LogInfo("[CatalogDump] bdata keys for '" + name + "': " + keys);
                }
            }
        }
    }
}

void CatalogDump::Scan(InventoryGUI* gui)
{
    if (!s_inited || !gui) return;

    // One-shot diagnostic: emit topology on the first frame a new GUI is
    // seen, then silence. Prevents the ~60Hz update loop from flooding
    // the log. Catalog writes still happen on every frame (s_seen dedups).
    s_diagThisCall = (s_loggedContexts.insert((void*)gui).second);

    // Diagnostic entry banner. `ownerInventory` and `childInventory` let
    // us tell trader-main from child-backpack-GUI from character-main
    // without needing the vtable detector (we don't have it wired here).
    DiagLog("[CatalogDump.Scan] gui=" + PtrHex(gui) + " ownerInv=" + PtrHex(gui->ownerInventory) +
            " childInv=" + PtrHex(gui->childInventory));

    // The GUI's own inventory accessor. Semantics vary by GUI subclass:
    //  - Character GUI → Character's personal Inventory (worn + carried)
    //  - Container GUI → the container's Inventory
    //  - Trader GUI    → the trader Character's personal Inventory (just
    //    their equipped gear — NOT the shop sale stock). The merged
    //    ShopTraderInventory lives on the ShopTrader callback object,
    //    reached below.
    Inventory* primaryInv = gui->_NV_getInventory();
    DiagLog("[CatalogDump.Scan] primaryInv=" + PtrHex(primaryInv));
    ScanInv(primaryInv, 0);

    // Callback object's inventory — the real source for trader shops.
    // RootObject::getInventory() is virtual, so it dispatches correctly:
    //  - For ShopTrader (the callback on an InventoryTraderGUI): returns
    //    ShopTraderInventory, which aggregates the trader Character's
    //    inventory plus every storage-chest ContainerItem attached to the
    //    trader's shop building.
    //  - For Character / ContainerItem callbacks: returns the same
    //    Inventory* we already scanned via primaryInv. s_seen dedups.
    //
    // The container-recursion inside ScanInv then walks into every
    // ContainerItem in the aggregated view, capturing the full sale stock
    // including items inside shop chests and nested backpacks.
    RootObject* callback = gui->_NV_getCallbackObject();
    if (callback)
    {
        // KenshiLib convention: `_NV_*` calls the named RVA *non-virtually*,
        // bypassing vtable dispatch. For getInventory, RootObject's base
        // implementation (RVA 0x38F960) returns NULL — it has no inventory
        // member itself. To reach the Character / ShopTrader / ContainerItem
        // override, we MUST go through the vtable via the bare virtual name.
        int cbType       = (int)callback->getDataType();
        Inventory* cbInv = callback->getInventory();
        DiagLog("[CatalogDump.Scan] callback=" + PtrHex(callback) + " type=" + IntToStr(cbType) +
                " cbInv=" + PtrHex(cbInv) + " sameAsPrimary=" + (cbInv == primaryInv ? "YES" : "NO"));
        if (cbInv && cbInv != primaryInv) ScanInv(cbInv, 0);
    }
    else
    {
        DiagLog("[CatalogDump.Scan] callback=NULL");
    }

    // Equipped backpack via the GUI's explicit getter. Usually redundant
    // with the recursion above (backpack appears as a CONTAINER item in
    // the primary inventory), but retained as a safety net for flows where
    // the backpack is only reachable through this accessor. Idempotent —
    // s_seen dedups.
    ContainerItem* backpack = gui->getBackpack();
    DiagLog("[CatalogDump.Scan] backpack=" + PtrHex(backpack));
    if (backpack && backpack->inventory) ScanInv(backpack->inventory, 0);
}

#else // !STACKSORT_VERBOSE

void CatalogDump::Init() {}
void CatalogDump::Scan(InventoryGUI*) {}

#endif
