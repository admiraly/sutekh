# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Seeded probe scene for M3-02 measurements. NOT the committed congested.blob.v1 (M3-01 owns the
normative generator, D10). Its layout follows the 7.3 and 7.4 prose: units uniform in a central
2048 x 2048 box, two teams interleaved by group, groups of 17 (one anchor and 16 members with 4x4
offsets), the weapon table of 7.4, and orders on the anchor and on the remainder units only.

Details the prose leaves open and this probe fixes: anchor speed 96, anchor orders to the box
centre, team = group index mod 2, weapon kind drawn uniformly. Every result from this scene is
labelled scene_source = "m3-02-probe" and is not a gate result.
"""
from __future__ import annotations

from model import BIAS, KIND_ANCHOR, KIND_UNIT, NONE, ORDER_ATTACK_MOVE, World
from prng import SplitMix64

DOMAIN = 32768
BOX_LO = 16384 - 1024
BOX_SIDE = 2048
CENTRE = 16384

# 7.4 weapon table (synthetic)
WEAPONS = (
    {"name": "rifle", "range_sq": 65536, "reload": 6, "flight": 2, "damage": 3, "speed": 96, "hp": 12},
    {"name": "bow", "range_sq": 262144, "reload": 10, "flight": 5, "damage": 5, "speed": 64, "hp": 10},
    {"name": "melee", "range_sq": 256, "reload": 2, "flight": 1, "damage": 2, "speed": 128, "hp": 20},
)
ANCHOR_SPEED = 96


def probe_capacity(n_rows: int) -> int:
    return n_rows


def build_probe(n_rows: int, seed: int, event_log_max: int = 65536) -> World:
    """Build the probe world with n_rows rows (spawned in slot order, so slot = spawn index)."""
    if n_rows < 1:
        raise ValueError("n_rows must be positive")
    world = World(probe_capacity(n_rows), event_log_max=event_log_max)
    rng = SplitMix64(seed)
    group_size = 17
    grouped = n_rows - (n_rows % group_size)
    for i in range(n_rows):
        x = BOX_LO + rng.uniform(BOX_SIDE)
        y = BOX_LO + rng.uniform(BOX_SIDE)
        if i < grouped:
            g, j = divmod(i, group_size)
            team = g % 2
            if j == 0:
                world.spawn({"kind": KIND_ANCHOR, "team": team, "hp": 0, "pos_x": x, "pos_y": y,
                             "speed": ANCHOR_SPEED, "range_sq": 0, "reload": 1, "damage": 1,
                             "flight": 1, "order_mode": ORDER_ATTACK_MOVE,
                             "order_x": CENTRE, "order_y": CENTRE})
                continue
            anchor_slot = i - j  # the anchor of group g was spawned at slot g * 17
            w = WEAPONS[rng.uniform(len(WEAPONS))]
            col, row = (j - 1) % 4, (j - 1) // 4
            world.spawn({"kind": KIND_UNIT, "team": team, "hp": w["hp"], "pos_x": x, "pos_y": y,
                         "speed": w["speed"], "range_sq": w["range_sq"], "reload": w["reload"],
                         "damage": w["damage"], "flight": w["flight"], "order_mode": 0,
                         "formation_slot": anchor_slot, "formation_gen": world.gen[anchor_slot],
                         "formation_ox": col * 32 - 48 + BIAS, "formation_oy": row * 32 - 48 + BIAS})
        else:
            team = i % 2
            w = WEAPONS[rng.uniform(len(WEAPONS))]
            world.spawn({"kind": KIND_UNIT, "team": team, "hp": w["hp"], "pos_x": x, "pos_y": y,
                         "speed": w["speed"], "range_sq": w["range_sq"], "reload": w["reload"],
                         "damage": w["damage"], "flight": w["flight"],
                         "order_mode": ORDER_ATTACK_MOVE, "order_x": CENTRE, "order_y": CENTRE,
                         "formation_slot": NONE})
    return world
