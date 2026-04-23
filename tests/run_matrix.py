#!/usr/bin/env python3
import argparse
import concurrent.futures as cf
import ctypes
import glob
import os
import subprocess
import sys
import threading
import time


def _pin_to_core0():
    """Pin this process to core 0 so the orchestration/poller stays off
    benchmark cores (shards pin themselves via STACKSORT_PROFILE_AFFINITY)."""
    try:
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetCurrentProcess()
        kernel32.SetProcessAffinityMask(handle, 1)
    except Exception:
        pass


def _read_progress(path):
    """Return (done, total) from a harness .progress sidecar, or None."""
    try:
        with open(path, "r") as f:
            line = f.readline().strip()
        if not line:
            return None
        frac = line.split()[0]
        done, total = frac.split("/")
        return int(done), int(total)
    except Exception:
        return None


def _progress_poller(tag_dir, stop_event, jobs_done, total_jobs, interval=2.0):
    """Background thread: poll .progress files and redraw a status line."""
    while not stop_event.is_set():
        entries = []
        for p in sorted(glob.glob(os.path.join(tag_dir, "*.progress"))):
            info = _read_progress(p)
            name = os.path.basename(p).replace(".csv.progress", "")
            if info:
                entries.append((name, info[0], info[1]))
            else:
                entries.append((name, 0, 0))

        parts = []
        for name, done, total in entries:
            if total > 0:
                parts.append("{} {}/{}".format(name, done, total))
        shard_str = "  ".join(parts) if parts else "waiting..."

        bar_total = sum(e[2] for e in entries)
        bar_done = sum(e[1] for e in entries)
        if bar_total > 0:
            pct = 100.0 * bar_done / bar_total
            w = 30
            filled = int(pct / 100.0 * w)
            bar = "#" * filled + "-" * (w - filled)
            line = "\r  [{}] {}/{} instances ({:.0f}%)  jobs {}/{}  {}".format(
                bar, bar_done, bar_total, pct, jobs_done[0], total_jobs,
                shard_str)
        else:
            line = "\r  jobs {}/{}  {}".format(
                jobs_done[0], total_jobs, shard_str)

        sys.stdout.write(line + "\033[K")
        sys.stdout.flush()
        stop_event.wait(interval)


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CORPUS  = os.path.join(SCRIPT_DIR, "corpus")
DEFAULT_CONFIGS = os.path.join(SCRIPT_DIR, "configs")
DEFAULT_OUT     = os.path.join(SCRIPT_DIR, "results")
DEFAULT_BIN     = os.path.join(SCRIPT_DIR, "harness", "bin", "stacksort_bench.exe")


def run_one(bench_bin, corpus_dir, config_path, rotate, base_seed, seeds,
            refine, out_path, timeout, affinity_mask=None, high_priority=False):
    """Returns (name, rotate, rc, elapsed, stderr).

    affinity_mask: optional DWORD_PTR mask passed as STACKSORT_PROFILE_AFFINITY
    so parallel shards can pin to distinct cores. Honored by both profile and
    non-profile harness builds (default mask = 1 in either). Env var name
    kept for backward compat.

    high_priority: when True, spawn the harness at HIGH_PRIORITY_CLASS to
    reduce scheduler-preemption noise in profiling runs."""
    name = os.path.splitext(os.path.basename(config_path))[0]
    env = None
    if affinity_mask is not None:
        env = os.environ.copy()
        env["STACKSORT_PROFILE_AFFINITY"] = hex(affinity_mask)
    flags = 0
    if high_priority:
        flags = subprocess.HIGH_PRIORITY_CLASS
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
            env=env,
            creationflags=flags,
        )
        rc = result.returncode
        stderr = result.stderr
    except subprocess.TimeoutExpired:
        rc = -1
        stderr = "TIMEOUT"
    elapsed = time.time() - t0
    return (name, rotate, rc, elapsed, stderr)


