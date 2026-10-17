# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Runtime mutation campaign for the M3 control plane (contract M3 section 15: M3-M19, M3-M28, M3-M30, M3-M35).

Every mutant edits real engine source in an isolated git worktree of HEAD, builds it, and runs the named detectors
(ctest names; also M3-M20 and M3-M33). A mutant is
  killed        a detector failed on the built mutant;
  survived      every detector passed (an evidence gap or a missing test, never counted as killed);
  build_failed  the mutant did not compile or link (classified apart from a semantic kill);
  not_applied   the edit anchor was not found exactly once (the campaign is stale).
The unmutated tree must pass every detector first ("baseline") and the restored tree again at the end ("restored").
Results go to a JSON file with the base revision, the SHA-256 of every mutated source file (before and after) and
the exit status of each detector. Python reference-model mutants (tests/rts/ref/m3_control_mutants.py) are
supporting oracle coverage and are not repeated here.

Usage: python3 -I tests/rts/runtime_mutants.py <output.json> [--only M3-M19a,...] [--keep]
Run from the repository root of a clean checkout. Needs cmake, ninja and a C compiler.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = "/root/sutekh-runtime-mutants"
FN_END = "\nstatic "  # a mutant edit is confined to one function: from its marker to the next top-level static

SPAWN_DET = ["rts_world_spawn_batch", "rts_ctrl_engine"]
SETF_DET = ["rts_world_set_fields", "rts_ctrl_engine"]
DOMAIN_DET = ["rts_control_domains", "rts_world_spawn_batch", "rts_world_set_fields", "rts_ctrl_engine"]
WORKER = "src/runtime/worker.c"
DOMAINS = "src/runtime/rts/control_domains.c"

