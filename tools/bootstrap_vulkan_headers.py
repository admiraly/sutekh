#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Fetch and verify the pinned Khronos Vulkan-Headers checkout used to build the optional renderer (su_render).

Modes:
  (default)   shallow-clone the pinned tag into third_party/vulkan-headers/ (gitignored), verify the commit and the
              license file, and write third_party/vulkan-headers.lock.json
  --check     verify an existing checkout offline against the lock file

The headers are fetched at bootstrap and not redistributed in this repository (as for AsmJit). Without them, or a
Vulkan SDK (VULKAN_SDK), CMake skips the renderer; headless builds never need Vulkan.
Stdlib only; every subprocess gets an argument list (shell=False).
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = "https://github.com/KhronosGroup/Vulkan-Headers.git"
TAG = "v1.4.365"
COMMIT = "c46850864f4661461b0f6cb9922c058ffea4915e"
DEST = os.path.join(ROOT, "third_party", "vulkan-headers")
LOCK = os.path.join(ROOT, "third_party", "vulkan-headers.lock.json")
LICENSE = "LICENSE.md"


def git(args):
    res = subprocess.run(["git", *args], shell=False, capture_output=True, text=True, check=False)
    return res.returncode, res.stdout.strip(), res.stderr.strip()


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def verify():
    rc, head, _ = git(["-C", DEST, "rev-parse", "HEAD"])
    if rc != 0 or head != COMMIT:
        return None, "checkout is not at %s (%s)" % (COMMIT, head or "no git checkout")
    lic = os.path.join(DEST, LICENSE)
    if not os.path.isfile(lic):
        return None, "license file %s missing" % LICENSE
    text = open(lic, "r", encoding="utf-8", errors="replace").read()
    if "Apache-2.0" not in text or "MIT" not in text:
        return None, "license file does not name Apache-2.0 and MIT"
    if not os.path.isfile(os.path.join(DEST, "include", "vulkan", "vulkan_core.h")):
        return None, "include/vulkan/vulkan_core.h missing"
    return sha256(lic), None


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
        print("vulkan-headers %s ok (%s)" % (TAG, COMMIT))
        return 0
    if os.path.isdir(DEST):
        digest, err = verify()
        if err is None:
            print("vulkan-headers %s already present" % TAG)
        else:
            print("replacing an unusable checkout: " + err)
            shutil.rmtree(DEST)
    if not os.path.isdir(DEST):
        rc, _, errtext = git(["clone", "--depth", "1", "--branch", TAG, "--config", "advice.detachedHead=false", URL,
                              DEST])
        if rc != 0:
            print("FAIL: git clone: " + errtext)
            return 1
    digest, err = verify()
    if err is not None:
        print("FAIL: " + err)
        return 1
    lock = {
        "name": "Vulkan-Headers",
        "origin": URL,
        "tag": TAG,
        "commit": COMMIT,
        "license": "Apache-2.0 OR MIT",
        "license_file": LICENSE,
        "license_sha256": digest,
        "pinned_by": "tools/bootstrap_vulkan_headers.py",
        "use": "build-time headers of the optional renderer (src/render); not redistributed",
    }
    with open(LOCK, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(lock, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("vulkan-headers %s at %s; license sha256 %s" % (TAG, COMMIT, digest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
