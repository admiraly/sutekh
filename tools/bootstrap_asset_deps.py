#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Fetch and verify the pinned single-file libraries used by the asset importer and the audio decoder.

    cgltf 1.15 (MIT)                        glTF 2.0 parser            -> third_party/cgltf/
    stb_image 2.30, stb_vorbis 1.22         PNG/JPEG and Ogg Vorbis    -> third_party/stb/
    (MIT or public domain, at your choice)

Modes:
  (default)   download every file from the pinned commit, verify its SHA-256 against the pin in this script, and
              write third_party/asset-deps.lock.json
  --check     verify the files already present, offline
  --dest DIR  use DIR/cgltf and DIR/stb instead of third_party/cgltf and third_party/stb, for a project that includes
              Sutekh as a submodule and must not write into it; the tracked lock file is not written (its content
              follows from the pins below). Configure with -DSUTEKH_DEPS_DIR=DIR.

The files are fetched at bootstrap and not redistributed in this repository (as for AsmJit and Vulkan-Headers).
Without them CMake builds the cooked-asset runtime (src/asset/su_asset_pak.c) and the audio mixer with WAV only,
and skips the glTF importer and the Ogg decoder. The pins below are the integrity check: a file whose hash differs
is refused, whatever the network returned.
Stdlib only.
"""

import argparse
import hashlib
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS_ROOT = os.path.join(ROOT, "third_party")  # --dest replaces it
LOCK = os.path.join(ROOT, "third_party", "asset-deps.lock.json")

DEPS = [
    {
        "name": "cgltf",
        "version": "1.15",
        "origin": "https://github.com/jkuhlmann/cgltf",
        "commit": "360db1a95480fe102ae9c69b27c5d101167ff5ba",
        "license": "MIT",
        "dir": "cgltf",
        "files": {
            "cgltf.h": "e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec",
            "LICENSE": "f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95",
        },
    },
    {
        "name": "stb (stb_image 2.30, stb_vorbis 1.22)",
        "version": "stb_image 2.30, stb_vorbis 1.22",
        "origin": "https://github.com/nothings/stb",
        "commit": "2c980bb59875b0d32144a71867fbdebb2f77cd20",
        "license": "MIT OR Unlicense (public domain), at the user's choice",
        "dir": "stb",
        "files": {
            "stb_image.h": "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3",
            "stb_vorbis.c": "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636",
            "LICENSE": "bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63",
        },
    },
]


def raw_url(dep, name):
    owner_repo = dep["origin"].split("github.com/", 1)[1]
    return "https://raw.githubusercontent.com/%s/%s/%s" % (owner_repo, dep["commit"], name)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def verify_all():
    errors = []
    for dep in DEPS:
        for name, want in dep["files"].items():
            path = os.path.join(DEPS_ROOT, dep["dir"], name)
            if not os.path.isfile(path):
                errors.append("%s missing" % path)
                continue
            with open(path, "rb") as fh:
                got = sha256_bytes(fh.read())
            if got != want:
                errors.append("%s has sha256 %s, pinned %s" % (path, got, want))
    return errors


def write_lock():
    lock = {
        "pinned_by": "tools/bootstrap_asset_deps.py",
        "use": "build-time sources of the glTF importer (src/asset) and the Ogg decoder (src/audio); not redistributed",
        "dependencies": [
            {
                "name": d["name"],
                "version": d["version"],
                "origin": d["origin"],
                "commit": d["commit"],
                "license": d["license"],
                "files": {("third_party/%s/%s" % (d["dir"], n)): h for n, h in sorted(d["files"].items())},
            }
            for d in DEPS
        ],
    }
    with open(LOCK, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(lock, fh, indent=2, sort_keys=True)
        fh.write("\n")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--dest", help="directory that receives cgltf/ and stb/ (default: third_party)")
    args = ap.parse_args(argv)
    global DEPS_ROOT
    if args.dest:
        DEPS_ROOT = os.path.abspath(args.dest)
    if args.check:
        errors = verify_all()
        for e in errors:
            print("FAIL: " + e)
        if not errors:
            print("asset deps ok (%d files)" % sum(len(d["files"]) for d in DEPS))
        return 1 if errors else 0
    for dep in DEPS:
        folder = os.path.join(DEPS_ROOT, dep["dir"])
        os.makedirs(folder, exist_ok=True)
        for name, want in dep["files"].items():
            path = os.path.join(folder, name)
            if os.path.isfile(path):
                with open(path, "rb") as fh:
                    if sha256_bytes(fh.read()) == want:
                        continue
            url = raw_url(dep, name)
            try:
                with urllib.request.urlopen(url, timeout=60) as resp:
                    data = resp.read()
            except OSError as exc:
                print("FAIL: download %s: %s" % (url, exc))
                return 1
            got = sha256_bytes(data)
            if got != want:
                print("FAIL: %s has sha256 %s, pinned %s; not written" % (url, got, want))
                return 1
            with open(path, "wb") as fh:
                fh.write(data)
            print("fetched %s/%s" % (dep["dir"], name))
    errors = verify_all()
    if errors:
        for e in errors:
            print("FAIL: " + e)
        return 1
    if args.dest:
        print("asset deps ok in %s (lock file not written)" % DEPS_ROOT)
        return 0
    write_lock()
    print("asset deps ok; lock written to %s" % os.path.relpath(LOCK, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
