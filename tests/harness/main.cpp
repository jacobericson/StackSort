// StackSort tuning harness -- standalone console executable.
// Runs one config across a corpus of instances, writing one CSV row per
// (instance x seed x target) tuple. No game dependencies -- links only
// the four Packer*.cpp files plus this harness.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <exception>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Packer.h"
#include "RefineCriteria.h"
#include "Instance.h"
#include "Config.h"
#include "Csv.h"

// build.bat passes /DSTACKSORT_GIT_SHA="<short sha>". Ad-hoc builds fall through.
#ifndef STACKSORT_GIT_SHA
#define STACKSORT_GIT_SHA "unknown"
#endif

static const int CSV_SCHEMA_VERSION = 3;

struct Args
{
    std::string corpusDir;
    std::string configFile;
    std::string outFile;
    int numSeeds;
    unsigned int baseSeed;
    int rotateMode;
    int refineMode;
    int minTarget;
    int maxTarget;

    Args() : numSeeds(1), baseSeed(1), rotateMode(0), refineMode(1), minTarget(0), maxTarget(0) {}
};

static void PrintUsage()
{
    (void)fprintf(stderr, "Usage: stacksort_bench --corpus DIR --config FILE --out FILE [options]\n"
                          "  --corpus DIR       Directory containing instance .txt files\n"
                          "  --config FILE      Path to INI config file (inherits from sibling baseline.ini)\n"
                          "  --out FILE         Output CSV path\n"
                          "  --seeds N          Number of seeds per instance (default 1)\n"
                          "  --base-seed N      Base RNG seed (default 1, per-seed = base + idx)\n"
                          "  --rotate {0|1}     Force canRotate on all items (default 0)\n"
                          "  --refine {0|1}     Run production two-pass first+refinement (default 1)\n"
                          "  --targets MIN:MAX  Target range (default 1:gridH-1)\n");
}

static bool ParseIntArg(const char* val, int& out, const char* argName)
{
    char* endp = NULL;
    errno      = 0;
    long v     = strtol(val, &endp, 10);
    if (endp == val || *endp != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
    {
        (void)fprintf(stderr, "Invalid integer for %s: %s\n", argName, val);
        return false;
    }
    out = (int)v;
    return true;
}

static bool ParseArgs(int argc, char** argv, Args& out)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--corpus" && i + 1 < argc)
        {
            out.corpusDir = argv[++i];
        }
        else if (a == "--config" && i + 1 < argc)
        {
            out.configFile = argv[++i];
        }
        else if (a == "--out" && i + 1 < argc)
        {
            out.outFile = argv[++i];
        }
        else if (a == "--seeds" && i + 1 < argc)
        {
            if (!ParseIntArg(argv[++i], out.numSeeds, "--seeds")) return false;
        }
        else if (a == "--base-seed" && i + 1 < argc)
        {
            int tmp;
            if (!ParseIntArg(argv[++i], tmp, "--base-seed")) return false;
            out.baseSeed = (unsigned int)tmp;
        }
        else if (a == "--rotate" && i + 1 < argc)
        {
            if (!ParseIntArg(argv[++i], out.rotateMode, "--rotate")) return false;
        }
        else if (a == "--refine" && i + 1 < argc)
        {
            if (!ParseIntArg(argv[++i], out.refineMode, "--refine")) return false;
        }
        else if (a == "--targets" && i + 1 < argc)
        {
            std::string r = argv[++i];
            size_t colon  = r.find(':');
            if (colon == std::string::npos)
            {
                (void)fprintf(stderr, "Invalid --targets format (expected MIN:MAX): %s\n", r.c_str());
                return false;
            }
            if (!ParseIntArg(r.substr(0, colon).c_str(), out.minTarget, "--targets MIN")) return false;
            if (!ParseIntArg(r.substr(colon + 1).c_str(), out.maxTarget, "--targets MAX")) return false;
        }
        else if (a == "--help" || a == "-h")
        {
            PrintUsage();
            return false;
        }
        else
        {
            (void)fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            PrintUsage();
            return false;
        }
    }
    if (out.corpusDir.empty() || out.configFile.empty() || out.outFile.empty())
    {
        PrintUsage();
        return false;
    }
    return true;
}

