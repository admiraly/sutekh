# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Exact integer square root (contract M3 section 2.1 and 4.1(c)1), standard library only.

Newton iteration on Python integers. No floating point is used, so the result is exact for every
non-negative integer. Used for R = isqrt(range_sq) in the targeting query.
"""
from __future__ import annotations


def isqrt(n: int) -> int:
    """Return floor(sqrt(n)) for an integer n >= 0."""
    if type(n) is not int or n < 0:
        raise ValueError("isqrt requires a non-negative int")
    if n < 2:
        return n
    x = 1 << ((n.bit_length() + 1) // 2)  # starts at or above sqrt(n)
    while True:
        y = (x + n // x) // 2
        if y >= x:
            return x
        x = y
