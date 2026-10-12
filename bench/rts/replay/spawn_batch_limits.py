# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Pure admission rules for world.spawn_batch (contract M3 D3, OD-3b, N-c, 9.3).

These are the checks the engine handler must apply, in order, and that the Python model mirrors:
  1. the request line is at most REQUEST_BYTES_MAX bytes, checked before any row is parsed;
  2. the row count is 1..ROWS_MAX;
  3. every row is valid (the caller validates rows; this module does not parse them);
  4. rows fit the free slots, rows <= capacity - alive;
  5. the high-water bound holds, hw + rows <= HW_MAX (N-c, 838,860).
Nothing here mutates state. The engine applies these before its first su_world_spawn.
"""
from __future__ import annotations

REQUEST_BYTES_MAX = 1048576  # 1 MiB request cap (agent protocol)
ROWS_MAX = 1024  # OD-3b
HW_MAX = 838860  # N-c precheck bound


class BatchRefused(Exception):
    """A limit or precheck refused the batch. `code` is the protocol status name."""

    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code = code
        self.detail = detail


def check_request_size(request_line: bytes) -> None:
    """Byte-level check on the raw request line, excluding the newline, before row parsing."""
    if len(request_line) > REQUEST_BYTES_MAX:
        raise BatchRefused("SU_E_RESOURCE_LIMIT", "request exceeds 1 MiB; refused before parsing rows")


def check_row_count(rows: int) -> None:
    if not (1 <= rows <= ROWS_MAX):
        raise BatchRefused("SU_E_RESOURCE_LIMIT", f"spawn_batch takes 1 to {ROWS_MAX} rows")


def check_prechecks(rows: int, capacity: int, alive: int, high_water: int) -> None:
    """Free-slot and high-water prechecks (D3, N-c). Run before the first spawn."""
    if rows > capacity - alive:
        raise BatchRefused("SU_E_RESOURCE_LIMIT", "not enough free slots; world unchanged")
    if high_water + rows > HW_MAX:
        raise BatchRefused("SU_E_RESOURCE_LIMIT", "high water would pass the bound; world unchanged")