# id, contract text, file, function marker (None = whole file), old text (exactly once in scope), new text, detectors
MUTANTS = [
    ("M3-M19a", "spawn_batch ignores expected_world_revision", WORKER, "static uint32_t h_world_spawn_batch(",
     "st = check_expected(ww, expect, f);", "st = SU_OK; (void)expect;", ["rts_world_spawn_batch", "rts_ctrl_engine"]),
    ("M3-M19b", "set_fields ignores expected_world_revision", WORKER, "static uint32_t h_world_set_fields(",
     "st = check_expected(ww, expect, f);", "st = SU_OK; (void)expect;", ["rts_ctrl_engine"]),
    ("M3-M19c", "world.spawn ignores expected_world_revision", WORKER, "static uint32_t h_world_spawn(",
     "st = check_expected(ww, expect, f);", "st = SU_OK; (void)expect;", ["rts_ctrl_engine", "recovery"]),
    ("M3-M28a", "world.create accepts max_struct_per_tick below capacity", WORKER, "static uint32_t h_world_create(",
     "(max_struct < su_world_row_count(inst.w) ||", "((0 && max_struct < su_world_row_count(inst.w)) ||",
     ["rts_caps_engine"]),
    ("M3-M28b", "world.create accepts max_events_per_tick below capacity", WORKER, "static uint32_t h_world_create(",
     "max_events < su_world_row_count(inst.w)))", "(0 && max_events < su_world_row_count(inst.w))))",
     ["rts_caps_engine"]),
    ("M3-M28c", "config check applies to no schema (rts profile test dropped)", WORKER,
     "static uint32_t h_world_create(", 'strcmp(sched.schema_id, "rts") == 0 &&', "0 &&", ["rts_caps_engine"]),
    ("M3-M30a", "flight = 0 accepted", DOMAINS, None, "return value >= 1u && value <= FLIGHT_MAX ? SU_OK : SU_E_TYPE;",
     "return value <= FLIGHT_MAX ? SU_OK : SU_E_TYPE;", DOMAIN_DET),
    ("M3-M30b", "flight above 64 accepted", DOMAINS, None,
     "return value >= 1u && value <= FLIGHT_MAX ? SU_OK : SU_E_TYPE;", "return value >= 1u ? SU_OK : SU_E_TYPE;",
     DOMAIN_DET),
    ("M3-M30c", "set_fields writes any schema field (writable set not enforced)", WORKER,
     "static uint32_t h_world_set_fields(", "if (!su_rts_ctl_writable(name)) {", "if (0 && !su_rts_ctl_writable(name)) {",
     SETF_DET),
    ("M3-M30d", "spawn accepts a nonzero system-owned field", DOMAINS, "uint32_t su_rts_ctl_spawn_row(",
     "if (k >= 0 && in[k].value != 0u) {", "if (0 && k >= 0 && in[k].value != 0u) {", SPAWN_DET),
    ("M3-M30e", "set_fields skips the per-field domain check", WORKER, "static uint32_t h_world_set_fields(",
     "if (su_rts_ctl_check_value(name, (uint32_t)v) != SU_OK) {",
     "if (0 && su_rts_ctl_check_value(name, (uint32_t)v) != SU_OK) {", SETF_DET),
    ("M3-M30g", "writable set not enforced anywhere (worker check off and check_value accepts unknown names)", WORKER,
     "static uint32_t h_world_set_fields(", "if (!su_rts_ctl_writable(name)) {", "if (0 && !su_rts_ctl_writable(name)) {",
     SETF_DET, [(DOMAINS, "uint32_t su_rts_ctl_check_value(", "    return SU_E_TYPE;\n}\n\nuint32_t su_rts_ctl_check_formation_slot",
                 "    return SU_OK;\n}\n\nuint32_t su_rts_ctl_check_formation_slot")]),
    ("M3-M20", "rts.scatter delivers damage from a dead sender (stale projectile fields of a despawned shooter)",
     "profiles/rts/native/rts_scatter.c", None, "if (g_alive[r] == 0u) {", "if (0) {",
     ["rts_native_ops", "rts_native_diff", "rts_native_records"]),
    ("M3-M33a", "structural per-row caps use one global value for every system (OD-20 reverted)",
     "src/runtime/entity_tick.c", "static void sys_row_caps(",
     "ps[i] = m->uses_entity_ops ? m->max_struct_per_row : 0u;", "ps[i] = 1u;", ["rts_capacity"]),
    ("M3-M33b", "event per-row caps use one global value for every system (OD-20 reverted)",
     "src/runtime/entity_tick.c", "static void sys_row_caps(",
     "pe[i] = m->uses_entity_ops ? m->max_events_per_row : 0u;", "pe[i] = 1u;", ["rts_capacity"]),
    ("M3-M35a", "spawn accepts an omitted reload, damage or flight (default 0)", DOMAINS, "uint32_t su_rts_ctl_spawn_row(",
     "if (has(required[i], ctx) && find_named(in, n, required[i]) < 0) {",
     "if (strcmp(required[i], \"kind\") == 0 && has(required[i], ctx) && find_named(in, n, required[i]) < 0) {",
     SPAWN_DET),
    ("M3-M35b", "spawn accepts an omitted kind", DOMAINS, "uint32_t su_rts_ctl_spawn_row(",
     "if (has(required[i], ctx) && find_named(in, n, required[i]) < 0) {",
     "if (strcmp(required[i], \"kind\") != 0 && has(required[i], ctx) && find_named(in, n, required[i]) < 0) {",
     SPAWN_DET),
    ("M3-M35c", "set_fields: formation_gen / range_sq resulting-row rules not applied", WORKER,
     "static uint32_t h_world_set_fields(", "if (su_rts_ctl_check_row(kind, range, fs, fg) != SU_OK) {",
     "if (0 && su_rts_ctl_check_row(kind, range, fs, fg) != SU_OK) {", SETF_DET),
    ("M3-M35d", "formation_gen allowed to be nonzero with slot NONE", DOMAINS, "uint32_t su_rts_ctl_check_row(",
     "return formation_gen == 0u ? SU_OK : SU_E_TYPE;", "return SU_OK;", DOMAIN_DET),
    ("M3-M35e", "formation_gen 0 allowed with a real slot (generation never validates)", DOMAINS,
     "uint32_t su_rts_ctl_check_row(", "return formation_gen != 0u ? SU_OK : SU_E_TYPE;", "return SU_OK;", DOMAIN_DET),
    ("M3-M35f", "spawn omits the formation_ox/formation_oy requirement with a real slot", DOMAINS,
     "uint32_t su_rts_ctl_spawn_row(", 'if (has("formation_ox", ctx) && find_named(in, n, "formation_ox") < 0) {',
     'if (0 && has("formation_ox", ctx) && find_named(in, n, "formation_ox") < 0) {', SPAWN_DET),
    ("M3-M35g", "spawn omits the formation_oy requirement with a real slot", DOMAINS, "uint32_t su_rts_ctl_spawn_row(",
     'if (has("formation_oy", ctx) && find_named(in, n, "formation_oy") < 0) {',
     'if (0 && has("formation_oy", ctx) && find_named(in, n, "formation_oy") < 0) {', SPAWN_DET),
]

# Mutants that survive by design, with the reason (reviewed; not counted as killed).
EQUIVALENT = {
    "M3-M30c": "set_fields: su_rts_ctl_check_value also returns SU_E_TYPE for every name outside the writable set, so "
               "the explicit writable check is a redundant first guard; M3-M30g removes both.",
}


