#!/usr/bin/env python3
"""Thin developer CLI for the native M0 skeleton (stdlib only).

Commands:
  configure --profile {dev,release}   run cmake configure into build/
  build     --profile {dev,release}   configure if needed, then cmake --build
  clean                               remove build/ (only that fixed path)

Every subprocess is started with an argument list and shell=False. Values
are validated against fixed allowlists before use. The exact commands and
tool versions are printed before each run.
"""

import argparse
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(REPO_ROOT, "build")
PROFILES = {"dev": "Debug", "release": "Release"}
GENERATOR_CANDIDATES = ("Ninja",)


def _run(argv, cwd=REPO_ROOT):
    """Print and run one command given as an argument list."""
    if not isinstance(argv, list) or not all(isinstance(a, str) for a in argv):
        raise TypeError("command must be a list of strings")
    print("+ " + " ".join(argv), flush=True)
    result = subprocess.run(argv, cwd=cwd, shell=False, check=False)
    if result.returncode != 0:
        print(f"error: command exited with {result.returncode}", file=sys.stderr)
    return result.returncode


def _tool_version(argv):
    try:
        out = subprocess.run(argv, shell=False, capture_output=True,
                             text=True, check=False)
        line = (out.stdout or out.stderr).strip().splitlines()
        return line[0] if line else "unknown"
    except OSError as exc:
        return f"unavailable ({exc.__class__.__name__})"


def _report_tools(generator):
    print("tool versions:", flush=True)
    print(f"  python: {sys.version.split()[0]}", flush=True)
    print(f"  cmake: {_tool_version(['cmake', '--version'])}", flush=True)
    if generator == "Ninja":
        print(f"  ninja: {_tool_version(['ninja', '--version'])}", flush=True)
    cc = os.environ.get("CC", "cc")
    if shutil.which(cc) is None:
        print(f"  compiler: {cc} not found on PATH", flush=True)
    else:
        print(f"  compiler: {_tool_version([cc, '--version'])}", flush=True)


def _choose_generator():
    for gen in GENERATOR_CANDIDATES:
        if gen == "Ninja" and shutil.which("ninja") is not None:
            return gen
    return None


def _configure(profile):
    build_type = PROFILES[profile]
    generator = _choose_generator()
    _report_tools(generator or "default")
    argv = ["cmake", "-S", REPO_ROOT, "-B", BUILD_DIR,
            f"-DCMAKE_BUILD_TYPE={build_type}"]
    if generator is not None:
        argv += ["-G", generator]
    return _run(argv)


def cmd_configure(args):
    return _configure(args.profile)


def cmd_build(args):
    rc = _configure(args.profile)
    if rc != 0:
        return rc
    return _run(["cmake", "--build", BUILD_DIR, "--parallel"])


def cmd_clean(_args):
    target = os.path.realpath(BUILD_DIR)
    if os.path.dirname(target) != os.path.realpath(REPO_ROOT) or \
            os.path.basename(target) != "build":
        print("error: refusing to clean unexpected path", file=sys.stderr)
        return 2
    if os.path.isdir(target):
        print(f"+ remove {target}", flush=True)
        shutil.rmtree(target)
    else:
        print("nothing to clean", flush=True)
    return 0


def build_parser():
    parser = argparse.ArgumentParser(prog="dev.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name, func in (("configure", cmd_configure), ("build", cmd_build)):
        p = sub.add_parser(name)
        p.add_argument("--profile", choices=sorted(PROFILES), default="dev")
        p.set_defaults(func=func)
    p = sub.add_parser("clean")
    p.set_defaults(func=cmd_clean)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
