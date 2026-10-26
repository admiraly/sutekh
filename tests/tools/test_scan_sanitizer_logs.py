# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Regression tests for tools/scan_sanitizer_logs.py (stdlib unittest, no engine build)."""
from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import scan_sanitizer_logs as scan  # noqa: E402

LSAN_PTRACE_FORK = """==1211965==WARNING: TestPTrace() failed to fork (errno 1)
==1211965==LeakSanitizer has encountered a fatal error.
==1211965==HINT: For debugging, try setting environment variable LSAN_OPTIONS=verbosity=1:log_threads=1
==1211965==HINT: LeakSanitizer does not work under ptrace (strace, gdb, etc)
"""
LSAN_ATTACH = """==1382170==Could not attach to thread 1382160 (errno 1).
==1382170==Failed suspending threads.
==1382160==LeakSanitizer has encountered a fatal error.
==1382160==HINT: For debugging, try setting environment variable LSAN_OPTIONS=verbosity=1:log_threads=1
==1382160==HINT: LeakSanitizer does not work under ptrace (strace, gdb, etc)
"""
UAF = """=================================================================
==1159093==ERROR: AddressSanitizer: heap-use-after-free on address 0x7d00363e4d38 at pc 0x000000522cb8
READ of size 8 at 0x7d00363e4d38 thread T0
SUMMARY: AddressSanitizer: heap-use-after-free src/host/su_replay.c:696:61 in su_replay_open
"""
LEAK = """==1159056==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 2020 byte(s) in 1 object(s) allocated from:
SUMMARY: AddressSanitizer: 4040 byte(s) leaked in 2 allocation(s).
"""
UBSAN = "src/x.c:10:5: runtime error: signed integer overflow: 9223372036854775807 + 1 cannot be represented\n"


def run(files: dict[str, str]) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as d:
        for name, text in files.items():
            (Path(d) / name).write_text(text, encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = scan.main(["scan_sanitizer_logs.py", d])
        return rc, out.getvalue()


class Scan(unittest.TestCase):
    def test_empty_directory_passes(self):
        rc, out = run({})
        self.assertEqual(rc, 0)
        self.assertIn("0 log file(s)", out)

    def test_leak_check_not_run_is_counted_not_failed(self):
        rc, out = run({"san.1": LSAN_PTRACE_FORK, "san.2": LSAN_ATTACH})
        self.assertEqual(rc, 0)
        self.assertIn("2 leak check(s) not run", out)

    def test_asan_report_fails(self):
        rc, out = run({"san.1": LSAN_ATTACH, "san.2": UAF})
        self.assertEqual(rc, 1)
        self.assertIn("heap-use-after-free", out)

    def test_leak_report_fails(self):
        self.assertEqual(run({"san.1": LEAK})[0], 1)

    def test_ubsan_report_fails(self):
        self.assertEqual(run({"san.1": UBSAN})[0], 1)

    def test_fatal_error_with_other_output_fails(self):
        # A leak check that could not run is only accepted when the log holds nothing else.
        self.assertEqual(run({"san.1": LSAN_ATTACH + "something else\n"})[0], 1)

    def test_unrecognised_output_fails(self):
        self.assertEqual(run({"san.1": "==1==AddressSanitizer: something new\n"})[0], 1)

    def test_missing_directory_is_a_usage_error(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(scan.main(["scan_sanitizer_logs.py", str(ROOT / "no-such-directory")]), 2)
        self.assertIn("does not exist", err.getvalue())


if __name__ == "__main__":
    unittest.main()
