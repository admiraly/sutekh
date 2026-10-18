# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Regression: the ir02 benchmark client must keep reading when serve writes much to stderr.

A fake `serve` writes 1 MiB to stderr per request (far above the 64 KiB pipe buffer). Before the
stderr drain, the client blocked on the full pipe and never got the reply.
"""
from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bench"))

import ir02_entities_bench as bench  # noqa: E402

FAKE_SERVE = '''import json, sys
for line in sys.stdin:
    req = json.loads(line)
    sys.stderr.write("x" * (1 << 20) + "\\n")
    sys.stderr.flush()
    print(json.dumps({"ok": True, "id": req["id"], "result": {}}), flush=True)
'''


class StderrDrain(unittest.TestCase):
    def test_client_survives_heavy_stderr(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "fake_serve.py"
            script.write_text(FAKE_SERVE, encoding="utf-8", newline="\n")
            binary = Path(tmp) / "fake_serve"
            binary.write_text(f'#!/bin/sh\nexec "{sys.executable}" -I "{script}"\n', encoding="utf-8", newline="\n")
            binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
            client = bench.Client(str(binary), dict(os.environ))
            try:
                for _ in range(5):
                    self.assertEqual(client.ok("world.step", {}), {})
            finally:
                client.close()


if __name__ == "__main__":
    unittest.main()
