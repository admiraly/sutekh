# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Runs the 6dof_racer_microcosm driver (bench/space3d/racer_microcosm.c).

    run_bench.py --driver <racer_microcosm> --check
        Small deterministic run (N = 8, 720 ticks) with replay from start and from a checkpoint; the final state
        and event-log digests must equal bench/space3d/expected.json. Exit 0 only when every check ran and passed.
    run_bench.py --driver <racer_microcosm> --evidence <dir> [--ticks 1200] [--repeat 3]
        N = 8, 32, 256 with the query, and without it (IR systems only), each with replay checks, each repeated in
        separate processes. Writes <dir>/racer-bench.json (machine, toolchain, source and driver identities, every
        driver record) and prints a summary table with the spread over repeats. Timings are observations on this
        machine (constitution C10), not qualification.
    run_bench.py --driver <racer_microcosm> --write-expected
        Records the --check digests in bench/space3d/expected.json (only after a reviewed semantic change).
"""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXPECTED = os.path.join(ROOT, "bench", "space3d", "expected.json")
CHECK_ARGS = ["--n", "8", "--ticks", "720", "--warmup", "0", "--digest-every", "120", "--replay"]


def run(driver, extra):
    out = subprocess.run([driver, "--root", ROOT] + extra, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        raise SystemExit("driver failed (%d): %s" % (out.returncode, " ".join(extra)))
    return json.loads(out.stdout)


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


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def git_head():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def git_dirty():
    try:
        out = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"], capture_output=True,
                             text=True, check=True).stdout
        return bool(out.strip())
    except (OSError, subprocess.CalledProcessError):
        return None


def check(driver):
    rec = run(driver, CHECK_ARGS)
    exp = json.load(open(EXPECTED, encoding="utf-8"))["n8_t720"]
    failures = []
    if rec["replay_from_start_identical"] is not True:
        failures.append("replay from start differs")
    if rec["replay_from_checkpoint_identical"] is not True:
        failures.append("replay from checkpoint differs")
    for key in ("final_state_sha256", "event_log_sha256", "events", "final_tick"):
        if rec[key] != exp[key]:
            failures.append("%s: got %s, expected %s" % (key, rec[key], exp[key]))
    if rec["race"]["gates_passed"] == 0:
        failures.append("no gate was passed: the scene does not exercise the gate query")
    for f in failures:
        print("FAIL", f)
    print("racer check: final %s, events %d (%s), gates %d, collision ticks %d, replay %s/%s, %d failures"
          % (rec["final_state_sha256"][:16], rec["events"], rec["event_log_sha256"][:16], rec["race"]["gates_passed"],
             rec["race"]["collision_ticks"], rec["replay_from_start_identical"], rec["replay_from_checkpoint_identical"],
             len(failures)))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", required=True)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--write-expected", action="store_true")
    ap.add_argument("--evidence")
    ap.add_argument("--ticks", type=int, default=1200)
    ap.add_argument("--repeat", type=int, default=3)
    args = ap.parse_args()
    if args.check:
        return check(args.driver)
    if args.write_expected:
        rec = run(args.driver, CHECK_ARGS)
        exp = {"n8_t720": {k: rec[k] for k in ("final_state_sha256", "event_log_sha256", "events", "final_tick")},
               "args": CHECK_ARGS, "note": "bit-exact on every conforming compiler and target (integer-only)"}
        with open(EXPECTED, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(exp, indent=1) + "\n")
        print(json.dumps(exp, indent=1))
        return 0
    if not args.evidence:
        ap.error("one of --check, --evidence, --write-expected")
    os.makedirs(args.evidence, exist_ok=True)
    records = []
    for rep_i in range(args.repeat):
        for n in (8, 32, 256):
            for query in (True, False):
                extra = ["--n", str(n), "--ticks", str(args.ticks), "--warmup", "120", "--replay"]
                if not query:
                    extra.append("--no-query")
                rec = run(args.driver, extra)
                rec["args"] = extra
                rec["repeat"] = rep_i
                records.append(rec)
    doc = {
        "format": "sutekh.bench.6dof_racer_microcosm/1",
        "statement": "observation on one machine; not performance qualification (ENGINE_CONSTITUTION C10)",
        "machine": {"cpu": cpu_name(), "logical_cpus": os.cpu_count(), "os": platform.platform(),
                    "python": platform.python_version()},
        "source": {"git_head": git_head(), "worktree_dirty": git_dirty()},
        "driver": {"path": os.path.relpath(args.driver, ROOT) if args.driver.startswith(ROOT) else args.driver,
                   "sha256": sha256_file(args.driver)},
        "measurement": "wall time of su_sched_run_ticks(1 tick) per tick, monotonic clock, single thread, "
                       "after warm-up; one process per configuration and repeat; repeats interleaved",
        "repeats": args.repeat,
        "records": records,
    }
    out = os.path.join(args.evidence, "racer-bench.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(doc, indent=1) + "\n")
    print("| N | query | rows | p50 us (min..max of %d runs) | p99 us | max us | worst p99 / 8.33 ms | gates | "
          "collision ticks | events | digests equal across runs | replay start/ckpt |" % args.repeat)
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for n in (8, 32, 256):
        for query in (True, False):
            rs = [r for r in records if r["n"] == n and r["query"] == query]
            p50 = [r["tick_ns"]["p50"] / 1e3 for r in rs]
            p99 = [r["tick_ns"]["p99"] / 1e3 for r in rs]
            mx = [r["tick_ns"]["max"] / 1e3 for r in rs]
            same = len({(r["final_state_sha256"], r["event_log_sha256"]) for r in rs}) == 1
            r0 = rs[0]
            print("| %d | %s | %d | %.1f..%.1f | %.1f..%.1f | %.1f..%.1f | %.2f%% | %d | %d | %d | %s | %s/%s |" % (
                n, "yes" if query else "no", r0["rows"], min(p50), max(p50), min(p99), max(p99), min(mx), max(mx),
                100.0 * max(r["p99_fraction_of_budget"] for r in rs), r0["race"]["gates_passed"],
                r0["race"]["collision_ticks"], r0["events"], same,
                all(r["replay_from_start_identical"] is True for r in rs),
                all(r["replay_from_checkpoint_identical"] is True for r in rs)))
    print("wrote", out)
    bad = [r for r in records if r["replay_from_start_identical"] is not True
           or r["replay_from_checkpoint_identical"] is not True]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
