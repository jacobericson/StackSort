#!/usr/bin/env python3
import argparse
import concurrent.futures as cf
import glob
import os
import subprocess
import sys
import time


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CORPUS  = os.path.join(SCRIPT_DIR, "corpus")
DEFAULT_CONFIGS = os.path.join(SCRIPT_DIR, "configs")
DEFAULT_OUT     = os.path.join(SCRIPT_DIR, "results")
DEFAULT_BIN     = os.path.join(SCRIPT_DIR, "harness", "bin", "stacksort_bench.exe")


def run_one(bench_bin, corpus_dir, config_path, rotate, base_seed, seeds,
            refine, out_path, timeout):
    """Returns (name, rotate, rc, elapsed, stderr)."""
    name = os.path.splitext(os.path.basename(config_path))[0]
    t0 = time.time()
    try:
        result = subprocess.run(
            [bench_bin,
             "--corpus", corpus_dir,
             "--config", config_path,
             "--base-seed", str(base_seed),
             "--seeds", str(seeds),
             "--rotate", str(rotate),
             "--refine", str(refine),
             "--out", out_path],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        rc = result.returncode
        stderr = result.stderr
    except subprocess.TimeoutExpired:
        rc = -1
        stderr = "TIMEOUT"
    elapsed = time.time() - t0
    return (name, rotate, rc, elapsed, stderr)


def shard_seed_ranges(total_seeds, num_shards):
    """Default base of 1 keeps continuity with pre-sharding runs
    that used --base-seed 1."""
    if num_shards <= 1:
        yield 1, total_seeds
        return
    base = 1
    extra = total_seeds % num_shards
    base_chunk = total_seeds // num_shards
    for s in range(num_shards):
        n = base_chunk + (1 if s < extra else 0)
        if n <= 0:
            continue
        yield base, n
        base += n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=3,
                    help="Seeds per instance (default 3)")
    default_workers = max(1, os.cpu_count() or 8)
    ap.add_argument("--workers", type=int, default=default_workers,
                    help="Parallel workers (default: CPU count). "
                         "wall_clock_ms is slightly noisier under full load; "
                         "drop to CPU count - 4 if comparing timings run-to-run.")
    ap.add_argument("--seed-shards", type=int, default=1,
                    help="Split each (config x rotation) job into N shards by "
                         "seed range. Each shard runs disjoint seeds [base..base+n) "
                         "and writes a separate CSV (e.g. tag_config_rot0_s0.csv). "
                         "analyze.py reads them all via glob. Multiplies the "
                         "parallelism for slow configs (e.g. high restart counts). "
                         "Requires the harness to emit absolute seed in the seed "
                         "column so shards don't collide on (config,inst,target,seed) "
                         "groupby keys.")
    ap.add_argument("--timeout", type=int, default=3600,
                    help="Per-subprocess timeout in seconds (default 3600). "
                         "Bump for very slow ablations (high restarts, low plateau).")
    ap.add_argument("--corpus", default=DEFAULT_CORPUS)
    ap.add_argument("--configs", default=DEFAULT_CONFIGS)
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--bench", default=DEFAULT_BIN)
    ap.add_argument("--tag", default="run",
                    help="Prefix for output CSV filenames (default 'run')")
    ap.add_argument("--rotations", default="0,1",
                    help="Comma-separated rotation modes (default '0,1')")
    ap.add_argument("--refine", type=int, default=1,
                    help="Run production two-pass first+refinement (default 1). "
                         "Pass --refine 0 for first-pass-only data.")
    ap.add_argument("--only", default="",
                    help="Comma-separated config name allowlist (e.g. "
                         "'grouping_power_5,restarts_16'). Matches the file "
                         "stem. Empty = run all configs in --configs dir.")
    args = ap.parse_args()

    if not os.path.exists(args.bench):
        print("ERROR: harness binary not found: {}".format(args.bench),
              file=sys.stderr)
        print("Build it with tests/harness/build.bat", file=sys.stderr)
        return 1

    config_files = sorted(glob.glob(os.path.join(args.configs, "*.ini")))
    if not config_files:
        print("ERROR: no .ini configs in {}".format(args.configs), file=sys.stderr)
        return 1

    if args.only:
        allow = set(s.strip() for s in args.only.split(",") if s.strip())
        config_files = [c for c in config_files
                        if os.path.splitext(os.path.basename(c))[0] in allow]
        if not config_files:
            print("ERROR: --only filter '{}' matched no configs".format(args.only),
                  file=sys.stderr)
            return 1

    rotations = [int(r.strip()) for r in args.rotations.split(",") if r.strip()]

    os.makedirs(args.out_dir, exist_ok=True)

    jobs = []
    for rot in rotations:
        for cfg in config_files:
            name = os.path.splitext(os.path.basename(cfg))[0]
            shards = list(shard_seed_ranges(args.seeds, args.seed_shards))
            for s_idx, (base_seed, n_seeds) in enumerate(shards):
                if args.seed_shards > 1:
                    out_path = os.path.join(
                        args.out_dir,
                        "{}_{}_rot{}_s{}.csv".format(args.tag, name, rot, s_idx))
                else:
                    out_path = os.path.join(
                        args.out_dir,
                        "{}_{}_rot{}.csv".format(args.tag, name, rot))
                jobs.append((cfg, rot, base_seed, n_seeds, out_path))

    shard_note = " x {} shards".format(args.seed_shards) if args.seed_shards > 1 else ""
    print("Running {} jobs ({} configs x {} rotations{}), {} seeds, refine={}, {} workers, timeout={}s".format(
        len(jobs), len(config_files), len(rotations), shard_note,
        args.seeds, args.refine, args.workers, args.timeout))
    print("Output directory: {}".format(args.out_dir))
    print("")

    t0 = time.time()
    failures = []
    done = 0
    with cf.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futures = {
            ex.submit(run_one, args.bench, args.corpus, cfg, rot,
                      base_seed, n_seeds, args.refine, out_path, args.timeout): (cfg, rot)
            for (cfg, rot, base_seed, n_seeds, out_path) in jobs
        }
        for fut in cf.as_completed(futures):
            name, rot, rc, elapsed, stderr = fut.result()
            done += 1
            status = "ok" if rc == 0 else "FAIL (rc={})".format(rc)
            print("  [{}/{}] {:30s} rot={} {:.1f}s  {}".format(
                done, len(jobs), name, rot, elapsed, status))
            if rc != 0:
                failures.append((name, rot, rc, stderr))

    total = time.time() - t0
    print("")
    print("Total elapsed: {:.1f}s".format(total))

    if failures:
        print("")
        print("FAILURES:")
        for name, rot, rc, stderr in failures:
            print("  {} rot={} rc={}".format(name, rot, rc))
            if stderr:
                for line in stderr.strip().split("\n")[:5]:
                    print("    {}".format(line))
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