def merge_shard_outputs(shards_by_cfgrot, tag_dir):
    """Concatenate per-shard CSVs into a single <name>_rot<N>.csv per
    (config, rotation). Leaves non-sharded outputs alone. Removes shard
    files on success. Row order within a config/rotation is shard-index
    ascending; rows within each shard are already seed-ordered by the
    harness.
    """
    import csv
    merged_count = 0
    for (name, rot), paths in shards_by_cfgrot.items():
        if len(paths) <= 1:
            continue
        merged_path = os.path.join(tag_dir, "{}_rot{}.csv".format(name, rot))
        first_header = None
        with open(merged_path, "w", newline="", encoding="utf-8") as out_f:
            writer = csv.writer(out_f)
            for path in paths:
                with open(path, "r", newline="", encoding="utf-8") as src_f:
                    reader = csv.reader(src_f)
                    try:
                        header = next(reader)
                    except StopIteration:
                        continue
                    if first_header is None:
                        first_header = header
                        writer.writerow(header)
                    elif header != first_header:
                        raise RuntimeError(
                            "shard header mismatch: {} vs {}".format(
                                paths[0], path))
                    for row in reader:
                        writer.writerow(row)
        for path in paths:
            if os.path.abspath(path) != os.path.abspath(merged_path):
                try:
                    os.remove(path)
                except OSError:
                    pass
        merged_count += 1
    return merged_count


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
    ap.add_argument("--seed-shards", type=int, default=0,
                    help="Split each (config x rotation) job into N shards by "
                         "seed range. Each shard runs disjoint seeds [base..base+n) "
                         "and writes a separate CSV (e.g. <tag>/config_rot0_s0.csv). "
                         "analyze.py reads them all via glob. Multiplies the "
                         "parallelism for slow configs (e.g. high restart counts). "
                         "Requires the harness to emit absolute seed in the seed "
                         "column so shards don't collide on (config,inst,target,seed) "
                         "groupby keys. Default 0 = match --workers so a single "
                         "config x rotation fills all worker threads.")
    ap.add_argument("--timeout", type=int, default=3600,
                    help="Per-subprocess timeout in seconds (default 3600). "
                         "Bump for very slow ablations (high restarts, low plateau).")
    ap.add_argument("--corpus", default=DEFAULT_CORPUS)
    ap.add_argument("--configs", default=DEFAULT_CONFIGS)
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--bench", default=DEFAULT_BIN)
    ap.add_argument("--tag", default="run",
                    help="Subdirectory under --out-dir that holds this run's "
                         "output CSVs (default 'run'). CSV filenames are "
                         "'<config>_rot<N>[_s<N>].csv' — the tag lives in the "
                         "path, not the filename.")
    ap.add_argument("--rotations", default="0,1",
                    help="Comma-separated rotation modes (default '0,1')")
    ap.add_argument("--refine", type=int, default=1,
                    help="Run production two-pass first+refinement (default 1). "
                         "Pass --refine 0 for first-pass-only data.")
    ap.add_argument("--only", default="",
                    help="Comma-separated config name allowlist (e.g. "
                         "'grouping_power_5,restarts_16'). Matches the file "
                         "stem. Empty = run all configs in --configs dir.")
    ap.add_argument("--pin-shards", action="store_true",
                    help="Pin each parallel shard to its own CPU core via "
                         "STACKSORT_PROFILE_AFFINITY. Keeps TSC clean under "
                         "parallel profile runs and reduces scheduler-migration "
                         "noise in non-profile runs. The env-var mask is "
                         "honored by both harness builds (default mask = 1 "
                         "when unset). Core selection follows --pin-cores if "
                         "set, else contiguous from --pin-base-core.")
    ap.add_argument("--pin-base-core", type=int, default=1,
                    help="First core used when --pin-shards is set (contiguous "
                         "allocation). Default 1 leaves core 0 for the "
                         "orchestrator/poller. Ignored if --pin-cores is set.")
    ap.add_argument("--pin-cores", default="",
                    help="Comma-separated explicit core list for --pin-shards, "
                         "e.g. '8,12' pins shard 0 to core 8 and shard 1 to "
                         "core 12 (useful for skipping SMT siblings on topo "
                         "where logical N and N+1 share a physical core). "
                         "Cycles through the list when there are more shards "
                         "than cores. Overrides --pin-base-core.")
    ap.add_argument("--high-priority", action="store_true",
                    help="Spawn each harness subprocess at HIGH_PRIORITY_CLASS "
                         "(Windows). Reduces scheduler-preemption noise in "
                         "profiling runs. No effect on non-Windows platforms.")
    ap.add_argument("--keep-shards", action="store_true",
                    help="When sharding is active, keep the per-shard CSVs "
                         "alongside the merged output. Default is to merge "
                         "into <name>_rot<N>.csv and delete the shard files.")
    args = ap.parse_args()

    if args.seed_shards <= 0:
        args.seed_shards = max(1, min(args.workers, args.seeds))

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

    tag_dir = os.path.join(args.out_dir, args.tag)
    os.makedirs(tag_dir, exist_ok=True)

    jobs = []
    shards_by_cfgrot = {}
    for rot in rotations:
        for cfg in config_files:
            name = os.path.splitext(os.path.basename(cfg))[0]
            shards = list(shard_seed_ranges(args.seeds, args.seed_shards))
            for s_idx, (base_seed, n_seeds) in enumerate(shards):
                if args.seed_shards > 1:
                    out_path = os.path.join(
                        tag_dir,
                        "{}_rot{}_s{}.csv".format(name, rot, s_idx))
                else:
                    out_path = os.path.join(
                        tag_dir,
                        "{}_rot{}.csv".format(name, rot))
                jobs.append((cfg, rot, base_seed, n_seeds, out_path))
                shards_by_cfgrot.setdefault((name, rot), []).append(out_path)

    # Optional per-shard CPU pinning. If --pin-cores is given, cycle through
    # that explicit list (useful for skipping SMT siblings). Otherwise map
    # each job to a distinct core contiguously from pin_base_core. If there
    # are more jobs than cores, masks wrap within the range.
    cpu_count = os.cpu_count() or 8
    job_affinities = [None] * len(jobs)
    if args.pin_shards:
        if args.pin_cores:
            explicit = [int(c.strip()) for c in args.pin_cores.split(",") if c.strip()]
            if not explicit:
                print("ERROR: --pin-cores is empty", file=sys.stderr)
                return 1
            for i in range(len(jobs)):
                job_affinities[i] = 1 << explicit[i % len(explicit)]
        else:
            core_range = max(1, cpu_count - args.pin_base_core)
            for i in range(len(jobs)):
                job_affinities[i] = 1 << (args.pin_base_core + (i % core_range))

    shard_note = " x {} shards".format(args.seed_shards) if args.seed_shards > 1 else ""
    print("Running {} jobs ({} configs x {} rotations{}), {} seeds, refine={}, {} workers, timeout={}s".format(
        len(jobs), len(config_files), len(rotations), shard_note,
        args.seeds, args.refine, args.workers, args.timeout))
    print("Output directory: {}".format(tag_dir))
    print("")

    if args.pin_shards:
        _pin_to_core0()

    t0 = time.time()
    failures = []
    done = 0
    jobs_done = [0]
    stop_event = threading.Event()
    poller = threading.Thread(target=_progress_poller,
                              args=(tag_dir, stop_event, jobs_done, len(jobs)),
                              daemon=True)
    poller.start()

    with cf.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futures = {
            ex.submit(run_one, args.bench, args.corpus, cfg, rot,
                      base_seed, n_seeds, args.refine, out_path, args.timeout,
                      job_affinities[i], args.high_priority): (cfg, rot)
            for i, (cfg, rot, base_seed, n_seeds, out_path) in enumerate(jobs)
        }
        for fut in cf.as_completed(futures):
            name, rot, rc, elapsed, stderr = fut.result()
            done += 1
            jobs_done[0] = done
            if rc != 0:
                failures.append((name, rot, rc, stderr))

    stop_event.set()
    poller.join(timeout=3)
    sys.stdout.write("\r\033[K")
    sys.stdout.flush()

    total = time.time() - t0
    print("Total elapsed: {:.1f}s  ({} jobs, {} failures)".format(
        total, len(jobs), len(failures)))

    for p in glob.glob(os.path.join(tag_dir, "*.progress")):
        try:
            os.remove(p)
        except OSError:
            pass

    if failures:
        print("")
        print("FAILURES:")
        for name, rot, rc, stderr in failures:
            print("  {} rot={} rc={}".format(name, rot, rc))
            if stderr:
                for line in stderr.strip().split("\n")[:5]:
                    print("    {}".format(line))
        return 1

    if args.seed_shards > 1 and not args.keep_shards:
        merged = merge_shard_outputs(shards_by_cfgrot, tag_dir)
        if merged > 0:
            print("Merged {} sharded CSV{} (pass --keep-shards to retain).".format(
                merged, "s" if merged != 1 else ""))

    return 0


if __name__ == "__main__":
    sys.exit(main())