static std::vector<std::string> ListInstanceFiles(const std::string& dir)
{
    std::vector<std::string> out;
    std::string pattern = dir + "\\*.txt";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

static std::string IntToStr(long long v)
{
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

static std::string DoubleToStr(double v, int prec = 4)
{
    std::ostringstream oss;
    oss.precision(prec);
    oss << std::fixed << v;
    return oss.str();
}

static std::string BoolToStr(bool v)
{
    return v ? "1" : "0";
}

static std::vector<std::string> BuildHeader()
{
    std::vector<std::string> h;
    h.push_back("schema_version");
    h.push_back("timestamp");
    h.push_back("git_sha");
    h.push_back("config_name");
    h.push_back("instance_name");
    h.push_back("seed");
    h.push_back("target");
    h.push_back("grid_w");
    h.push_back("grid_h");
    h.push_back("num_items");
    h.push_back("num_types");
    h.push_back("density");
    h.push_back("dim");
    h.push_back("rotate_all");
    h.push_back("num_restarts");
    h.push_back("iters_per_restart");
    h.push_back("lahc_history_len");
    h.push_back("plateau_threshold");
    h.push_back("move_swap_max");
    h.push_back("move_insert_max");
    h.push_back("move_rotate_max");
    h.push_back("enable_baf_seed");
    h.push_back("enable_unconstrained_fallback");
    h.push_back("enable_optimize_grouping");
    h.push_back("enable_fast_converge");
    h.push_back("enable_repair_move");
    h.push_back("enable_pre_reservation");
    h.push_back("enable_strip_shift");
    h.push_back("enable_tile_swap");
    h.push_back("enable_path_relinking");
    h.push_back("all_placed");
    h.push_back("num_placed");
    h.push_back("score");
    h.push_back("ler_area");
    h.push_back("ler_width");
    h.push_back("ler_height");
    h.push_back("ler_x");
    h.push_back("ler_y");
    h.push_back("concentration");
    h.push_back("stranded_cells");
    h.push_back("grouping_bonus");
    h.push_back("num_rotated");
    h.push_back("lahc_iters_executed");
    h.push_back("best_found_iter");
    h.push_back("best_found_restart");
    h.push_back("plateau_breaks");
    h.push_back("unconstrained_fallback_won");
    h.push_back("greedy_seed_score");
    h.push_back("greedy_seed_ler_area");
    h.push_back("wall_clock_ms");
    h.push_back("pack_calls");
    h.push_back("notes");
    h.push_back("refine_mode");
    h.push_back("first_pass_score");
    h.push_back("first_pass_ler_area");
    h.push_back("first_pass_ler_width");
    h.push_back("first_pass_ler_height");
    h.push_back("first_pass_all_placed");
    h.push_back("first_pass_num_placed");
    h.push_back("first_pass_concentration");
    h.push_back("first_pass_stranded");
    h.push_back("first_pass_grouping_bonus");
    h.push_back("first_pass_num_rotated");
    h.push_back("first_pass_wall_ms");
    h.push_back("refine_triggered");
    h.push_back("refine_replaced");
    h.push_back("refine_wall_ms");
    h.push_back("scoring_grouping_weight");
    h.push_back("scoring_frag_weight");
    h.push_back("repair_rolls");
    h.push_back("repair_scans");
    h.push_back("repair_hits");
    h.push_back("repair_accepts");
    h.push_back("first_pass_repair_rolls");
    h.push_back("first_pass_repair_scans");
    h.push_back("first_pass_repair_hits");
    h.push_back("first_pass_repair_accepts");
    h.push_back("strip_shift_strips_found");
    h.push_back("strip_shift_strips_improved");
    h.push_back("tile_swap_candidates_found");
    h.push_back("tile_swap_candidates_committed");
    h.push_back("first_pass_strip_shift_strips_found");
    h.push_back("first_pass_strip_shift_strips_improved");
    h.push_back("first_pass_tile_swap_candidates_found");
    h.push_back("first_pass_tile_swap_candidates_committed");
    h.push_back("path_relink_pairs_run");
    h.push_back("path_relink_intermediates_scored");
    h.push_back("path_relink_global_best_updates");
    h.push_back("path_relink_aborted_paths");
    h.push_back("path_relink_avg_path_len_sum");
    h.push_back("path_relink_global_best_gain_max");
    h.push_back("first_pass_path_relink_pairs_run");
    h.push_back("first_pass_path_relink_intermediates_scored");
    h.push_back("first_pass_path_relink_global_best_updates");
    h.push_back("first_pass_path_relink_aborted_paths");
    h.push_back("first_pass_path_relink_avg_path_len_sum");
    h.push_back("first_pass_path_relink_global_best_gain_max");
    h.push_back("scoring_grouping_power_quarters");
    h.push_back("skyline_waste_coef");
    h.push_back("tier_weight_exact");
    h.push_back("tier_weight_custom");
    h.push_back("tier_weight_type");
    h.push_back("tier_weight_function");
    h.push_back("tier_weight_flags");
    h.push_back("function_sim_food_food_restricted");
    h.push_back("function_sim_firstaid_robotrepair");
    h.push_back("soft_grouping_pct");
    h.push_back("grouping_borders_raw");
    h.push_back("grouping_bonus_exact");
    h.push_back("fp_skyline_snap_hits");
    h.push_back("fp_skyline_snap_probes");
#ifdef STACKSORT_PROFILE
    h.push_back("fp_cycles_move_gen");
    h.push_back("fp_cycles_skyline_pack");
    h.push_back("fp_cycles_ler");
    h.push_back("fp_cycles_concentration");
    h.push_back("fp_cycles_grouping");
    h.push_back("fp_cycles_stranded");
    h.push_back("fp_cycles_score");
    h.push_back("fp_cycles_accept");
    h.push_back("fp_cycles_pre_reservation");        // per-run: reserve-width probe loop
    h.push_back("fp_cycles_greedy_seed");            // per-run: BSSF + BAF + selection
    h.push_back("fp_cycles_unconstrained_fallback"); // per-run: optional fallback PackH
    h.push_back("fp_cycles_optimize_grouping");      // per-run: post-LAHC same-footprint swap
    h.push_back("fp_cycles_strip_shift");            // per-run: post-LAHC strip permutation
    h.push_back("fp_cycles_tile_swap");              // per-run: post-LAHC multi-placement swap
    h.push_back("fp_cycles_path_relink");            // per-run: post-LAHC path relinking over elite pool
    h.push_back("fp_cycles_borders_raw");            // per-run: final cross-power clustering metric
    h.push_back("fp_kept_prefix_sum");
    h.push_back("fp_kept_prefix_count");
    h.push_back("fp_grid_hash_probes");
    h.push_back("fp_grid_hash_hits");
    h.push_back("fp_cycles_skyline_prefix");
    h.push_back("fp_cycles_skyline_waste_map");
    h.push_back("fp_cycles_skyline_candidate");
    h.push_back("fp_cycles_skyline_adjacency");
    h.push_back("fp_cycles_skyline_commit");
    h.push_back("fp_cycles_ler_histogram");
    h.push_back("fp_cycles_ler_stack");
#endif
    return h;
}

static int run(int argc, char** argv)
{
    Args args;
    if (!ParseArgs(argc, argv, args)) return 1;

    // CPU affinity pinning. Honored by both profile and non-profile builds
    // so run_matrix.py --pin-shards works uniformly. Mask comes from the env
    // var STACKSORT_PROFILE_AFFINITY (name kept for backward compat). Default
    // mask = 1 (core 0) so rdtsc deltas stay on one TSC domain in profile
    // builds and so parallel shards get distinct cores via --pin-shards in
    // non-profile. Set env var to "0" to disable pinning entirely.
    {
        const char* affEnv = getenv("STACKSORT_PROFILE_AFFINITY");
        DWORD_PTR mask     = 1;
        if (affEnv && *affEnv) mask = (DWORD_PTR)_strtoui64(affEnv, NULL, 0);
        if (mask != 0) SetProcessAffinityMask(GetCurrentProcess(), mask);
    }

    // Load baseline first so ablation configs can inherit from it.
    std::string configDir = ".";
    {
        size_t slash = args.configFile.find_last_of("/\\");
        if (slash != std::string::npos) configDir = args.configFile.substr(0, slash);
    }
    std::string baselinePath = configDir + "/baseline.ini";

    HarnessConfig baselineCfg;
    std::string err;
    if (!ParseConfigFile(baselinePath, Packer::SearchParams::defaults(), baselineCfg, err))
    {
        (void)fprintf(stderr, "Baseline config error: %s\n", err.c_str());
        return 1;
    }

    HarnessConfig targetCfg;
    {
        const std::string& a = args.configFile;
        const std::string& b = baselinePath;
        // Normalize trivial path differences before comparing
        if (a == b)
        {
            targetCfg = baselineCfg;
        }
        else if (!ParseConfigFile(args.configFile, baselineCfg.params, targetCfg, err))
        {
            (void)fprintf(stderr, "Target config error: %s\n", err.c_str());
            return 1;
        }
    }

    std::vector<std::string> instancePaths = ListInstanceFiles(args.corpusDir);
    if (instancePaths.empty())
    {
        (void)fprintf(stderr, "No instance files in %s\n", args.corpusDir.c_str());
        return 1;
    }

    CsvWriter csv;
    if (!csv.Open(args.outFile))
    {
        (void)fprintf(stderr, "Cannot open output %s\n", args.outFile.c_str());
        return 1;
    }

    csv.WriteRow(BuildHeader());

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    std::string tsStr = IntToStr((long long)time(NULL));

    std::string progressFile = args.outFile + ".progress";

    int totalRuns = 0;
    for (size_t ii = 0; ii < instancePaths.size(); ++ii)
    {
        Instance inst;
        if (!ParseInstanceFile(instancePaths[ii], inst, err))
        {
            (void)fprintf(stderr, "Skip %s: %s\n", instancePaths[ii].c_str(), err.c_str());
            continue;
        }

        for (size_t j = 0; j < inst.items.size(); ++j)
            inst.items[j].canRotate = (args.rotateMode != 0);

        int maxT   = std::max(inst.gridH - 1, 1);
        int startT = (args.minTarget > 0) ? args.minTarget : 1;
        int endT   = (args.maxTarget > 0) ? args.maxTarget : maxT;
        endT       = std::min(endT, maxT);
        startT     = std::max(startT, 1);

        std::set<int> types;
        int totalArea = 0;
        for (size_t k = 0; k < inst.items.size(); ++k)
        {
            types.insert(inst.items[k].exactId);
            totalArea += inst.items[k].w * inst.items[k].h;
        }
        int numTypes   = (int)types.size();
        double density = (double)totalArea / (double)(inst.gridW * inst.gridH);

        for (int seedIdx = 0; seedIdx < args.numSeeds; ++seedIdx)
        {
            for (int target = startT; target <= endT; ++target)
            {
                Packer::SearchParams runParams = targetCfg.params;
                runParams.rngSeed              = args.baseSeed + (unsigned int)seedIdx;

                // --- First pass
                std::vector<Packer::Item> firstPassBest;
                Packer::PackDiagnostics firstDiag;
                LARGE_INTEGER tFP0, tFP1;
                QueryPerformanceCounter(&tFP0);
                Packer::Result firstPass =
                    Packer::PackAnnealed(inst.gridW, inst.gridH, inst.items, Packer::TARGET_H, target, NULL, NULL,
                                         &firstPassBest, 0, &runParams, &firstDiag);
                QueryPerformanceCounter(&tFP1);
                double firstPassMs = (double)(tFP1.QuadPart - tFP0.QuadPart) * 1000.0 / (double)freq.QuadPart;

                // --- Optional refinement (production two-pass)
                Packer::Result finalResult        = firstPass;
                Packer::PackDiagnostics finalDiag = firstDiag;
                int refineTriggered               = 0;
                int refineReplaced                = 0;
                double refineMs                   = 0.0;

                if (args.refineMode)
                {
                    int perpendicular  = inst.gridW; // H-mode
                    int totalFreeCells = inst.gridW * inst.gridH;
                    for (size_t k = 0; k < inst.items.size(); ++k)
                        totalFreeCells -= inst.items[k].w * inst.items[k].h;

                    int strandedMax = (targetCfg.refineStrandedMax >= 0) ? targetCfg.refineStrandedMax : 0;
                    int itemThreshold =
                        (targetCfg.refineItemThreshold >= 0) ? targetCfg.refineItemThreshold : REFINE_ITEM_THRESHOLD;

                    if (targetCfg.refineAlways == 1 ||
                        NeedsRefinement(firstPass, Packer::TARGET_H, perpendicular, target, totalFreeCells,
                                        (int)inst.items.size(), strandedMax, itemThreshold))
                    {
                        refineTriggered = 1;

                        Packer::SearchParams refineParams = runParams;
                        refineParams.numRestarts =
                            (targetCfg.refineRestarts > 0) ? targetCfg.refineRestarts : REFINE_RESTARTS;
                        refineParams.itersPerRestart =
                            (targetCfg.refineIters > 0) ? targetCfg.refineIters : REFINE_ITERS;
                        refineParams.lahcHistoryLen =
                            (targetCfg.refineLahcHist > 0) ? targetCfg.refineLahcHist : REFINE_LAHC_HIST;
                        refineParams.plateauThreshold =
                            (targetCfg.refinePlateau > 0) ? targetCfg.refinePlateau : REFINE_PLATEAU;
                        // Distinct RNG stream so refinement doesn't replay the first pass.
                        refineParams.rngSeed = runParams.rngSeed ^ 0x5A5A5A5Au;

                        std::vector<Packer::Item> refineBest;
                        Packer::PackDiagnostics refineDiag;
                        LARGE_INTEGER tR0, tR1;
                        QueryPerformanceCounter(&tR0);
                        Packer::Result refined = Packer::PackAnnealed(
                            inst.gridW, inst.gridH, inst.items, Packer::TARGET_H, target, NULL,
                            firstPassBest.empty() ? NULL : &firstPassBest, &refineBest, 0, &refineParams, &refineDiag);
                        QueryPerformanceCounter(&tR1);
                        refineMs = (double)(tR1.QuadPart - tR0.QuadPart) * 1000.0 / (double)freq.QuadPart;

                        if (refined.allPlaced && refined.score > firstPass.score)
                        {
                            finalResult    = refined;
                            finalDiag      = refineDiag;
                            refineReplaced = 1;
                        }
                    }
                }

                double totalWallMs = firstPassMs + refineMs;

                int numRotated = 0;
                for (size_t k = 0; k < finalResult.placements.size(); ++k)
                    if (finalResult.placements[k].rotated) ++numRotated;

                int firstPassNumRotated = 0;
                for (size_t k = 0; k < firstPass.placements.size(); ++k)
                    if (firstPass.placements[k].rotated) ++firstPassNumRotated;

                std::vector<std::string> row;
                row.push_back(IntToStr(CSV_SCHEMA_VERSION));
                row.push_back(tsStr);
                row.push_back(STACKSORT_GIT_SHA);
                row.push_back(targetCfg.name);
                row.push_back(inst.name);
                row.push_back(IntToStr((long long)args.baseSeed + (long long)seedIdx));
                row.push_back(IntToStr(target));
                row.push_back(IntToStr(inst.gridW));
                row.push_back(IntToStr(inst.gridH));
                row.push_back(IntToStr((int)inst.items.size()));
                row.push_back(IntToStr(numTypes));
                row.push_back(DoubleToStr(density));
                row.push_back("H");
                row.push_back(IntToStr(args.rotateMode));
                row.push_back(IntToStr(runParams.numRestarts));
                row.push_back(IntToStr(runParams.itersPerRestart));
                row.push_back(IntToStr(runParams.lahcHistoryLen));
                row.push_back(IntToStr(runParams.plateauThreshold));
                row.push_back(IntToStr(runParams.moveSwapMax));
                row.push_back(IntToStr(runParams.moveInsertMax));
                row.push_back(IntToStr(runParams.moveRotateMax));
                row.push_back(IntToStr(runParams.enableBafSeed));
                row.push_back(IntToStr(runParams.enableUnconstrainedFallback));
                row.push_back(IntToStr(runParams.enableOptimizeGrouping));
                row.push_back(IntToStr(runParams.enableFastConverge));
                row.push_back(IntToStr(runParams.enableRepairMove));
                row.push_back(IntToStr(runParams.enablePreReservation));
                row.push_back(IntToStr(runParams.enableStripShift));
                row.push_back(IntToStr(runParams.enableTileSwap));
                row.push_back(IntToStr(runParams.enablePathRelinking));
                row.push_back(BoolToStr(finalResult.allPlaced));
                row.push_back(IntToStr((int)finalResult.placements.size()));
                row.push_back(IntToStr(finalResult.score));
                row.push_back(IntToStr(finalResult.lerArea));
                row.push_back(IntToStr(finalResult.lerWidth));
                row.push_back(IntToStr(finalResult.lerHeight));
                row.push_back(IntToStr(finalResult.lerX));
                row.push_back(IntToStr(finalResult.lerY));
                row.push_back(DoubleToStr(finalResult.concentration, 6));
                row.push_back(IntToStr(finalResult.strandedCells));
                row.push_back(IntToStr(finalResult.groupingBonus));
                row.push_back(IntToStr(numRotated));
                row.push_back(IntToStr(finalDiag.lahcItersExecuted));
                row.push_back(IntToStr(finalDiag.bestFoundIter));
                row.push_back(IntToStr(finalDiag.bestFoundRestart));
                row.push_back(IntToStr(finalDiag.plateauBreaks));
                row.push_back(BoolToStr(finalDiag.unconstrainedFallbackWon));
                row.push_back(IntToStr(finalDiag.greedySeedScore));
                row.push_back(IntToStr(finalDiag.greedySeedLerArea));
                row.push_back(DoubleToStr(totalWallMs, 3));
                row.push_back(IntToStr(finalDiag.packCalls));
                row.push_back("");
                row.push_back(IntToStr(args.refineMode));
                row.push_back(IntToStr(firstPass.score));
                row.push_back(IntToStr(firstPass.lerArea));
                row.push_back(IntToStr(firstPass.lerWidth));
                row.push_back(IntToStr(firstPass.lerHeight));
                row.push_back(BoolToStr(firstPass.allPlaced));
                row.push_back(IntToStr((int)firstPass.placements.size()));
                row.push_back(DoubleToStr(firstPass.concentration, 6));
                row.push_back(IntToStr(firstPass.strandedCells));
                row.push_back(IntToStr(firstPass.groupingBonus));
                row.push_back(IntToStr(firstPassNumRotated));
                row.push_back(DoubleToStr(firstPassMs, 3));
                row.push_back(IntToStr(refineTriggered));
                row.push_back(IntToStr(refineReplaced));
                row.push_back(DoubleToStr(refineMs, 3));
                int resolvedGW = (runParams.scoringGroupingWeight > 0) ? runParams.scoringGroupingWeight
                                                                       : Packer::DEFAULT_GROUPING_WEIGHT;
                int resolvedFW =
                    (runParams.scoringFragWeight > 0) ? runParams.scoringFragWeight : Packer::DEFAULT_FRAG_WEIGHT;
                row.push_back(IntToStr(resolvedGW));
                row.push_back(IntToStr(resolvedFW));
                row.push_back(IntToStr(finalDiag.repairMoveRolls));
                row.push_back(IntToStr(finalDiag.repairMoveScans));
                row.push_back(IntToStr(finalDiag.repairMoveHits));
                row.push_back(IntToStr(finalDiag.repairMoveAccepts));
                row.push_back(IntToStr(firstDiag.repairMoveRolls));
                row.push_back(IntToStr(firstDiag.repairMoveScans));
                row.push_back(IntToStr(firstDiag.repairMoveHits));
                row.push_back(IntToStr(firstDiag.repairMoveAccepts));
                row.push_back(IntToStr(finalDiag.stripShiftStripsFound));
                row.push_back(IntToStr(finalDiag.stripShiftStripsImproved));
                row.push_back(IntToStr(finalDiag.tileSwapCandidatesFound));
                row.push_back(IntToStr(finalDiag.tileSwapCandidatesCommitted));
                row.push_back(IntToStr(firstDiag.stripShiftStripsFound));
                row.push_back(IntToStr(firstDiag.stripShiftStripsImproved));
                row.push_back(IntToStr(firstDiag.tileSwapCandidatesFound));
                row.push_back(IntToStr(firstDiag.tileSwapCandidatesCommitted));
                row.push_back(IntToStr(finalDiag.pathRelinkPairsRun));
                row.push_back(IntToStr(finalDiag.pathRelinkIntermediatesScored));
                row.push_back(IntToStr(finalDiag.pathRelinkGlobalBestUpdates));
                row.push_back(IntToStr(finalDiag.pathRelinkAbortedPaths));
                row.push_back(IntToStr(finalDiag.pathRelinkAvgPathLenSum));
                row.push_back(IntToStr(finalDiag.pathRelinkGlobalBestGainMax));
                row.push_back(IntToStr(firstDiag.pathRelinkPairsRun));
                row.push_back(IntToStr(firstDiag.pathRelinkIntermediatesScored));
                row.push_back(IntToStr(firstDiag.pathRelinkGlobalBestUpdates));
                row.push_back(IntToStr(firstDiag.pathRelinkAbortedPaths));
                row.push_back(IntToStr(firstDiag.pathRelinkAvgPathLenSum));
                row.push_back(IntToStr(firstDiag.pathRelinkGlobalBestGainMax));
                int resolvedGPQ = (runParams.groupingPowerQuarters >= 1 && runParams.groupingPowerQuarters <= 8)
                                      ? runParams.groupingPowerQuarters
                                      : Packer::DEFAULT_GROUPING_POWER_QUARTERS;
                int resolvedSWC =
                    (runParams.skylineWasteCoef >= 1) ? runParams.skylineWasteCoef : Packer::DEFAULT_SKYLINE_WASTE_COEF;
                row.push_back(IntToStr(resolvedGPQ));
                row.push_back(IntToStr(resolvedSWC));

                // Resolved tier weights — sentinel -1 → compiled default; clamp to
                // [0,100] so stamp columns mirror what PackerLAHC's resolver sees.
                int rExact =
                    (runParams.tierWeightExact >= 0) ? runParams.tierWeightExact : Packer::DEFAULT_TIER_WEIGHT_EXACT;
                int rCustom =
                    (runParams.tierWeightCustom >= 0) ? runParams.tierWeightCustom : Packer::DEFAULT_TIER_WEIGHT_CUSTOM;
                int rType =
                    (runParams.tierWeightType >= 0) ? runParams.tierWeightType : Packer::DEFAULT_TIER_WEIGHT_TYPE;
                int rFunction = (runParams.tierWeightFunction >= 0) ? runParams.tierWeightFunction
                                                                    : Packer::DEFAULT_TIER_WEIGHT_FUNCTION;
                int rFlags =
                    (runParams.tierWeightFlags >= 0) ? runParams.tierWeightFlags : Packer::DEFAULT_TIER_WEIGHT_FLAGS;
                int rFsFfr = (runParams.funcSimFoodFoodRestricted >= 0) ? runParams.funcSimFoodFoodRestricted
                                                                        : Packer::DEFAULT_FUNC_SIM_FOOD_FOOD_RESTRICTED;
                int rFsFr = (runParams.funcSimFirstaidRobotrepair >= 0) ? runParams.funcSimFirstaidRobotrepair
                                                                        : Packer::DEFAULT_FUNC_SIM_FIRSTAID_ROBOTREPAIR;
                rExact    = std::min(rExact, 100);
                rCustom   = std::min(rCustom, 100);
                rType     = std::min(rType, 100);
                rFunction = std::min(rFunction, 100);
                rFlags    = std::min(rFlags, 100);
                rFsFfr    = std::min(rFsFfr, 100);
                rFsFr     = std::min(rFsFr, 100);
                int rSoft =
                    (runParams.softGroupingPct >= 0) ? runParams.softGroupingPct : Packer::DEFAULT_SOFT_GROUPING_PCT;
                row.push_back(IntToStr(rExact));
                row.push_back(IntToStr(rCustom));
                row.push_back(IntToStr(rType));
                row.push_back(IntToStr(rFunction));
                row.push_back(IntToStr(rFlags));
                row.push_back(IntToStr(rFsFfr));
                row.push_back(IntToStr(rFsFr));
                row.push_back(IntToStr(rSoft));

                row.push_back(IntToStr(finalDiag.groupingBordersRaw));
                row.push_back(IntToStr(finalDiag.groupingBonusExact));
                row.push_back(IntToStr(firstDiag.skylineSnapHits));
                row.push_back(IntToStr(firstDiag.skylineSnapProbes));
#ifdef STACKSORT_PROFILE
                row.push_back(IntToStr(firstDiag.profCyclesMoveGen));
                row.push_back(IntToStr(firstDiag.profCyclesSkylinePack));
                row.push_back(IntToStr(firstDiag.profCyclesLer));
                row.push_back(IntToStr(firstDiag.profCyclesConcentration));
                row.push_back(IntToStr(firstDiag.profCyclesGrouping));
                row.push_back(IntToStr(firstDiag.profCyclesStranded));
                row.push_back(IntToStr(firstDiag.profCyclesScore));
                row.push_back(IntToStr(firstDiag.profCyclesAccept));
                row.push_back(IntToStr(firstDiag.profCyclesPreReservation));
                row.push_back(IntToStr(firstDiag.profCyclesGreedySeed));
                row.push_back(IntToStr(firstDiag.profCyclesUnconstrainedFallback));
                row.push_back(IntToStr(firstDiag.profCyclesOptimizeGrouping));
                row.push_back(IntToStr(firstDiag.profCyclesStripShift));
                row.push_back(IntToStr(firstDiag.profCyclesTileSwap));
                row.push_back(IntToStr(firstDiag.profCyclesPathRelink));
                row.push_back(IntToStr(firstDiag.profCyclesBordersRaw));
                row.push_back(IntToStr(firstDiag.keptPrefixSum));
                row.push_back(IntToStr(firstDiag.keptPrefixCount));
                row.push_back(IntToStr(firstDiag.gridHashProbes));
                row.push_back(IntToStr(firstDiag.gridHashHits));
                row.push_back(IntToStr(firstDiag.profCyclesSkylinePrefix));
                row.push_back(IntToStr(firstDiag.profCyclesSkylineWasteMap));
                row.push_back(IntToStr(firstDiag.profCyclesSkylineCandidate));
                row.push_back(IntToStr(firstDiag.profCyclesSkylineAdjacency));
                row.push_back(IntToStr(firstDiag.profCyclesSkylineCommit));
                row.push_back(IntToStr(firstDiag.profCyclesLerHistogram));
                row.push_back(IntToStr(firstDiag.profCyclesLerStack));
#endif
                csv.WriteRow(row);

                ++totalRuns;
            }
        }

        (void)fprintf(stdout, "[%s] %dx%d, %d items, density %.2f, targets %d..%d\n", inst.name.c_str(), inst.gridW,
                      inst.gridH, (int)inst.items.size(), density, startT, endT);

        {
            FILE* pf = fopen(progressFile.c_str(), "w");
            if (pf)
            {
                (void)fprintf(pf, "%d/%d %s\n", (int)(ii + 1), (int)instancePaths.size(), inst.name.c_str());
                (void)fclose(pf);
            }
        }
    }

    if (!csv.Close())
    {
        (void)fprintf(stderr, "CSV write error on %s: output may be truncated\n", args.outFile.c_str());
        return 1;
    }
    (void)fprintf(stdout, "Total runs: %d -> %s\n", totalRuns, args.outFile.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception& e)
    {
        (void)fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }
    catch (...)
    {
        (void)fprintf(stderr, "FATAL: unknown exception\n");
        return 1;
    }
}
