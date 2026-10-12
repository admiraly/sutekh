# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Puts the reference modules and the shared checks on sys.path. Imported first by each test module,
because python3 -I does not add the script directory to sys.path."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REF = os.path.normpath(os.path.join(_HERE, "..", "..", "..", "bench", "rts", "ref"))
for _p in (_REF, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)
