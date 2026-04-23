#!/usr/bin/env python3
"""Per-shard-isolated PGO training dispatcher.

MSVC 2010 PGO races on the pgd counter when multiple instrumented processes
share one pgd -- pgc files silently overwrite each other. This script avoids
the race by copying the exe + pgd into per-shard dirs and running each shard
with cwd = its shard dir. The MSVC runtime writes pgc next to the exe, so
per-shard exe paths = per-shard pgc streams. All shards' pgc files share the
same pgd instance ID (they come from copies of one INSTRUMENT link), so the
final pgomgr /merge reconciles them into the source pgd.

After each shard completes, its pgc files are renamed into the pgd's dir with
unique counters (shard_idx * 1000 + original) so pgomgr's sibling-discovery
finds them all without collision.
"""
import argparse
import concurrent.futures as cf
import ctypes
import glob
import os
import shutil
import subprocess
import sys
import time


def shard_seed_ranges(total_seeds, num_shards):
    base = 1
    chunk = total_seeds // num_shards
    extra = total_seeds % num_shards
    for s in range(num_shards):
        k = chunk + (1 if s < extra else 0)
        if k <= 0:
            continue
        yield base, k
        base += k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True,
                    help="Path to instrumented exe (source; copied per shard).")
    ap.add_argument("--dll", default="",
                    help="Path to instrumented DLL (copied alongside exe per "
                         "shard). Use for DLL-targeted PGO where the packer "
                         "lives in a DLL loaded by the host exe.")
    ap.add_argument("--pgd", required=True,
                    help="Path to pgd (source; copied per shard).")
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--seeds", type=int, required=True,
                    help="Total seeds per instance (split across shards).")
    ap.add_argument("--shards-per-rotation", type=int, default=1,
                    help="Parallel shards per rotation. Total processes = "
                         "len(--rotations) * shards-per-rotation.")
    ap.add_argument("--rotations", default="0,1")
    ap.add_argument("--pin-cores", default="",
                    help="Comma-separated explicit cores for shards, "
                         "cycling through list (e.g. '1,3,5,7,9,11,13,15').")
    ap.add_argument("--shard-root", default="bin",
                    help="Parent dir for per-shard subdirs (default bin).")
    ap.add_argument("--high-priority", action="store_true",
                    help="Spawn shards at HIGH_PRIORITY_CLASS (Windows).")
    ap.add_argument("--timeout", type=int, default=3600)
    args = ap.parse_args()

    bench_abs = os.path.abspath(args.bench)
    dll_abs = os.path.abspath(args.dll) if args.dll else ""
    pgd_abs = os.path.abspath(args.pgd)
    corpus_abs = os.path.abspath(args.corpus)
    config_abs = os.path.abspath(args.config)

    checks = [("bench", bench_abs), ("pgd", pgd_abs),
              ("corpus", corpus_abs), ("config", config_abs)]
    if dll_abs:
        checks.append(("dll", dll_abs))
    for label, path in checks:
        if not os.path.exists(path):
            print("ERROR: {} not found: {}".format(label, path), file=sys.stderr)
            return 1

    shards = list(shard_seed_ranges(args.seeds, args.shards_per_rotation))
    rotations = [int(r.strip()) for r in args.rotations.split(",") if r.strip()]
    pin_list = [int(c.strip()) for c in args.pin_cores.split(",") if c.strip()]

    jobs = []
    for rot in rotations:
        for s_idx, (base_seed, n_seeds) in enumerate(shards):
            jobs.append((rot, s_idx, base_seed, n_seeds))

    bench_basename = os.path.basename(bench_abs)
    dll_basename = os.path.basename(dll_abs) if dll_abs else ""
    pgd_basename = os.path.basename(pgd_abs)
    # pgc files are named <pgd-stem>!<counter>.pgc by the MSVC runtime, so
    # profile-variant pgds produce profile-variant pgcs. Derive the prefix.
    pgc_prefix = os.path.splitext(pgd_basename)[0]
    shard_root_abs = os.path.abspath(args.shard_root)

    # Pin orchestrator to core 0 so its cycles don't nibble into shard cores.
    try:
        ctypes.windll.kernel32.SetProcessAffinityMask(
            ctypes.windll.kernel32.GetCurrentProcess(), 1)
    except Exception:
        pass

    # Wipe stale shard dirs so we only merge this run's pgc data.
    for p in glob.glob(os.path.join(shard_root_abs, "shard_rot*_s*")):
        if os.path.isdir(p):
            shutil.rmtree(p)

    def prep_shard(rot, s_idx):
        d = os.path.join(shard_root_abs, "shard_rot{}_s{}".format(rot, s_idx))
        os.makedirs(os.path.join(d, "pgo"), exist_ok=True)
        shutil.copy2(bench_abs, os.path.join(d, bench_basename))
        if dll_abs:
            shutil.copy2(dll_abs, os.path.join(d, dll_basename))
        # pgd lives at <shard>/pgo/<basename> to match the relative path baked
        # into the exe/dll at link time (/PGD:pgo\<name>.pgd).
        shutil.copy2(pgd_abs, os.path.join(d, "pgo", pgd_basename))
        return d

    def run_shard(job_idx, rot, s_idx, base_seed, n_seeds):
        shard_dir = prep_shard(rot, s_idx)
        exe_path = os.path.join(shard_dir, bench_basename)

        env = os.environ.copy()
        if pin_list:
            core = pin_list[job_idx % len(pin_list)]
            env["STACKSORT_PROFILE_AFFINITY"] = hex(1 << core)

        cmd = [exe_path,
               "--corpus", corpus_abs,
               "--config", config_abs,
               "--base-seed", str(base_seed),
               "--seeds", str(n_seeds),
               "--rotate", str(rot),
               "--refine", "1",
               "--out", os.path.join(shard_dir, "train.csv")]
        t0 = time.time()
        flags = subprocess.HIGH_PRIORITY_CLASS if args.high_priority else 0
        try:
            result = subprocess.run(
                cmd, cwd=shard_dir, env=env,
                capture_output=True, text=True,
                timeout=args.timeout,
                creationflags=flags)
            return (job_idx, rot, s_idx, result.returncode,
                    time.time() - t0,
                    (result.stderr or "")[-400:], shard_dir)
        except subprocess.TimeoutExpired:
            return (job_idx, rot, s_idx, -1, time.time() - t0,
                    "TIMEOUT", shard_dir)

    print("Dispatching {} shards (seeds={}, shards/rot={}) pinned to cores {}".format(
        len(jobs), args.seeds, args.shards_per_rotation,
        pin_list if pin_list else "[unpinned]"))

    t_start = time.time()
    failures = []
    with cf.ThreadPoolExecutor(max_workers=len(jobs)) as ex:
        futures = [ex.submit(run_shard, i, rot, s_idx, bs, ns)
                   for i, (rot, s_idx, bs, ns) in enumerate(jobs)]
        for fut in cf.as_completed(futures):
            job_idx, rot, s_idx, rc, elapsed, stderr, shard_dir = fut.result()
            status = "OK" if rc == 0 else "FAIL rc={}".format(rc)
            print("  shard rot{} s{}: {} in {:.1f}s".format(
                rot, s_idx, status, elapsed))
            if rc != 0:
                failures.append((rot, s_idx, rc, stderr))

    total = time.time() - t_start
    print("Total: {:.1f}s ({} shards, {} failures)".format(
        total, len(jobs), len(failures)))

    if failures:
        for rot, s_idx, rc, err in failures:
            print("  rot{} s{} rc={}: {}".format(rot, s_idx, rc, err))
        return 1

    # Collect each shard's pgc files into the pgd's dir. Each shard's counter
    # starts at 1 in its own pgd copy, so we'd get collisions on simple move.
    # Renumber as (job_idx * 1000 + original_counter) to keep all unique.
    pgd_dir = os.path.dirname(pgd_abs)
    collected = 0
    pgc_glob = pgc_prefix + "!*.pgc"
    for job_idx, (rot, s_idx, _, _) in enumerate(jobs):
        shard_dir = os.path.join(shard_root_abs,
                                 "shard_rot{}_s{}".format(rot, s_idx))
        for pgc in sorted(glob.glob(os.path.join(shard_dir, pgc_glob))):
            base_counter = int(os.path.basename(pgc).split("!")[1].split(".")[0])
            new_counter = job_idx * 1000 + base_counter
            dest = os.path.join(pgd_dir,
                                "{}!{}.pgc".format(pgc_prefix, new_counter))
            shutil.move(pgc, dest)
            collected += 1

    print("Collected {} .pgc files into {}".format(collected, pgd_dir))
    if collected == 0:
        print("ERROR: no .pgc files produced by instrumented runtime.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
