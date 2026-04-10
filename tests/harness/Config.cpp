// INI-style config file parser for the StackSort tuning harness.
// Sections: [search] (LAHC knobs), [moves] (percentages), [features] (bool flags).

#include "Config.h"

#include <fstream>
#include <sstream>
#include <cstdlib>

static std::string StripComments(const std::string& line)
{
    size_t pos  = line.find('#');
    size_t pos2 = line.find(';');
    if (pos2 != std::string::npos && pos2 < pos) pos = pos2;
    if (pos == std::string::npos) return line;
    return line.substr(0, pos);
}

static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool ParseBool(const std::string& v, bool def)
{
    std::string lower;
    for (size_t i = 0; i < v.size(); ++i)
    {
        char c = v[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        lower += c;
    }
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") return false;
    return def;
}

static std::string StemFromPath(const std::string& path)
{
    size_t slash     = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot       = base.find_last_of('.');
    if (dot == std::string::npos) return base;
    return base.substr(0, dot);
}

bool ParseConfigFile(const std::string& filePath, const Packer::SearchParams& base, HarnessConfig& out,
                     std::string& errMsg)
{
    std::ifstream f(filePath.c_str());
    if (!f.is_open())
    {
        errMsg = "Cannot open " + filePath;
        return false;
    }

    out.name   = StemFromPath(filePath);
    out.params = base;

    int swapPct   = -1;
    int insertPct = -1;
    int rotatePct = -1;

    std::string line;
    std::string section;
    int lineNum = 0;
    while (std::getline(f, line))
    {
        ++lineNum;
        std::string cleaned = Trim(StripComments(line));
        if (cleaned.empty()) continue;

        if (cleaned[0] == '[')
        {
            size_t close = cleaned.find(']');
            if (close == std::string::npos)
            {
                std::ostringstream ee;
                ee << filePath << ":" << lineNum << ": unterminated section header";
                errMsg = ee.str();
                return false;
            }
            section = cleaned.substr(1, close - 1);
            continue;
        }

        size_t eq = cleaned.find('=');
        if (eq == std::string::npos)
        {
            std::ostringstream ee;
            ee << filePath << ":" << lineNum << ": expected key = value";
            errMsg = ee.str();
            return false;
        }

        std::string key = Trim(cleaned.substr(0, eq));
        std::string val = Trim(cleaned.substr(eq + 1));

        if (section == "search")
        {
            if (key == "num_restarts") out.params.numRestarts = atoi(val.c_str());
            else if (key == "iters_per_restart") out.params.itersPerRestart = atoi(val.c_str());
            else if (key == "lahc_history_len") out.params.lahcHistoryLen = atoi(val.c_str());
            else if (key == "plateau_threshold") out.params.plateauThreshold = atoi(val.c_str());
            else if (key == "rng_seed") out.params.rngSeed = (unsigned int)atoi(val.c_str());
        }
        else if (section == "moves")
        {
            if (key == "swap_pct") swapPct = atoi(val.c_str());
            else if (key == "insert_pct") insertPct = atoi(val.c_str());
            else if (key == "rotate_pct") rotatePct = atoi(val.c_str());
            // repair_pct is implicit: 100 - (swap + insert + rotate)
        }
        else if (section == "features")
        {
            int flag = ParseBool(val, true) ? 1 : 0;
            if (key == "enable_baf_seed") out.params.enableBafSeed = flag;
            else if (key == "enable_unconstrained_fallback") out.params.enableUnconstrainedFallback = flag;
            else if (key == "enable_optimize_grouping") out.params.enableOptimizeGrouping = flag;
            else if (key == "enable_fast_converge") out.params.enableFastConverge = flag;
            else if (key == "enable_pre_reservation") out.params.enablePreReservation = flag;
            else if (key == "enable_repair_move") out.params.enableRepairMove = flag;
        }
        else if (section == "scoring")
        {
            if (key == "grouping_weight") out.params.scoringGroupingWeight = atoi(val.c_str());
            else if (key == "frag_weight") out.params.scoringFragWeight = atoi(val.c_str());
            else if (key == "grouping_power_quarters") out.params.groupingPowerQuarters = atoi(val.c_str());
        }
        else if (section == "skyline")
        {
            if (key == "waste_coef") out.params.skylineWasteCoef = atoi(val.c_str());
        }
        else if (section == "refine")
        {
            if (key == "num_restarts") out.refineRestarts = atoi(val.c_str());
            else if (key == "iters_per_restart") out.refineIters = atoi(val.c_str());
            else if (key == "lahc_history_len") out.refineLahcHist = atoi(val.c_str());
            else if (key == "plateau_threshold") out.refinePlateau = atoi(val.c_str());
            else if (key == "always") out.refineAlways = ParseBool(val, false) ? 1 : 0;
            else if (key == "stranded_max") out.refineStrandedMax = atoi(val.c_str());
            else if (key == "item_threshold") out.refineItemThreshold = atoi(val.c_str());
        }
    }

    // If any move pct was specified, compute cumulative thresholds.
    if (swapPct >= 0 || insertPct >= 0 || rotatePct >= 0)
    {
        if (swapPct < 0) swapPct = 50;
        if (insertPct < 0) insertPct = 25;
        if (rotatePct < 0) rotatePct = 15;
        out.params.moveSwapMax   = swapPct;
        out.params.moveInsertMax = swapPct + insertPct;
        out.params.moveRotateMax = swapPct + insertPct + rotatePct;
    }

    return true;
}
