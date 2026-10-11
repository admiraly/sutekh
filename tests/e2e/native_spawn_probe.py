#!/usr/bin/env python3
"""TX4 instrument (M1-02c-3): prove that native specialization, IR edits and installs execute no
compiler, assembler, linker or build tool (stdlib only; reuses the detector of no_build_probe.py).

Run:  python3 tests/e2e/native_spawn_probe.py --binary <sutekh> [--out PATH]

Session driven (JSON lines on stdin, answers read from stdout), traced as one tree:
  strace -f -ttt -e trace=execve,execveat -s 4096 -o LOG -- <binary> serve   (cwd = repo root)
  engine.info; world.create (edit fixtures); world.specialize scalar (and avx2 when the host has it);
  capsule.submit + candidate.evaluate of movement_v2 (the IR edit); candidate.install of it; world.specialize
  again at the new revision; one world.step; world.close.

Pass rules:
  * Positive controls first (no_build_probe.positive_controls): a deliberate `cc --version` through PATH and
    through its absolute path must both be detected. A miss means the detector is not trusted: inconclusive.
  * Every request of the session must answer ok. A failed answer is inconclusive, not a pass.
  * Any successful exec of a build tool (cc, gcc, clang, as, ld, cmake, make, ...) anywhere in the traced
    tree is a violation. Only the binary itself (the supervisor and its worker re-exec) may be executed.
  * The traced tree must show the binary's own exec (the strace child) and, when the session spawns a
    worker, at least one re-exec of the binary; otherwise the trace is not evidence of the session.

Exit status: 0 pass; 1 violation; 2 inconclusive (no strace, ptrace denied, control missed, a request failed,
unreadable log); 3 unavailable (the binary does not advertise world.specialize, i.e. not a native build).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import no_build_probe as nbp  # noqa: E402  (detector, strace parser, positive controls)

PROBE_FORMAT = "m0-native-spawn-probe/0.1"
EDIT_WORLD = "tests/fixtures/edit/world.json"
EDIT_V1 = "tests/fixtures/edit/movement.capsule.json"
EDIT_V2 = "tests/fixtures/edit/movement_v2.capsule.json"
EDIT_V2_TEST = "tests/fixtures/edit/movement_v2.test.json"
STRACE_ERR_MARKERS = ("Operation not permitted", "ptrace", "attach: ", "Permission denied")


class Inconclusive(Exception):
    pass


class Unavailable(Exception):
    pass


class Session:
    """One traced `<binary> serve` process. Requests are answered in order."""

    def __init__(self, binary: str, log_path: str):
        if shutil.which("strace") is None:
            raise Inconclusive("strace is not installed")
        cmd = ["strace", "-f", "-ttt", "-e", "trace=execve,execveat", "-s", "4096", "-o", log_path,
               "--", binary, "serve"]
        self.errlog = log_path + ".stderr"
        self.proc = subprocess.Popen(cmd, cwd=REPO, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=open(self.errlog, "w", encoding="utf-8"), shell=False, text=True,
                                     bufsize=1)
        self.n = 0

    def call(self, method: str, params: dict) -> dict:
        self.n += 1
        line = json.dumps({"protocol": "0.1", "id": f"tx4-{self.n}", "method": method, "params": params})
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            answer = self.proc.stdout.readline()
        except (BrokenPipeError, OSError) as exc:
            raise Inconclusive(f"supervisor pipe failed during {method}: {exc}")
        if not answer:
            raise Inconclusive(f"supervisor closed its output during {method}")
        try:
            env = json.loads(answer)
        except json.JSONDecodeError:
            raise Inconclusive(f"{method}: answer is not JSON")
        return env

    def ok(self, method: str, params: dict) -> dict:
        env = self.call(method, params)
        if env.get("ok") is not True:
            raise Inconclusive(f"{method} failed: {json.dumps(env.get('error'))[:300]}")
        return env["result"]

    def finish(self) -> int:
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        return self.proc.wait(timeout=120)


def read_log(path: str) -> list:
    if not os.path.exists(path):
        raise Inconclusive("strace wrote no log")
    with open(path, encoding="utf-8", errors="replace") as fh:
        return nbp.parse_strace(fh.read())


def run_session(binary: str, tmp: str) -> dict:
    log = os.path.join(tmp, "session.strace")
    s = Session(binary, log)
    steps = []
    try:
        info = s.ok("engine.info", {})
        spec = (info.get("native") or {}).get("specialization")
        if not isinstance(spec, dict):
            raise Unavailable("engine.info does not advertise world.specialize (not a native build)")
        host = spec.get("host_caps") or {}
        backends = ["scalar"] + (["avx2"] if host.get("avx2") is True else [])
        created = s.ok("world.create", {"fixture_path": EDIT_WORLD, "capsule_path": EDIT_V1})
        wid = created["world_id"]
        rev = created["world_revision"]
        steps.append("world.create")
        for b in backends:
            res = s.ok("world.specialize", {"world_id": wid, "expected_world_revision": rev,
                                            "system_id": "demo.movement", "backend": b})
            steps.append(f"world.specialize {b} -> {res.get('execution_tier')}")
        sub = s.ok("capsule.submit", {"capsule_path": EDIT_V2})
        cid = sub["candidate_id"]
        steps.append("capsule.submit (IR edit)")
        ev = s.ok("candidate.evaluate", {"candidate_id": cid, "test_path": EDIT_V2_TEST})
        if ev.get("status") != "passed":
            raise Inconclusive(f"candidate.evaluate did not pass: {ev.get('status')}")
        steps.append("candidate.evaluate")
        inst = s.ok("candidate.install", {"candidate_id": cid, "world_id": wid, "expected_world_revision": rev,
                                          "allowed_effective_tick": 0})
        rev = inst.get("world_revision", rev)
        steps.append("candidate.install")
        for b in backends:
            res = s.ok("world.specialize", {"world_id": wid, "expected_world_revision": rev,
                                            "system_id": "demo.movement", "backend": b})
            steps.append(f"world.specialize {b} after install -> {res.get('execution_tier')}")
        stepped = s.ok("world.step", {"world_id": wid, "expected_world_revision": rev, "ticks": 1})
        steps.append("world.step")
        s.ok("world.close", {"world_id": wid, "expected_world_revision": stepped.get("world_revision", rev)})
        steps.append("world.close")
        rc = s.finish()
    except Exception:
        if s.proc.poll() is None:
            s.proc.kill()
            s.proc.wait()
        raise
    with open(s.errlog, encoding="utf-8", errors="replace") as fh:
        err = fh.read()
    if any(m in err for m in STRACE_ERR_MARKERS):
        raise Inconclusive("strace could not attach (ptrace denied?): " + err[-300:])
    if rc != 0:
        raise Inconclusive(f"strace/supervisor returned {rc}: {err[-300:]}")
    return {"steps": steps, "log": log, "records": read_log(log), "backends": backends}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(REPO, "build", "sutekh"))
    ap.add_argument("--out", default="")
    args = ap.parse_args(argv)
    binary = os.path.abspath(args.binary)
    evidence = {"format": PROBE_FORMAT, "binary": binary, "verdict": None}
    tmp = tempfile.mkdtemp(prefix="tx4-")
    try:
        if not os.path.isfile(binary):
            raise Unavailable(f"binary not found: {binary}")
        if shutil.which("strace") is None:
            raise Inconclusive("strace is not installed")
        # Positive controls: the detector must see deliberate build-tool execs before any claim.
        controls = nbp.positive_controls(tmp)
        evidence["controls"] = [{"name": c["name"], "detected": c["detected"]} for c in controls]
        if not all(c["detected"] for c in controls):
            raise Inconclusive("positive control missed: the build-tool detector is not trusted")
        run = run_session(binary, tmp)
        records = run["records"]
        summary = nbp.summarise(records)
        own = os.path.basename(binary)
        own_execs = [r for r in records if r["rc"] == 0 and os.path.basename(r["path"]) == own]
        evidence.update(steps=run["steps"], backends=run["backends"], summary=summary,
                        own_successful_execs=len(own_execs))
        if not own_execs:
            raise Inconclusive("the trace shows no successful exec of the binary: not evidence")
        if summary["build_tool_execs_total"]:
            evidence["verdict"] = "violation"
            print("violation: build tool executed: " + json.dumps(summary["build_tool_execs_total"]))
            code = 1
        else:
            evidence["verdict"] = "pass"
            print(f"pass: {len(records)} exec attempts, {summary['successful_execs']} successful, "
                  f"no build tool; executed: {', '.join(summary['distinct_executed_basenames'])}")
            code = 0
    except Unavailable as exc:
        evidence.update(verdict="unavailable", reason=str(exc))
        print(f"unavailable: {exc}")
        code = 3
    except Inconclusive as exc:
        evidence.update(verdict="inconclusive", reason=str(exc))
        print(f"inconclusive: {exc}")
        code = 2
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(evidence, fh, indent=2, sort_keys=True)
            fh.write("\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
