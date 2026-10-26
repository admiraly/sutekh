#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Fail when a sanitizer log directory holds any report (issue 148).

The sanitizer CI job runs ctest with ASAN_OPTIONS/UBSAN_OPTIONS log_path=<dir>/san, so every instrumented process
(tests, the sutekh supervisor, workers and disposable children whose exit status a test may not check) writes its
reports to <dir>/san.<pid>. Any AddressSanitizer, LeakSanitizer or UndefinedBehaviorSanitizer report fails.

LeakSanitizer cannot run in a process that may not ptrace (disposable children under the containment seccomp filter,
and workers with the self-sandbox on in the containment test). It then prints "LeakSanitizer has encountered a fatal
error" and nothing else; those logs are counted and listed as leak checks not run, not as passes or failures.

Usage: scan_sanitizer_logs.py <dir>
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPORT = re.compile(r'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:|'
                    r'WARNING: (AddressSanitizer|UndefinedBehaviorSanitizer)')
LSAN_NOT_RUN = 'LeakSanitizer has encountered a fatal error'
# The only lines a leak-check-not-run log may contain besides the fatal error (pids vary).
LSAN_NOT_RUN_LINES = re.compile(r'^==\d+==(LeakSanitizer has encountered a fatal error\.|HINT: .*|'
                                r'WARNING: TestPTrace\(\) failed to fork \(errno \d+\)|'
                                r'Could not attach to thread \d+ \(errno \d+\)\.|Failed suspending threads\.)$')


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    root = Path(argv[1])
    if not root.exists():
        print(f'scan_sanitizer_logs: log directory {root} does not exist; the sanitizer run did not create it '
              '(was ASAN_OPTIONS/UBSAN_OPTIONS log_path set, and the directory made before the run?)', file=sys.stderr)
        return 2
    if not root.is_dir():
        print(f'scan_sanitizer_logs: {root} is not a directory', file=sys.stderr)
        return 2
    logs = sorted(p for p in root.iterdir() if p.is_file())
    reports = []
    not_run = []
    for log in logs:
        text = log.read_text(encoding='utf-8', errors='replace')
        lines = [ln for ln in text.splitlines() if ln.strip()]
        if REPORT.search(text):
            reports.append((log, text))
        elif LSAN_NOT_RUN in text and all(LSAN_NOT_RUN_LINES.match(ln) for ln in lines):
            not_run.append(log)
        elif lines:
            # Unrecognised sanitizer output: show it and fail rather than guess.
            reports.append((log, text))
    print(f'scan_sanitizer_logs: {len(logs)} log file(s), {len(reports)} with a report, '
          f'{len(not_run)} leak check(s) not run (process may not ptrace)')
    for log, text in reports:
        print(f'---- {log} ----')
        print(text)
    return 1 if reports else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
