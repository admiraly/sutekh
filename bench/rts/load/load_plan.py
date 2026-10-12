# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Chunking for a scene load through world.spawn_batch (contract T-RTS-LOAD-1 (e), OD-3b).

Each call carries at most ROWS_MAX rows. The last call may be short. Pure arithmetic: no engine call.
"""
from __future__ import annotations

ROWS_MAX = 1024  # OD-3b
ROW_BYTES_MAX = 925  # contract 10.3: one maximum row without the separator
REQUEST_BYTES_MAX = 1048576


def chunk_sizes(total_rows: int) -> list:
    """Row count of each spawn_batch call for a load of total_rows, in order."""
    if type(total_rows) is not int or total_rows < 0:
        raise ValueError("total_rows must be a non-negative integer")
    full, rest = divmod(total_rows, ROWS_MAX)
    sizes = [ROWS_MAX] * full
    if rest:
        sizes.append(rest)
    return sizes


def worst_request_bytes(rows: int) -> int:
    """Upper bound on the request line for rows maximum-form rows: rows * 925 + (rows - 1) + 2 brackets."""
    if rows < 1:
        return 2
    return ROW_BYTES_MAX * rows + (rows - 1) + 2
