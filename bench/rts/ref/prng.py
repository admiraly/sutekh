# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""splitmix64_v1 (contract M3 section 7.1), standard library only.

State is u64. next(): state += 0x9E3779B97F4A7C15; z = state; two multiply-xorshift rounds; the
returned u32 is z >> 32. uniform(n) = (u32 * n) >> 32 (multiply-high, bias below n / 2**32).
This is used only for seeded probe scenes and querier sampling in the M3-02 reference. It is not
the committed scene generator (M3-01 owns generators, D10).
"""
from __future__ import annotations

MASK64 = (1 << 64) - 1
GAMMA = 0x9E3779B97F4A7C15


class SplitMix64:
    def __init__(self, seed: int):
        if type(seed) is not int or not (0 <= seed <= MASK64):
            raise ValueError("seed must be a u64 integer")
        self.state = seed

    def next_u64(self) -> int:
        self.state = (self.state + GAMMA) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)

    def next_u32(self) -> int:
        return self.next_u64() >> 32

    def uniform(self, n: int) -> int:
        """Return an integer in [0, n) by multiply-high of a u32 draw."""
        if type(n) is not int or not (1 <= n <= 1 << 32):
            raise ValueError("uniform(n) requires 1 <= n <= 2**32")
        return (self.next_u32() * n) >> 32
