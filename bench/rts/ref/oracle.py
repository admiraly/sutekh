# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Exact targeting oracle and the targeting metrics of contract M3 section 4.2 (standard library only).

The oracle scans every indexed enemy (alive, kind 1, hp > 0, other team), not the grid. For each
querier it returns the exact (dist_sq, slot) minimum among qualifying enemies, or None.

Metrics over a sample of queriers, for the spatial output of the model at the same tick-start state:
  missed                 oracle has a qualifying enemy, the budgeted query has target_valid = 0
  valid_but_not_nearest  target_valid = 1 and the target is not the exact (dist_sq, slot) minimum
  ratio                  sqrt(target_dist_sq) / sqrt(oracle_dist_sq) over the not-nearest cases
Rates use the queriers with an oracle-qualifying enemy as the denominator (primary). The denominator
choice is an open question recorded in the handoff; the all-queriers and valid-only denominators are
reported too. Ratios are metric-only floats, never simulation state.

Gates (4.2) are applied only to a committed scene; the caller sets gates_applicable accordingly.
"""
from __future__ import annotations

import math

from model import KIND_UNIT, World, spatial_phase
from prng import SplitMix64

GATE_MISSED = 0.001
GATE_VNN = 0.02
GATE_RATIO_P95 = 3.0
THIN_MARGIN_VNN = 0.015


def armed_units(world: World) -> list:
    return [s for s in range(world.cap)
            if world.alive[s] and world.cols["kind"][s] == KIND_UNIT and world.cols["range_sq"][s] > 0]


def sample_queriers(world: World, k: int, seed: int) -> list:
    """k armed units without replacement by a seeded partial Fisher-Yates over splitmix64_v1.
    If there are at most k armed units, all of them are used. Returned in ascending slot order."""
    cands = armed_units(world)
    if len(cands) <= k:
        return cands
    rng = SplitMix64(seed)
    idx = list(range(len(cands)))
    for i in range(k):
        j = i + rng.uniform(len(idx) - i)
        idx[i], idx[j] = idx[j], idx[i]
    return sorted(cands[i] for i in idx[:k])


def oracle_best(world: World, r: int, enemies_by_team: dict = None):
    """Exact nearest qualifying enemy of querier r: (dist_sq, slot) or None."""
    cols = world.cols
    x, y = cols["pos_x"][r], cols["pos_y"][r]
    range_sq = cols["range_sq"][r]
    my_team = cols["team"][r]
    if enemies_by_team is None:
        enemies_by_team = indexed_enemies(world)
    best = None
    for e in enemies_by_team.get(1 - my_team, ()):
        if e == r:
            continue
        dx = abs(cols["pos_x"][e] - x)
        dy = abs(cols["pos_y"][e] - y)
        d = dx * dx + dy * dy
        if d <= range_sq:
            cand = (d, e)
            if best is None or cand < best:
                best = cand
    return best


def indexed_enemies(world: World) -> dict:
    """Alive kind-1 rows with hp > 0, grouped by team (ascending slot). The oracle's universe."""
    out = {0: [], 1: []}
    for s in range(world.cap):
        if world.alive[s] and world.cols["kind"][s] == KIND_UNIT and world.cols["hp"][s] > 0:
            out[world.cols["team"][s]].append(s)
    return out


def _percentile(values: list, q: float):
    """Nearest-rank percentile (q in (0, 1]). Empty input gives None."""
    if not values:
        return None
    ordered = sorted(values)
    return ordered[math.ceil(q * len(ordered)) - 1]


def targeting_metrics(world: World, queriers: list, scene_source: str) -> dict:
    """Run the model's spatial phase on the current state, then compare each querier to the oracle."""
    spatial_phase(world)
    enemies = indexed_enemies(world)
    cols = world.cols
    oracle_positive = 0
    missed = 0
    valid = 0
    vnn = 0
    truncated = 0
    ratios = []
    inf_count = 0
    for r in queriers:
        truncated += cols["truncated"][r]
        exact = oracle_best(world, r, enemies)
        tv = cols["target_valid"][r] == 1
        if exact is not None:
            oracle_positive += 1
        if tv:
            valid += 1
            assert exact is not None, "budgeted query found a target the oracle does not see"
            tslot = cols["target_slot"][r]
            tdist = cols["target_dist_sq"][r]
            if (tdist, tslot) != exact:
                vnn += 1
                od = exact[0]
                if od == 0:
                    if tdist == 0:
                        ratios.append(1.0)
                    else:
                        inf_count += 1
                        ratios.append(float("inf"))
                else:
                    ratios.append(math.sqrt(tdist) / math.sqrt(od))
        elif exact is not None:
            missed += 1
    n = len(queriers)
    def rate(a, b):
        return (a / b) if b else None

    p95 = _percentile(ratios, 0.95)
    p50 = _percentile(ratios, 0.50)
    result = {
        "scene_source": scene_source,
        "gates_applicable": False,
        "queriers": n,
        "oracle_positive": oracle_positive,
        "target_valid": valid,
        "missed": missed,
        "valid_but_not_nearest": vnn,
        "truncated_queriers": truncated,
        "missed_rate": rate(missed, oracle_positive),
        "valid_but_not_nearest_rate": rate(vnn, oracle_positive),
        "valid_but_not_nearest_rate_over_valid": rate(vnn, valid),
        "missed_rate_over_all_queriers": rate(missed, n),
        "ratio_count": len(ratios),
        "ratio_p50": p50,
        "ratio_p95": p95,
        "ratio_inf_count": inf_count,
        "thin_margin_point_estimate": (rate(vnn, oracle_positive) or 0.0) > THIN_MARGIN_VNN,
    }
    if result["gates_applicable"]:
        result["gates"] = gate_verdicts(result)
    return result


def gate_verdicts(m: dict) -> dict:
    """4.2 gates. Only meaningful on the committed congested.blob.v1 scene (M3-01 owns it)."""
    missed_ok = (m["missed_rate"] or 0.0) <= GATE_MISSED
    vnn_ok = (m["valid_but_not_nearest_rate"] or 0.0) <= GATE_VNN
    p95 = m["ratio_p95"]
    ratio_ok = p95 is None or p95 <= GATE_RATIO_P95
    return {"missed": missed_ok, "valid_but_not_nearest": vnn_ok, "ratio_p95": ratio_ok,
            "pass": missed_ok and vnn_ok and ratio_ok}
