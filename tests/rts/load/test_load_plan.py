# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Chunking for the 1,000,000-row load (T-RTS-LOAD-1 (e)) and the request bound (d)."""
import os
import sys
import unittest

_BENCH = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "bench", "rts", "load"))
if _BENCH not in sys.path:
    sys.path.insert(0, _BENCH)

import load_plan as lp  # noqa: E402


class LoadChunks(unittest.TestCase):
    def test_million_rows_is_977_calls_last_short(self):  # (e)
        sizes = lp.chunk_sizes(1000000)
        self.assertEqual(len(sizes), 977)
        self.assertEqual(sizes[-1], 1000000 - 976 * 1024)
        self.assertTrue(all(s == 1024 for s in sizes[:-1]))
        self.assertEqual(sum(sizes), 1000000)

    def test_exact_multiple_has_no_short_call(self):
        self.assertEqual(lp.chunk_sizes(2048), [1024, 1024])

    def test_empty_load_has_no_calls(self):
        self.assertEqual(lp.chunk_sizes(0), [])

    def test_rejects_bad_totals(self):
        for bad in (-1, 1.5, True):
            with self.assertRaises(ValueError):
                lp.chunk_sizes(bad)

    def test_full_chunk_fits_the_request_cap(self):  # (d)
        self.assertEqual(lp.worst_request_bytes(1024), 948225)
        self.assertLess(lp.worst_request_bytes(1024), lp.REQUEST_BYTES_MAX)


if __name__ == "__main__":
    unittest.main()
