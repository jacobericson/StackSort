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
#include <kenshi/gui/InventoryGUI.h>
#pragma warning(pop)

#include "Log.h"
#include "StackSort.h" // StackSort_CanRotate

static std::set<std::string> s_seen;
static std::ofstream s_out;
static bool s_inited = false;

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

void CatalogDump::Init()
{
    if (s_inited) return;

    std::string path = GetDLLDirectory() + "stacksort_catalog.tsv";

    // Preseed dedup set from any existing file so we accumulate across runs.
    bool fileExists = false;
    {
        std::ifstream in(path.c_str());
        if (in.is_open())
        {
            fileExists = true;
            std::string line;
            bool firstLine = true;
            while (std::getline(in, line))
            {
                if (firstLine)
                {
                    firstLine = false;
                    continue;
                } // header
                if (line.empty()) continue;
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                s_seen.insert(line.substr(0, tab));
            }
        }
    }

    s_out.open(path.c_str(), std::ios::out | std::ios::app);
    if (!s_out.is_open())
    {
        ErrorLog("[CatalogDump] Failed to open " + path);
        return;
    }

    if (!fileExists)
    {
        s_out << "name\tw\th\tcanRotate\n";
        s_out.flush();
    }

    s_inited = true;

    InfoLog("[CatalogDump] initialized (" + IntToStr((int)s_seen.size()) + " known, " + path + ")");
}

static void ScanInv(Inventory* inv)
{
    if (!inv) return;

    lektor<InventorySection*>& sections = inv->getAllSections();
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

            if (s_seen.insert(name).second)
            {
                s_out << Sanitize(name) << '\t' << si.item->itemWidth << '\t' << si.item->itemHeight << '\t'
                      << (StackSort_CanRotate(si.item) ? 1 : 0) << '\n';
                s_out.flush();
            }
        }
    }
}

void CatalogDump::Scan(InventoryGUI* gui)
{
    if (!s_inited || !gui) return;

    // Body inventory via virtual dispatch -- correct override fires for
    // character / container / trader GUIs alike.
    ScanInv(gui->_NV_getInventory());

    // Backpack only exists on character GUIs; NULL for traders/containers.
    ContainerItem* backpack = gui->getBackpack();
    if (backpack && backpack->inventory) ScanInv(backpack->inventory);
}

#else // !STACKSORT_VERBOSE

void CatalogDump::Init() {}
void CatalogDump::Scan(InventoryGUI*) {}

#endif
