#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Fetch and verify the pinned SDL3 headers used to build the optional Linux window backend (su_window).

Modes:
  (default)   shallow, sparse clone of the pinned tag into third_party/sdl3/ (gitignored; only include/ and
              LICENSE.txt are checked out), verify the commit and the license, and write
              third_party/sdl3-headers.lock.json
  --check     verify an existing checkout offline against the lock file

Only the headers are used: su_window opens the SDL3 shared library (libSDL3.so.0) at run time, as su_render opens
the Vulkan loader, so nothing links against SDL and headless builds never need it. The headers are those of the
oldest supported runtime (SDL 3.2), so the build cannot reach a function newer runtimes alone provide; SDL keeps
ABI compatibility within 3.x. The headers are fetched at bootstrap and not redistributed in this repository.
Decision record: docs/BACKENDS_AND_DEPENDENCIES.md (decision ledger). Stdlib only; subprocesses get argument lists.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = "https://github.com/libsdl-org/SDL.git"
TAG = "release-3.2.30"
COMMIT = "f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc"
DEST = os.path.join(ROOT, "third_party", "sdl3")
LOCK = os.path.join(ROOT, "third_party", "sdl3-headers.lock.json")
LICENSE = "LICENSE.txt"


def git(args):
    try:
        res = subprocess.run(["git", *args], shell=False, capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return 127, "", "git is not on PATH"
    return res.returncode, res.stdout.strip(), res.stderr.strip()


def license_digest(path):
    # Line endings normalized to LF: a checkout with core.autocrlf (Windows) must give the same digest.
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read().replace(b"\r\n", b"\n")).hexdigest()


def verify():
    rc, head, _ = git(["-C", DEST, "rev-parse", "HEAD"])
    if rc != 0 or head != COMMIT:
        return None, "checkout is not at %s (%s)" % (COMMIT, head or "no git checkout")
    lic = os.path.join(DEST, LICENSE)
    if not os.path.isfile(lic):
        return None, "license file %s missing" % LICENSE
    text = open(lic, "r", encoding="utf-8", errors="replace").read()
    if "This software is provided 'as-is'" not in text or "Sam Lantinga" not in text:
        return None, "license file is not the SDL zlib license"
    for name in ("SDL.h", "SDL_events.h", "SDL_video.h", "SDL_gamepad.h", "SDL_version.h"):
        if not os.path.isfile(os.path.join(DEST, "include", "SDL3", name)):
            return None, "include/SDL3/%s missing" % name
    return license_digest(lic), None


def clone():
    rc, _, errtext = git(["clone", "--depth", "1", "--filter=blob:none", "--sparse", "--branch", TAG,
                          "--config", "advice.detachedHead=false", URL, DEST])
    if rc != 0:
        return "git clone: " + errtext
    rc, _, errtext = git(["-C", DEST, "sparse-checkout", "set", "--no-cone", "/include/", "/" + LICENSE])
    if rc != 0:
        return "git sparse-checkout: " + errtext
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    if args.check:
        digest, err = verify()
        if err is not None:
            print("FAIL: " + err)
            return 1
        lock = json.load(open(LOCK, "r", encoding="utf-8"))
        if lock.get("commit") != COMMIT or lock.get("license_sha256") != digest:
            print("FAIL: lock file disagrees with the checkout")
            return 1
        print("sdl3-headers %s ok (%s)" % (TAG, COMMIT))
        return 0
    if os.path.isdir(DEST):
        digest, err = verify()
        if err is None:
            print("sdl3-headers %s already present" % TAG)
        else:
            print("replacing an unusable checkout: " + err)
            shutil.rmtree(DEST)
    if not os.path.isdir(DEST):
        err = clone()
        if err is not None:
            print("FAIL: " + err)
            return 1
    digest, err = verify()
    if err is not None:
        print("FAIL: " + err)
        return 1
    lock = {
        "name": "SDL3 headers",
        "origin": URL,
        "tag": TAG,
        "commit": COMMIT,
        "license": "Zlib",
        "license_file": LICENSE,
        "license_sha256": digest,
        "license_sha256_note": "sha256 of the license file with CRLF normalized to LF",
        "pinned_by": "tools/bootstrap_sdl3_headers.py",
        "use": "build-time headers of the optional Linux window backend (src/platform/window_sdl3.c); the "
               "SDL3 library (3.2.0 or later) is opened at run time; not redistributed",
    }
    with open(LOCK, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(lock, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("sdl3-headers %s at %s; license sha256 %s" % (TAG, COMMIT, digest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
