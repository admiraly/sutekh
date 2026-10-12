#!/usr/bin/env python3
"""Known-answer tests for splitmix64_v1 (contract 7.1). Run: python3 -I tests/rts/gen/test_prng.py

Source of the vectors: an independent C17 program (not this repository's code) implementing
S. Vigna's public-domain splitmix64 reference (x += 0x9e3779b97f4a7c15; two multiply-xorshift
rounds with 0xbf58476d1ce4e5b9 and 0x94d049bb133111eb; return z ^ (z >> 31)). The program was
is committed as tests/rts/gen/splitmix64_ref.c (build: gcc -std=c17 -Wall -Wextra -O0 -o ref splitmix64_ref.c;
gcc 13.3 was used). Its output is pinned here. The seed-0 sequence
e220a8397b1dcdaf, 6e789e6aa1b965f4, ... is the widely published splitmix64 seed-0 sequence.
The uniform(n) vectors are (u32 * n) >> 32 applied to the C output u32 = e220a839.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "..", "bench", "rts", "gen")))
import rtsgen  # noqa: E402

KAT_U64 = {
    0x0000000000000000: ["e220a8397b1dcdaf", "6e789e6aa1b965f4", "06c45d188009454f", "f88bb8a8724c81ec"],
    0x0000000000000001: ["910a2dec89025cc1", "beeb8da1658eec67", "f893a2eefb32555e", "71c18690ee42c90b"],
    0x0123456789ABCDEF: ["157a3807a48faa9d", "d573529b34a1d093", "2f90b72e996dccbe", "a2d419334c4667ec"],
}


class PrngKnownAnswers(unittest.TestCase):
    def test_u64_sequences(self):
        for seed, want in KAT_U64.items():
            r = rtsgen.SplitMix64(seed)
            got = ["%016x" % r.next_u64() for _ in want]
            self.assertEqual(got, want, "seed %016x" % seed)

    def test_u32_and_uniform_seed0(self):
        r = rtsgen.SplitMix64(0)
        self.assertEqual("%08x" % r.next_u32(), "e220a839")
        self.assertEqual(rtsgen.SplitMix64(0).uniform(1024), 904)  # 0x388, top 10 bits
        self.assertEqual(rtsgen.SplitMix64(0).uniform(3), 2)
        self.assertEqual(rtsgen.SplitMix64(0).uniform(17), 15)

    def test_uniform_range(self):
        r = rtsgen.SplitMix64(0x5EED)
        vals = [r.uniform(7) for _ in range(2000)]
        self.assertTrue(all(0 <= v < 7 for v in vals))
        self.assertEqual(len(set(vals)), 7)

    def test_seed_refusals(self):
        for bad in (-1, 1 << 64, 1.0, True, "0"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                rtsgen.SplitMix64(bad)
        rtsgen.SplitMix64((1 << 64) - 1)  # largest u64 is accepted

    def test_high_seed_bits_are_used(self):
        # A seed and its low 32 bits must not give the same stream (seed-handling mutant).
        hi = 0x1000000000000001
        self.assertNotEqual(rtsgen.SplitMix64(hi).next_u64(), rtsgen.SplitMix64(1).next_u64())

    def test_uniform_rejects_bad_n(self):
        for bad in (0, -3, 2.0, True):
            with self.assertRaises(ValueError, msg=repr(bad)):
                rtsgen.SplitMix64(1).uniform(bad)


if __name__ == "__main__":
    unittest.main(verbosity=2)
