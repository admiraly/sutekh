#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Print a normalized description of a configured CMake build, for comparing two checkouts (issue 143 evidence):
every target (name, type, artifact paths) from the CMake file API, every test (name, properties) from
`ctest --show-only=json-v1`, and every add_test / set_tests_properties line of the generated CTestTestfile.cmake
(full commands with resolved paths, whether or not the programs are built yet; source line backtraces removed).
Source and build paths are replaced by <SRC> and <BLD>.

usage: snapshot_build.py configure SRC BLD [cmake args...]   (writes the file API query, then configures)
       snapshot_build.py dump SRC BLD                         (prints the snapshot of a configured build)
"""
import json, os, re, subprocess, sys

def norm(s, src, bld):
    return s.replace(bld, "<BLD>").replace(src, "<SRC>") if isinstance(s, str) else s

def dump(src, bld):
    src, bld = os.path.realpath(src), os.path.realpath(bld)
    reply = os.path.join(bld, ".cmake", "api", "v1", "reply")
    index = sorted(f for f in os.listdir(reply) if f.startswith("index-"))[-1]
    idx = json.load(open(os.path.join(reply, index)))
    cm = next(o for o in idx["objects"] if o["kind"] == "codemodel")
    model = json.load(open(os.path.join(reply, cm["jsonFile"])))
    lines = []
    for cfg in model["configurations"]:
        for t in cfg["targets"]:
            tj = json.load(open(os.path.join(reply, t["jsonFile"])))
            arts = sorted(norm(os.path.join(bld, a["path"]) if not os.path.isabs(a["path"]) else a["path"], src, bld)
                          for a in tj.get("artifacts", []))
            lines.append("target %s %s %s" % (tj["name"], tj["type"], " ".join(arts)))
    out = subprocess.run(["ctest", "--test-dir", bld, "--show-only=json-v1"], capture_output=True, text=True,
                         check=True).stdout
    tests = json.loads(out)["tests"]
    for t in tests:
        props = sorted((p["name"], json.dumps(p["value"], sort_keys=True)) for p in t.get("properties", []))
        lines.append("test %s props=%s" % (t["name"], norm(json.dumps(props), src, bld)))
    # The generated test file has every command with resolved paths, built or not.
    for l in open(os.path.join(bld, "CTestTestfile.cmake"), encoding="utf-8"):
        if l.startswith(("add_test", "set_tests_properties")):
            l = re.sub(r' _BACKTRACE_TRIPLES "[^"]*"', "", l.rstrip("\n"))  # source line numbers
            lines.append("ctestfile " + norm(l, src, bld))
    for l in sorted(lines):
        print(l)

def main(argv):
    mode, src, bld = argv[0], argv[1], argv[2]
    if mode == "configure":
        q = os.path.join(bld, ".cmake", "api", "v1", "query")
        os.makedirs(q, exist_ok=True)
        open(os.path.join(q, "codemodel-v2"), "w").close()
        subprocess.run(["cmake", "-S", src, "-B", bld, *argv[3:]], check=True, stdout=subprocess.DEVNULL)
    else:
        dump(src, bld)

if __name__ == "__main__":
    main(sys.argv[1:])