def sh(cmd: list, cwd: str, log: str | None = None, timeout: int = 3600) -> int:
    with open(log or os.devnull, "ab") as fh:
        return subprocess.run(cmd, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT, timeout=timeout).returncode


def sha(path: str) -> str:
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def apply(tree: str, file: str, marker: str | None, old: str, new: str) -> bool:
    path = os.path.join(tree, file)
    text = open(path, encoding="utf-8").read()
    start, end = 0, len(text)
    if marker is not None:
        start = text.find(marker)
        if start < 0 or text.count(marker) != 1:
            return False
        stop = text.find(FN_END, start + len(marker))
        end = stop if stop >= 0 else len(text)
    region = text[start:end]
    if region.count(old) != 1:
        return False
    open(path, "w", encoding="utf-8").write(text[:start] + region.replace(old, new) + text[end:])
    return True


def detect(build: str, names: list, log: str) -> dict:
    out = {}
    for name in names:
        out[name] = sh(["ctest", "-R", f"^{name}$", "--output-on-failure"], build, log, timeout=900)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    only = {x for x in args.only.split(",") if x}
    base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=REPO, text=True).strip():
        print("refusing to run: tracked files are modified; the campaign mutates HEAD", file=sys.stderr)
        return 2
    shutil.rmtree(WORK, ignore_errors=True)
    subprocess.check_call(["git", "worktree", "prune"], cwd=REPO)
    subprocess.check_call(["git", "worktree", "add", "-q", "--detach", WORK, base], cwd=REPO)
    build = os.path.join(WORK, "build")
    log = os.path.join(WORK, "campaign.log")
    try:
        if sh(["cmake", "-S", ".", "-B", "build", "-G", "Ninja"], WORK, log) != 0:
            print("configure failed", file=sys.stderr)
            return 2
        all_detectors = sorted({d for m in MUTANTS for d in m[6]})
        if sh(["cmake", "--build", "build"], WORK, log) != 0:
            print("baseline build failed", file=sys.stderr)
            return 2
        baseline = detect(build, all_detectors, log)
        result = {"campaign": "M3 control-plane runtime mutants", "base_revision": base,
                  "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "baseline": baseline,
                  "tool_sha256": sha(os.path.abspath(__file__)), "mutants": []}
        if any(baseline.values()):
            result["error"] = "baseline detector failed; no mutant result is valid"
            json.dump(result, open(args.out, "w"), indent=2)
            print("baseline failed:", baseline, file=sys.stderr)
            return 2
        for entry in MUTANTS:
            mid, text, file, marker, old, new, dets = entry[:7]
            extra = entry[7] if len(entry) > 7 else []
            if only and mid not in only:
                continue
            rec = {"id": mid, "mutation": text, "file": file, "detectors": dets}
            path = os.path.join(WORK, file)
            before = sha(path)
            originals = {f: open(os.path.join(WORK, f), encoding="utf-8").read() for f in {file, *(e[0] for e in extra)}}
            orig = originals[file]
            applied = apply(WORK, file, marker, old, new) and all(apply(WORK, f, m, o, n) for f, m, o, n in extra)
            if not applied:
                rec["status"] = "not_applied"
            else:
                rec["source_sha256_before"], rec["source_sha256_after"] = before, sha(path)
                if sh(["cmake", "--build", "build"], WORK, log) != 0:
                    rec["status"] = "build_failed"
                else:
                    rec["detector_exit"] = detect(build, dets, log)
                    failed = [d for d, rc in rec["detector_exit"].items() if rc != 0]
                    rec["status"] = "killed" if failed else ("equivalent" if mid in EQUIVALENT else "survived")
                    rec["killed_by"] = failed
                    if mid in EQUIVALENT and not failed:
                        rec["equivalent_reason"] = EQUIVALENT[mid]
            for f, text0 in originals.items():
                open(os.path.join(WORK, f), "w", encoding="utf-8").write(text0)
            assert sha(path) == before
            result["mutants"].append(rec)
            print(f"{mid:9} {rec['status']:12} {rec.get('killed_by', '')}  {text}", flush=True)
        sh(["cmake", "--build", "build"], WORK, log)
        result["restored"] = detect(build, all_detectors, log)
        result["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        result["summary"] = {s: sum(1 for m in result["mutants"] if m["status"] == s)
                             for s in ("killed", "equivalent", "survived", "build_failed", "not_applied")}
        json.dump(result, open(args.out, "w"), indent=2)
        print(result["summary"], "restored:", "ok" if not any(result["restored"].values()) else "FAILED")
        return 0 if not any(result["restored"].values()) else 1
    finally:
        if not args.keep:
            subprocess.call(["git", "worktree", "remove", "--force", WORK], cwd=REPO)


if __name__ == "__main__":
    sys.exit(main())
