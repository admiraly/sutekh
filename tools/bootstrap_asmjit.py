#!/usr/bin/env python3
"""Fetch, verify and probe the pinned AsmJit checkout (M1-00, contract section 6).

Modes:
  (default)        fetch the pinned commit into third_party/asmjit/ and write
                   third_party/asmjit.lock.json
  --check          verify an existing checkout offline (commit, license sha256,
                   zlib license text) and compare it with the lock file
  --build-probe    compile the static library and a JIT smoke program in a
                   scratch directory OUTSIDE the repository, then run it

Stdlib only. Every subprocess is started from an argument list with shell=False.
The pin is a commit, not a branch or tag: AsmJit upstream publishes no releases.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_URL = "https://github.com/asmjit/asmjit.git"
PIN = "0d7f01052f2d3f6d6b9bb6714ef32bd7b80177eb"
DEST = os.path.join(REPO_ROOT, "third_party", "asmjit")
LOCK = os.path.join(REPO_ROOT, "third_party", "asmjit.lock.json")
LICENSE_NAMES = ("LICENSE.md", "LICENSE", "COPYING")
# Matched after collapsing whitespace, because the license wraps lines.
ZLIB_MARKERS = (
    "Permission is granted to anyone to use this software for any purpose",
    "The origin of this software must not be misrepresented",
    "Altered source versions must be plainly marked as such",
    "This notice may not be removed or altered from any source distribution",
)
PROBE_FLAGS = ["-std=c++20", "-DASMJIT_STATIC", "-DASMJIT_NO_AARCH64", "-DASMJIT_NO_UJIT"]
EXCLUDED_DIRS = ("arm", "a64", "ujit")

SMOKE_CPP = r'''
#include <asmjit/x86.h>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace asmjit;
typedef uint32_t (*AddFn)(uint32_t, uint32_t);

int main() {
    JitRuntime rt;
    CodeHolder code;
    code.init(rt.environment(), rt.cpu_features());
    x86::Assembler a(&code);
#if defined(_WIN32)
    // Win64: the arguments arrive in rcx and rdx. Zero-extend both (upper halves unspecified for uint32_t).
    a.mov(x86::ecx, x86::ecx);
    a.mov(x86::edx, x86::edx);
    a.lea(x86::eax, x86::ptr(x86::rcx, x86::rdx));
    printf("ABI win64 args=rcx,rdx\n");
#else
    // SysV: the arguments arrive in rdi and rsi. Zero-extend both (upper halves unspecified for uint32_t).
    a.mov(x86::edi, x86::edi);
    a.mov(x86::esi, x86::esi);
    a.lea(x86::eax, x86::ptr(x86::rdi, x86::rsi));
    printf("ABI sysv args=rdi,rsi\n");
#endif
    a.ret();
    AddFn fn = nullptr;
    Error err = rt.add(&fn, &code);
    if (err != Error::kOk) {
        printf("ERROR add code=%u\n", (unsigned)err);
        return 2;
    }
    uint32_t r1 = fn(4000000000u, 500000000u);
    uint32_t r2 = fn(1u, 2u);
    int ok1 = r1 == 205032704u;
    int ok2 = r2 == 3u;
    printf("CALL lea32 4000000000+500000000 got=%" PRIu32 " want=205032704 ok=%d\n", r1, ok1);
    printf("CALL lea32 1+2 got=%" PRIu32 " want=3 ok=%d\n", r2, ok2);
    // W^X check: report the permissions of the mapping that holds the generated code.
    uintptr_t addr = (uintptr_t)fn;
    int found = 0;
#if defined(_WIN32)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void*)addr, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT) {
            const char* perms = "---p";
            switch (mbi.Protect & 0xFFu) {
            case PAGE_EXECUTE_READ: perms = "r-xp"; break;
            case PAGE_EXECUTE_READWRITE: perms = "rwxp"; break;
            case PAGE_EXECUTE_WRITECOPY: perms = "rwxp"; break;
            case PAGE_READWRITE: perms = "rw-p"; break;
            case PAGE_READONLY: perms = "r--p"; break;
            case PAGE_EXECUTE: perms = "--xp"; break;
            default: break;
            }
            unsigned long long lo = (unsigned long long)(uintptr_t)mbi.BaseAddress;
            printf("MAP perms=%s range=%llx-%llx line=[VirtualQuery protect=0x%lx type=0x%lx]\n", perms, lo,
                   lo + (unsigned long long)mbi.RegionSize, (unsigned long)mbi.Protect, (unsigned long)mbi.Type);
            found = 1;
        }
    }
    FILE* f = nullptr;
#else
    FILE* f = fopen("/proc/self/maps", "r");
#endif
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long long lo = 0, hi = 0;
            char perms[8] = {0};
            if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi) {
                line[strcspn(line, "\n")] = 0;
                printf("MAP perms=%s range=%llx-%llx line=[%s]\n", perms, lo, hi, line);
                found = 1;
            }
        }
        fclose(f);
    }
    if (!found) printf("MAP none\n");
    return (ok1 && ok2) ? 0 : 1;
}
'''


class Refused(Exception):
    pass


def _git(args):
    """Run git with an argument list. Returns (returncode, stdout)."""
    res = subprocess.run(["git", *args], shell=False, capture_output=True, text=True, check=False)
    return res.returncode, res.stdout.strip(), res.stderr.strip()


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _head(dest):
    if not os.path.isdir(os.path.join(dest, ".git")):
        return None
    rc, out, _ = _git(["-C", dest, "rev-parse", "HEAD"])
    return out if rc == 0 else None


def _license_path(dest):
    for name in LICENSE_NAMES:
        p = os.path.join(dest, name)
        if os.path.isfile(p):
            return p
    return None


def _license_info(dest):
    path = _license_path(dest)
    if path is None:
        return {"license_file": None, "license_sha256": None, "zlib_text_present": False}
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = " ".join(fh.read().split())
    return {
        "license_file": os.path.relpath(path, dest),
        "license_sha256": _sha256_file(path),
        "zlib_text_present": all(m in text for m in ZLIB_MARKERS),
    }


def _write_lock(commit, info):
    lock = {
        "name": "asmjit",
        "origin": REPO_URL,
        "commit": commit,
        "license": "zlib",
        "license_file": info["license_file"],
        "license_sha256": info["license_sha256"],
        "pinned_by": "tools/bootstrap_asmjit.py",
        "pin_note": "No upstream release tags exist; pinned by commit.",
    }
    os.makedirs(os.path.dirname(LOCK), exist_ok=True)
    with open(LOCK, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(lock, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return lock


def fetch(force):
    head = _head(DEST) if os.path.exists(DEST) else None
    if os.path.exists(DEST):
        if head != PIN and not force:
            raise Refused(f"{DEST} exists at {head or 'a non-git tree'}, not the pin; pass --force to replace it")
        if head != PIN:
            shutil.rmtree(DEST)  # only the fixed, git-ignored third_party/asmjit path
    if not os.path.exists(DEST):
        os.makedirs(DEST)
        for argv in (
            ["init", "-q", DEST],
            # Byte-exact checkout on every host: a global core.autocrlf=true (the Git for Windows default) would
            # rewrite LICENSE.md and the sources with CRLF and change the pinned license hash.
            ["-C", DEST, "config", "core.autocrlf", "false"],
            ["-C", DEST, "config", "core.eol", "lf"],
            ["-C", DEST, "remote", "add", "origin", REPO_URL],
            ["-C", DEST, "fetch", "-q", "--depth", "1", "origin", PIN],
            ["-C", DEST, "-c", "advice.detachedHead=false", "checkout", "-q", "--detach", "FETCH_HEAD"],
        ):
            rc, _, err = _git(argv)
            if rc != 0:
                raise Refused(f"git {' '.join(argv[:3])} failed: {err}")
    got = _head(DEST)
    if got != PIN:
        raise Refused(f"HEAD is {got}, expected pin {PIN}")
    info = _license_info(DEST)
    if not info["zlib_text_present"]:
        raise Refused("zlib license text not found in the fetched LICENSE file")
    lock = _write_lock(PIN, info)
    print(json.dumps({"ok": True, "mode": "fetch", "commit": got, **info, "lock": LOCK,
                      "lock_written": lock["commit"] == PIN}, indent=2, sort_keys=True))
    return 0


def check():
    result = {"ok": False, "mode": "check", "dest": DEST}
    if not os.path.isdir(DEST):
        result["error"] = "checkout missing; run without --check first"
        print(json.dumps(result, indent=2, sort_keys=True))
        return 2
    head = _head(DEST)
    result["commit"] = head
    result["commit_matches_pin"] = head == PIN
    result.update(_license_info(DEST))
    lock_ok = None
    if os.path.isfile(LOCK):
        with open(LOCK, encoding="utf-8") as fh:
            lock = json.load(fh)
        lock_ok = lock.get("commit") == PIN and lock.get("license_sha256") == result["license_sha256"]
    result["lock_matches"] = lock_ok
    result["ok"] = bool(result["commit_matches_pin"] and result["zlib_text_present"] and lock_ok)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


def _sources(dest):
    srcs = []
    for root, dirs, files in os.walk(dest):
        if ".git" in root.split(os.sep):
            continue
        rel = os.path.relpath(root, dest).split(os.sep)
        if "asmjit" not in rel:
            continue
        if any(d in rel for d in EXCLUDED_DIRS):
            continue
        for f in files:
            if f.endswith(".cpp"):
                srcs.append(os.path.join(root, f))
    return sorted(srcs)


def _include_root(dest):
    for root, _, files in os.walk(dest):
        if "x86.h" in files and os.path.basename(root) == "asmjit":
            return os.path.dirname(root)
    raise Refused("cannot locate the asmjit include root")


def build_probe(cxx, scratch, jobs):
    if shutil.which(cxx) is None:
        return {"ok": False, "error": f"{cxx} not found on PATH"}
    if os.path.realpath(scratch).startswith(os.path.realpath(REPO_ROOT) + os.sep):
        return {"ok": False, "error": "scratch directory must be outside the repository"}
    if _head(DEST) != PIN:
        return {"ok": False, "error": "checkout missing or not at the pin; run the fetch first"}
    os.makedirs(scratch, exist_ok=True)
    obj_dir = os.path.join(scratch, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    inc = _include_root(DEST)
    srcs = _sources(DEST)
    report = {"ok": False, "mode": "build-probe", "cxx": cxx, "cxx_version": None,
              "commit": PIN, "source_files": len(srcs), "scratch": scratch, "flags": PROBE_FLAGS}
    rc, out, _ = _run_capture([cxx, "--version"])
    report["cxx_version"] = out.splitlines()[0] if out else None

    def compile_one(src):
        obj = os.path.join(obj_dir, hashlib.sha1(src.encode()).hexdigest() + ".o")
        argv = [cxx, *PROBE_FLAGS, "-O2", "-I", inc, "-c", src, "-o", obj]
        t0 = time.monotonic()
        rc, _, err = _run_capture(argv)
        return {"src": os.path.relpath(src, DEST), "obj": obj, "rc": rc,
                "seconds": round(time.monotonic() - t0, 3), "stderr_head": err[:400] if rc else ""}

    t_all = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(compile_one, srcs))
    report["compile_wall_seconds"] = round(time.monotonic() - t_all, 3)
    failed = [r for r in results if r["rc"] != 0]
    report["compile_failures"] = [{k: r[k] for k in ("src", "rc", "stderr_head")} for r in failed]
    report["compile_jobs"] = jobs
    if failed:
        report["error"] = "library compile failed"
        return report
    win = sys.platform == "win32"
    lib = os.path.join(scratch, "asmjit.lib" if win else "libasmjit.a")
    if os.path.exists(lib):
        os.remove(lib)
    ar = (shutil.which("llvm-ar") or "llvm-ar") if win else "ar"
    report["archiver"] = ar
    t0 = time.monotonic()
    rc, _, err = _run_capture([ar, "rcs", lib, *[r["obj"] for r in results]])
    report["archive_seconds"] = round(time.monotonic() - t0, 3)
    report["archive_rc"] = rc
    if rc != 0:
        report["error"] = f"ar failed: {err[:400]}"
        return report
    smoke_src = os.path.join(scratch, "smoke.cpp")
    with open(smoke_src, "w", encoding="utf-8") as fh:
        fh.write(SMOKE_CPP)
    exe = os.path.join(scratch, "smoke.exe" if win else "smoke")
    t0 = time.monotonic()
    rc, _, err = _run_capture([cxx, *PROBE_FLAGS, "-O2", "-I", inc, smoke_src, lib, "-o", exe])
    report["smoke_link_seconds"] = round(time.monotonic() - t0, 3)
    report["smoke_link_rc"] = rc
    if rc != 0:
        report["error"] = f"smoke link failed: {err[:400]}"
        return report
    t0 = time.monotonic()
    run = subprocess.run([exe], shell=False, capture_output=True, text=True, timeout=60, check=False)
    report["smoke_run_seconds"] = round(time.monotonic() - t0, 4)
    report["smoke_return_code"] = run.returncode
    report["smoke_stdout"] = run.stdout.splitlines()
    report["smoke_stderr_head"] = run.stderr[:400]
    maps = [l for l in run.stdout.splitlines() if l.startswith("MAP ")]
    report["jit_mapping"] = maps[0] if maps else None
    rwx = any("perms=rwx" in l for l in maps)
    report["wx_finding"] = "FINDING: rwx JIT mapping" if rwx else None
    report["abi"] = next((l for l in run.stdout.splitlines() if l.startswith("ABI ")), None)
    report["ok"] = run.returncode == 0 and bool(maps) and not rwx
    return report


def _run_capture(argv):
    res = subprocess.run(argv, shell=False, capture_output=True, text=True, check=False)
    return res.returncode, res.stdout.strip(), res.stderr.strip()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify existing checkout offline")
    mode.add_argument("--build-probe", action="store_true", help="compile library and run JIT smoke test")
    ap.add_argument("--force", action="store_true", help="replace an existing checkout at a different commit")
    ap.add_argument("--cxx", default="g++", choices=["g++", "clang++"], help="compiler for --build-probe")
    ap.add_argument("--scratch", default=None, help="scratch directory outside the repo (default: a new temp dir)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel compile jobs")
    args = ap.parse_args(argv)
    try:
        if args.check:
            return check()
        if args.build_probe:
            scratch = args.scratch or tempfile.mkdtemp(prefix="asmjit-probe-")
            report = build_probe(args.cxx, scratch, max(1, args.jobs))
            print(json.dumps(report, indent=2, sort_keys=True))
            return 0 if report.get("ok") else 1
        return fetch(args.force)
    except Refused as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, indent=2, sort_keys=True))
        return 1


if __name__ == "__main__":
    sys.exit(main())
