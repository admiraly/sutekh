# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Replay validator code mutants (contract M3 section 15, M3-M18: replay skips a digest record or ignores seq order).

Each mutant edits bench/rts/replay/rts_replay.py in a private copy of bench/rts and tests/rts/replay, then runs the
model-backend replay tests (test_replay.py). killed = a test failed; survived = all passed (an evidence gap);
not_applied = the anchor is not found exactly once. The unmutated copy must pass first and the restored copy last.
The engine-backend replay test (test_replay_engine.py) is run for each killed or surviving mutant too when a sutekh
binary is given, because the same validator drives it.

Usage: python3 -I tests/rts/replay/replay_mutants.py <output.json> [<path to sutekh>]
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TARGET = "bench/rts/replay/rts_replay.py"

MUTANTS = [
    ("M3-M18a", "replay compares only the digest record at t = 0", "if t in want:", "if t in want and t == 0:"),
    ("M3-M18b", "replay skips the final digest record", "if t in want:", 'if t in want and t != header["ticks"]:'),
    ("M3-M18c", "replay ignores the missing-digest check", "if missing:", "if False:"),
    ("M3-M18d", "reader ignores seq order within a boundary", "if t < last_t or (t == last_t and seq <= last_seq):",
     "if t < last_t:"),
    ("M3-M18e", "replay applies a boundary's commands in reverse seq order", "for c in by_t.get(t, []):",
     "for c in reversed(by_t.get(t, [])):"),
    ("M3-M18f", "reader ignores digest record order", 'if not _is_u(rec["t"]) or rec["t"] <= last_dt or rec["t"] > header["ticks"]:',
     'if not _is_u(rec["t"]) or rec["t"] > header["ticks"]:'),
]


def sha(path: str) -> str:
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def run(tree: str, binary: str | None) -> dict:
    out = {"test_replay": subprocess.run([sys.executable, "-I", "tests/rts/replay/test_replay.py"], cwd=tree,
                                         capture_output=True, timeout=1800).returncode}
    if binary:
        out["test_replay_engine"] = subprocess.run([sys.executable, "-I", "tests/rts/replay/test_replay_engine.py",
                                                    binary], cwd=tree, capture_output=True, timeout=3600).returncode
    return out


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    out_path = sys.argv[1]
    binary = os.path.abspath(sys.argv[2]) if len(sys.argv) == 3 else None
    base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    tree = tempfile.mkdtemp(prefix="replay-mutants-")
    try:
        for rel in ("bench/rts", "tests/rts/replay", "tests/rts/ref", "capsules/rts"):
            shutil.copytree(os.path.join(REPO, rel), os.path.join(tree, rel), ignore=shutil.ignore_patterns("__pycache__"))
        target = os.path.join(tree, TARGET)
        orig = open(target, encoding="utf-8").read()
        result = {"campaign": "M3-M18 replay validator mutants", "base_revision": base,
                  "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "source_sha256": sha(target),
                  "baseline": run(tree, binary), "mutants": []}
        if any(result["baseline"].values()):
            result["error"] = "baseline failed"
            json.dump(result, open(out_path, "w"), indent=2)
            return 2
        for mid, text, old, new in MUTANTS:
            rec = {"id": mid, "mutation": text}
            if orig.count(old) != 1:
                rec["status"] = "not_applied"
            else:
                open(target, "w", encoding="utf-8").write(orig.replace(old, new))
                rec["source_sha256_after"] = sha(target)
                rec["exit"] = run(tree, binary)
                failed = [k for k, v in rec["exit"].items() if v != 0]
                rec["status"], rec["killed_by"] = ("killed" if failed else "survived"), failed
                open(target, "w", encoding="utf-8").write(orig)
            result["mutants"].append(rec)
            print(f"{mid:9} {rec['status']:11} {rec.get('killed_by', '')} {text}", flush=True)
        result["restored"] = run(tree, binary)
        result["summary"] = {s: sum(1 for m in result["mutants"] if m["status"] == s)
                             for s in ("killed", "survived", "not_applied")}
        json.dump(result, open(out_path, "w"), indent=2)
        print(result["summary"], "restored:", "ok" if not any(result["restored"].values()) else "FAILED")
        return 0 if not any(result["restored"].values()) else 1
    finally:
        shutil.rmtree(tree, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
