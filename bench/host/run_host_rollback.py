# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Rollback through the public host API (bench/host/bench_host_rollback.c) on the 6dof_racer_microcosm.

    run_host_rollback.py --bench <bench_host_rollback> --racer <racer_microcosm> --work <dir>
                         [--n 8,16,32,256] [--tiers planned,native] [--k 6,12] [--repeat 3] [--iters 200]
                         [--evidence <dir>]

For every N the racer driver dumps the scene fixture (--dump-fixture); then, per repeat (interleaved), per N and
tier, one bench process restores in place, re-steps K ticks in one su_host_step call and takes the fast digest,
--iters times per K. Exit 0 only when every iteration matched its reference digest and every tier and repeat
produced the same snapshot SHA-256 per (N, K). With --evidence, writes <dir>/host-rollback-bench.json and prints a
markdown table. Timing figures are observations on the machine that ran them, not qualification (C10).
"""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def run(cmd):
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stdout + out.stderr)
        raise SystemExit("failed (%d): %s" % (out.returncode, " ".join(cmd)))
    return out.stdout


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def cpu_name():
    if sys.platform == "win32":
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
        except OSError:
            return platform.processor()
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def git(*args):
    try:
        return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--racer", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--n", default="8,16,32,256")
    ap.add_argument("--tiers", default="planned,native")
    ap.add_argument("--k", default="6,12")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--iters", type=int, default=200)
    ap.add_argument("--evidence")
    args = ap.parse_args()
    ns = [int(x) for x in args.n.split(",")]
    tiers = args.tiers.split(",")
    os.makedirs(args.work, exist_ok=True)
    fixtures = {}
    for n in ns:
        fixtures[n] = os.path.join(args.work, "racer_n%d.json" % n)
        run([args.racer, "--root", ROOT, "--n", str(n), "--ticks", "1", "--warmup", "0", "--dump-fixture",
             fixtures[n]])
    records = []
    for rep in range(args.repeat):
        for n in ns:
            for tier in tiers:
                cmd = [args.bench, "--root", ROOT, "--fixture", fixtures[n], "--n", str(n), "--tier", tier, "--k",
                       args.k, "--iters", str(args.iters)]
                rec = json.loads(run(cmd))
                rec["repeat"] = rep
                records.append(rec)
    failures = []
    for r in records:
        if r["digests_identical"] is not True:
            failures.append("N=%d %s repeat %d: an iteration's digest differed" % (r["n"], r["tier"], r["repeat"]))
    for n in ns:
        sigs = {tuple(k["snapshot_sha256"] for k in r["k"]) for r in records if r["n"] == n}
        if len(sigs) != 1:
            failures.append("N=%d: tiers or repeats disagree on the re-simulated snapshot digests" % n)
    ks = [int(x) for x in args.k.split(",")]
    lines = ["| N | rows | tier | K | restore p50 us | resim p50 us (min..max of %d runs) | per tick p50 us | "
             "digest_fast p50 us | total p50 / 8.33 ms | total p99 / 8.33 ms |" % args.repeat,
             "|---|---|---|---|---|---|---|---|---|---|"]
    for n in ns:
        for tier in tiers:
            rs = [r for r in records if r["n"] == n and r["tier"] == tier]
            for ki, k in enumerate(ks):
                kk = [r["k"][ki] for r in rs]
                resim = [x["resim_ns"]["p50"] / 1e3 for x in kk]
                lines.append("| %d | %d | %s | %d | %.1f | %.1f..%.1f | %.1f | %.1f | %.2f%% | %.2f%% |" % (
                    n, rs[0]["rows"], tier, k, max(x["restore_ns"]["p50"] for x in kk) / 1e3, min(resim), max(resim),
                    max(x["resim_per_tick_p50_ns"] for x in kk) / 1e3,
                    max(x["digest_fast_ns"]["p50"] for x in kk) / 1e3,
                    100.0 * max(x["total_p50_fraction_of_budget"] for x in kk),
                    100.0 * max(x["total_p99_fraction_of_budget"] for x in kk)))
    print("\n".join(lines))
    if args.evidence:
        os.makedirs(args.evidence, exist_ok=True)
        doc = {
            "format": "sutekh.bench.host_rollback/1",
            "statement": "observation on one machine; not performance qualification (ENGINE_CONSTITUTION C10)",
            "machine": {"cpu": cpu_name(), "logical_cpus": os.cpu_count(), "os": platform.platform(),
                        "python": platform.python_version()},
            "source": {"git_head": git("rev-parse", "HEAD"),
                       "worktree_dirty": bool(git("status", "--porcelain", "--untracked-files=no"))},
            "bench": {"path": os.path.basename(args.bench), "sha256": sha256_file(args.bench)},
            "measurement": "per K: restore = su_host_snapshot_restore_inplace of the anchor snapshot; resim = one "
                           "su_host_step(K) call; digest_fast = su_host_digest_fast; total = the three; after a "
                           "120-tick warm-up; monotonic clock, single thread; one process per (repeat, N, tier), "
                           "repeats interleaved; tier native = racer.move as a native kernel, racer.race on the "
                           "planned VM (it emits events), space3d.query a host operator",
            "repeats": args.repeat,
            "iterations": args.iters,
            "records": records,
            "table": lines,
            "failures": failures,
        }
        out = os.path.join(args.evidence, "host-rollback-bench.json")
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(doc, indent=1) + "\n")
        print("wrote", out)
    for f in failures:
        print("FAIL", f)
    print("host rollback: %d records, %d failures" % (len(records), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
