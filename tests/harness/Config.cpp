// INI-style config file parser for the StackSort tuning harness.
// Sections: [search] (LAHC knobs), [moves] (percentages), [features] (bool flags).

#include "Config.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>

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

static bool ParseInt(const std::string& val, int& out, std::string& errMsg, const std::string& file, int line,
                     const std::string& key)
{
    const char* s = val.c_str();
    char* endp    = NULL;
    errno         = 0;
    long v        = std::strtol(s, &endp, 10);
    if (endp == s || *endp != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
    {
        std::ostringstream ee;
        ee << file << ":" << line << ": invalid integer for '" << key << "': " << val;
        errMsg = ee.str();
        return false;
    }
    out = (int)v;
    return true;
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
            if (key == "num_restarts")
            {
                if (!ParseInt(val, out.params.numRestarts, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "iters_per_restart")
            {
                if (!ParseInt(val, out.params.itersPerRestart, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "lahc_history_len")
            {
                if (!ParseInt(val, out.params.lahcHistoryLen, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "plateau_threshold")
            {
                if (!ParseInt(val, out.params.plateauThreshold, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "rng_seed")
            {
                int tmp;
                if (!ParseInt(val, tmp, errMsg, filePath, lineNum, key)) return false;
                out.params.rngSeed = (unsigned int)tmp;
            }
        }
        else if (section == "moves")
        {
            if (key == "swap_pct")
            {
                if (!ParseInt(val, swapPct, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "insert_pct")
            {
                if (!ParseInt(val, insertPct, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "rotate_pct")
            {
                if (!ParseInt(val, rotatePct, errMsg, filePath, lineNum, key)) return false;
            }
            // repair_pct is implicit: 100 - (swap + insert + rotate)
            else if (key == "late_bias_alpha_q")
            {
                if (!ParseInt(val, out.params.lateBiasAlphaQ, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "late_bias_uniform_pct")
            {
                if (!ParseInt(val, out.params.lateBiasUniformPct, errMsg, filePath, lineNum, key)) return false;
            }
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
            else if (key == "enable_strip_shift") out.params.enableStripShift = flag;
            else if (key == "enable_tile_swap") out.params.enableTileSwap = flag;
            else if (key == "enable_path_relinking") out.params.enablePathRelinking = flag;
        }
        else if (section == "scoring")
        {
            if (key == "grouping_weight")
            {
                if (!ParseInt(val, out.params.scoringGroupingWeight, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "frag_weight")
            {
                if (!ParseInt(val, out.params.scoringFragWeight, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "grouping_power_quarters")
            {
                if (!ParseInt(val, out.params.groupingPowerQuarters, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "tier_weight_exact")
            {
                if (!ParseInt(val, out.params.tierWeightExact, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "tier_weight_custom")
            {
                if (!ParseInt(val, out.params.tierWeightCustom, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "tier_weight_type")
            {
                if (!ParseInt(val, out.params.tierWeightType, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "tier_weight_function")
            {
                if (!ParseInt(val, out.params.tierWeightFunction, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "tier_weight_flags")
            {
                if (!ParseInt(val, out.params.tierWeightFlags, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "function_sim_food_food_restricted")
            {
                if (!ParseInt(val, out.params.funcSimFoodFoodRestricted, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "function_sim_firstaid_robotrepair")
            {
                if (!ParseInt(val, out.params.funcSimFirstaidRobotrepair, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "soft_grouping_pct")
            {
                if (!ParseInt(val, out.params.softGroupingPct, errMsg, filePath, lineNum, key)) return false;
            }
        }
        else if (section == "skyline")
        {
            if (key == "waste_coef")
            {
                if (!ParseInt(val, out.params.skylineWasteCoef, errMsg, filePath, lineNum, key)) return false;
            }
        }
        else if (section == "refine")
        {
            if (key == "num_restarts")
            {
                if (!ParseInt(val, out.refineRestarts, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "iters_per_restart")
            {
                if (!ParseInt(val, out.refineIters, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "lahc_history_len")
            {
                if (!ParseInt(val, out.refineLahcHist, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "plateau_threshold")
            {
                if (!ParseInt(val, out.refinePlateau, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "always")
            {
                out.refineAlways = ParseBool(val, false) ? 1 : 0;
            }
            else if (key == "stranded_max")
            {
                if (!ParseInt(val, out.refineStrandedMax, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "item_threshold")
            {
                if (!ParseInt(val, out.refineItemThreshold, errMsg, filePath, lineNum, key)) return false;
            }
        }
        else if (section == "path_relinking")
        {
            if (key == "elite_cap")
            {
                if (!ParseInt(val, out.params.pathRelinkEliteCap, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "diversity_pct")
            {
                if (!ParseInt(val, out.params.pathRelinkDiversityPct, errMsg, filePath, lineNum, key)) return false;
            }
            else if (key == "max_path_len")
            {
                if (!ParseInt(val, out.params.pathRelinkMaxPathLen, errMsg, filePath, lineNum, key)) return false;
            }
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
