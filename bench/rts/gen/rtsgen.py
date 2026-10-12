#!/usr/bin/env python3
"""Normative RTS scene generator (M3-01).

Source of truth: planning/contracts/M3_RTS_CONTRACT.md r4, sections 2, 2.1, 2.2, 7.1-7.5 and
table 10.3 (T-RTS-LOAD-1 byte bound). The only scene generator in M3 (D10).

Python standard library only. Run as: python3 -I bench/rts/gen/rtsgen.py {digest,lock,emit}

Conventions that the contract leaves open are marked CONVENTION below and are listed in the
M3-01 handoff. Any change to them changes the committed digests.
"""
import argparse
import array
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
PROFILE_PATH = os.path.join(REPO, "profiles", "rts", "schema", "rts_u32_v1.json")
LOCK_PATH = os.path.join(REPO, "bench", "rts", "scenes.lock.json")

GENERATOR_VERSION = "0.1"
CANON_FORMAT = "rts-scene-canon/0.1"
LOCK_FORMAT = "rts-scene-lock/0.1"
PRNG_NAME = "splitmix64_v1"
NUMERIC_PROFILE = "rts_u32_v1"

# ---- 7.1 PRNG constants (splitmix64 reference, Vigna) -----------------------------------------
GOLDEN = 0x9E3779B97F4A7C15
MIX1 = 0xBF58476D1CE4E5B9
MIX2 = 0x94D049BB133111EB
MASK64 = (1 << 64) - 1

# ---- 2 layout ----------------------------------------------------------------------------------
FIELD_NAMES = (
    "kind", "team", "hp", "pos_x", "pos_y", "speed", "range_sq", "reload", "cooldown", "damage",
    "flight", "order_mode", "order_x", "order_y", "formation_slot", "formation_gen", "formation_ox",
    "formation_oy", "goal_x", "goal_y", "anchor_x", "anchor_y", "anchor_valid", "target_slot",
    "target_gen", "target_dist_sq", "target_valid", "target_x", "target_y", "truncated",
    "proj_active", "proj_left", "proj_target", "proj_target_gen", "proj_damage", "dmg_out",
    "dmg_target", "dmg_target_gen", "incoming",
)
FIELD_COUNT = 39
IX = {name: i for i, name in enumerate(FIELD_NAMES)}  # 0-based column index; field id = IX + 1
NONE = 0xFFFFFFFF
BIAS = 16384
COORD_MAX = 32767
CENTRE = 16384  # domain centre used for free-unit and attack-move orders
RANGE_SQ_MAX = 262144
SPEED_MAX = 1024
RELOAD_MAX = 1024
DAMAGE_MAX = 65535
FLIGHT_MAX = 64
HP_MAX = 65535
CHUNK_ROWS = 1024  # OD-3b: spawn_batch rows per call
REQUEST_BYTES_MAX = 1048576
MAX_ROW_JSON_BYTES = 925  # 10.3 "maximum row bytes", computed and asserted by the tests
MAX_BATCH_JSON_BYTES = MAX_ROW_JSON_BYTES * CHUNK_ROWS + (CHUNK_ROWS - 1) + 2  # 948,225

# ---- 7.3 / 7.4 generator conventions ---------------------------------------------------------
GROUP_SIZE = 17  # 1 anchor + 16 members
MEMBERS = 16
FORMATION_COLS = 4
FORMATION_PITCH = 32
FORMATION_ORIGIN = -48  # member offset = origin + col * pitch (col = i mod 4), stored + BIAS
ANCHOR_IDLE = {"reload": 1, "damage": 1, "flight": 1, "range_sq": 0, "hp": 0}  # 7.3
ORDER_ATTACK_MOVE = 2
REMAINDER_ORDER_MODE = 2  # 7.3: remainder rows are free units with their own attack-move order
KINDS = ("rifle", "bow", "melee")  # 7.4 order; a weapon index is drawn with uniform(3)
WEAPONS = {  # 7.4 (synthetic, not BFME; generator convention)
    "rifle": {"range_sq": 65536, "reload": 6, "flight": 2, "damage": 3, "speed": 96, "hp": 12},
    "bow": {"range_sq": 262144, "reload": 10, "flight": 5, "damage": 5, "speed": 64, "hp": 10},
    "melee": {"range_sq": 256, "reload": 2, "flight": 1, "damage": 2, "speed": 128, "hp": 20},
}
MIXED_BANDS = ((1024, 8191), (24576, 31743))  # 7.3, team 0 and team 1 x ranges (inclusive)
CONGESTED_BOX = (15360, 17407)  # 2048 x 2048 box centred on 16384 (inclusive bounds)
SPARSE_TEAM1_ONE_IN = 8  # CONVENTION: team 1 with probability 1/8 (uniform(8) == 0)

# ---- scene specifications committed in bench/rts/scenes.lock.json ---------------------------
SCENE_SPECS = (
    {"name": "duel16", "generator": "duel.fixture.v1", "rows": 16, "seed": 0},
    {"name": "mixed-1024", "generator": "mixed.battle.v1", "rows": 1024, "seed": 0x0123456789ABCDEF},
    {"name": "mixed-16384", "generator": "mixed.battle.v1", "rows": 16384, "seed": 0xFEDCBA9876543210},
    {"name": "congested-1024", "generator": "congested.blob.v1", "rows": 1024, "seed": 0x5EED00000000C0DE},
    {"name": "congested-16384", "generator": "congested.blob.v1", "rows": 16384, "seed": 0xC0FFEE0000000001},
    {"name": "sparse-16384", "generator": "sparse.field.v1", "rows": 16384, "seed": 0x0BADC0DE0000ABCD},
    {"name": "sparse-65536", "generator": "sparse.field.v1", "rows": 65536, "seed": 0x1000000000000001},
)


# ===========================================================================================
# 7.1 PRNG
# ===========================================================================================
class SplitMix64:
    """splitmix64_v1 (7.1). State and seed are u64."""

    def __init__(self, seed):
        if isinstance(seed, bool) or not isinstance(seed, int) or not (0 <= seed <= MASK64):
            raise ValueError("seed must be an integer in [0, 2**64 - 1]")
        self.state = seed

    def next_u64(self):
        self.state = (self.state + GOLDEN) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * MIX1) & MASK64
        z = ((z ^ (z >> 27)) * MIX2) & MASK64
        return z ^ (z >> 31)

    def next_u32(self):
        return self.next_u64() >> 32

    def uniform(self, n):
        """uniform(n) = (u32 * n) >> 32, in [0, n-1]."""
        if isinstance(n, bool) or not isinstance(n, int) or n < 1:
            raise ValueError("uniform(n) needs an integer n >= 1")
        return (self.next_u32() * n) >> 32


# ===========================================================================================
# Profile (profiles/rts/schema/rts_u32_v1.json)
# ===========================================================================================
def canon_json(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def schema_digest(fields):
    """M2-01 section 6: hash over the sorted (id, type, name, default) list.

    CONVENTION: the byte encoding is canonical JSON of that list (the contract names the tuple,
    not the bytes). Recorded in the handoff as an encoding choice.
    """
    tup = [[f["id"], f["type"], f["name"], f["default"]] for f in sorted(fields, key=lambda f: f["id"])]
    return hashlib.sha256(canon_json(tup)).hexdigest()


class Profile:
    def __init__(self, doc):
        if doc.get("profile") != NUMERIC_PROFILE:
            raise ValueError("profile name must be %s" % NUMERIC_PROFILE)
        fields = sorted(doc["fields"], key=lambda f: f["id"])
        if [f["id"] for f in fields] != list(range(1, FIELD_COUNT + 1)):
            raise ValueError("field ids must be exactly 1..%d" % FIELD_COUNT)
        names = tuple(f["name"] for f in fields)
        if names != FIELD_NAMES:
            raise ValueError("profile field names/order differ from the 39-field layout (section 2)")
        for f in fields:
            if f["type"] != "u32" or f["default"] != 0:
                raise ValueError("field %s: type must be u32 and default 0" % f["name"])
        self.doc = doc
        self.fields = fields
        self.by_name = {f["name"]: f for f in fields}
        self.schema_digest = schema_digest(fields)


def load_profile(path=PROFILE_PATH):
    with open(path, "r", encoding="utf-8") as fh:
        return Profile(json.load(fh))


# ===========================================================================================
# Rows and scenes
# ===========================================================================================
def _blank():
    v = [0] * FIELD_COUNT
    for name in ("formation_slot", "target_slot", "dmg_target", "proj_target"):
        v[IX[name]] = NONE  # 2.2 spawn defaults (D1)
    return v


def _put(v, **kw):
    for name, val in kw.items():
        v[IX[name]] = val
    return v


def _anchor_row(team, kind, x, y, order_mode, order_x, order_y):
    v = _blank()
    _put(v, kind=2, team=team, pos_x=x, pos_y=y, speed=WEAPONS[kind]["speed"],
         order_mode=order_mode, order_x=order_x, order_y=order_y,
         formation_slot=NONE, formation_gen=0, formation_ox=0, formation_oy=0)
    _put(v, **ANCHOR_IDLE)
    return v


def _unit_row(team, kind, x, y, order_mode=0, order_x=0, order_y=0, formation=None):
    w = WEAPONS[kind]
    v = _blank()
    _put(v, kind=1, team=team, hp=w["hp"], pos_x=x, pos_y=y, speed=w["speed"],
         range_sq=w["range_sq"], reload=w["reload"], damage=w["damage"], flight=w["flight"],
         order_mode=order_mode, order_x=order_x, order_y=order_y)
    if formation is not None:
        anchor_slot, ox, oy = formation
        _put(v, formation_slot=anchor_slot, formation_gen=1, formation_ox=ox, formation_oy=oy)
    return v


def _member_offset(i):
    """7.3: member i has column i mod 4 and row i div 4; stored offset + BIAS."""
    col, row = i % FORMATION_COLS, i // FORMATION_COLS
    return (BIAS + FORMATION_ORIGIN + col * FORMATION_PITCH,
            BIAS + FORMATION_ORIGIN + row * FORMATION_PITCH)


def _finish(rows):
    arr = array.array("I")
    for r in rows:
        if len(r) != FIELD_COUNT:
            raise AssertionError("row has wrong width")
        arr.extend(r)
    return arr


# ---- duel.fixture.v1 (10.2): hand-specified, no PRNG -------------------------------------------
def gen_duel(rows_n, seed):
    if rows_n != 16:
        raise ValueError("duel.fixture.v1 is defined for 16 rows in this revision")
    rows = [
        _unit_row(0, "rifle", 1000, 1000),                       # 0 shooter: exact-range pair
        _unit_row(1, "rifle", 1256, 1000),                       # 1 dx = 256: dist^2 == range_sq
        _unit_row(0, "rifle", 0, 16384, order_mode=1, order_x=32767, order_y=16384),  # 2 x = 0, move
        _unit_row(1, "rifle", 32767, 16384),                     # 3 x = 32767
        _unit_row(0, "bow", 10000, 10000),                       # 4 bow shooter, equal-distance pair
        _unit_row(1, "melee", 10000, 10300),                     # 5 dist^2 90000 (lower slot)
        _unit_row(1, "melee", 10300, 10000),                     # 6 dist^2 90000 (higher slot)
        _unit_row(0, "rifle", 20000, 20000),                     # 7 shooter A for hp-3 target
        _unit_row(0, "rifle", 20000, 19900),                     # 8 shooter B for hp-3 target
        _put(_unit_row(1, "rifle", 20000, 20100), hp=3),         # 9 hp 3, hit by 7 and 8
        _anchor_row(0, "rifle", 30000, 5000, 0, 0, 0),           # 10 formation anchor, no order
        _unit_row(0, "rifle", 30100, 5000, formation=(10, BIAS, BIAS)),  # 11 member, gen 1
        _unit_row(1, "melee", 100, 100),                         # 12 filler
        _unit_row(1, "melee", 32700, 100),                       # 13 filler
        _unit_row(0, "melee", 100, 32700),                       # 14 filler
        _unit_row(1, "melee", 32700, 32700),                     # 15 filler
    ]
    return _finish(rows), {"layout": "duel16.v1", "uses_prng": False}


# ---- mixed.battle.v1 (7.3) --------------------------------------------------------------------
def _band_centre(team):
    lo, hi = MIXED_BANDS[1 - team]  # attack-move target is the opposite band (half-open centre)
    return (lo + hi + 1) // 2, CENTRE


def _band_xy(rng, team):
    lo, hi = MIXED_BANDS[team]
    x = lo + rng.uniform(hi - lo + 1)
    y = rng.uniform(COORD_MAX + 1)
    return x, y


def gen_mixed(rows_n, seed):
    rng = SplitMix64(seed)
    groups, rem = divmod(rows_n, GROUP_SIZE)
    rows = []
    for _g in range(groups):
        team = rng.uniform(2)  # CONVENTION: draw order per group is team, kind, anchor x/y, members
        kind = KINDS[rng.uniform(len(KINDS))]
        base = len(rows)
        ax, ay = _band_xy(rng, team)
        ox, oy = _band_centre(team)
        rows.append(_anchor_row(team, kind, ax, ay, ORDER_ATTACK_MOVE, ox, oy))
        for i in range(MEMBERS):
            x, y = _band_xy(rng, team)
            rows.append(_unit_row(team, kind, x, y, formation=(base, *_member_offset(i))))
    for _j in range(rem):
        team = rng.uniform(2)
        kind = KINDS[rng.uniform(len(KINDS))]
        x, y = _band_xy(rng, team)
        ox, oy = _band_centre(team)
        rows.append(_unit_row(team, kind, x, y, REMAINDER_ORDER_MODE, ox, oy))
    return _finish(rows), {
        "bands": {"team0_x": list(MIXED_BANDS[0]), "team1_x": list(MIXED_BANDS[1])},
        "group_size": GROUP_SIZE,
        "members": MEMBERS,
        "formation": {"cols": FORMATION_COLS, "pitch": FORMATION_PITCH, "origin": FORMATION_ORIGIN},
        "remainder_order_mode": REMAINDER_ORDER_MODE,
        "weapons": WEAPONS,
        "kinds": list(KINDS),
        "orders": "anchor and remainder attack-move to the opposite band centre; members order_mode 0",
        "uses_prng": True,
    }


# ---- congested.blob.v1 (7.3) ------------------------------------------------------------------
def _box_xy(rng):
    lo, hi = CONGESTED_BOX
    return lo + rng.uniform(hi - lo + 1), lo + rng.uniform(hi - lo + 1)


def gen_congested(rows_n, seed):
    rng = SplitMix64(seed)
    groups, rem = divmod(rows_n, GROUP_SIZE)
    rows = []
    for g in range(groups):
        team = g % 2  # interleaved by group (CONVENTION)
        kind = KINDS[rng.uniform(len(KINDS))]
        base = len(rows)
        ax, ay = _box_xy(rng)
        rows.append(_anchor_row(team, kind, ax, ay, ORDER_ATTACK_MOVE, CENTRE, CENTRE))
        for i in range(MEMBERS):
            x, y = _box_xy(rng)
            rows.append(_unit_row(team, kind, x, y, formation=(base, *_member_offset(i))))
    for j in range(rem):
        slot = groups * GROUP_SIZE + j
        team = slot % 2  # interleaved by slot (CONVENTION)
        kind = KINDS[rng.uniform(len(KINDS))]
        x, y = _box_xy(rng)
        rows.append(_unit_row(team, kind, x, y, REMAINDER_ORDER_MODE, CENTRE, CENTRE))
    return _finish(rows), {
        "box": {"x": list(CONGESTED_BOX), "y": list(CONGESTED_BOX), "centre": [CENTRE, CENTRE]},
        "teams": "interleaved: group parity for groups, slot parity for remainder",
        "group_size": GROUP_SIZE,
        "members": MEMBERS,
        "formation": {"cols": FORMATION_COLS, "pitch": FORMATION_PITCH, "origin": FORMATION_ORIGIN},
        "remainder_order_mode": REMAINDER_ORDER_MODE,
        "weapons": WEAPONS,
        "kinds": list(KINDS),
        "orders": "anchor and remainder attack-move to the box centre; members order_mode 0",
        "uses_prng": True,
    }


# ---- sparse.field.v1 (7.3) --------------------------------------------------------------------
def gen_sparse(rows_n, seed):
    rng = SplitMix64(seed)
    rows = []
    for _s in range(rows_n):
        team = 1 if rng.uniform(SPARSE_TEAM1_ONE_IN) == 0 else 0  # CONVENTION (sparse teams)
        kind = KINDS[rng.uniform(len(KINDS))]
        x = rng.uniform(COORD_MAX + 1)
        y = rng.uniform(COORD_MAX + 1)
        rows.append(_unit_row(team, kind, x, y, ORDER_ATTACK_MOVE, CENTRE, CENTRE))
    return _finish(rows), {
        "uniform_domain": [0, COORD_MAX],
        "team1_one_in": SPARSE_TEAM1_ONE_IN,
        "formations": "none",
        "weapons": WEAPONS,
        "kinds": list(KINDS),
        "orders": "attack-move to the domain centre",
        "uses_prng": True,
    }


GENERATORS = {
    "duel.fixture.v1": gen_duel,
    "mixed.battle.v1": gen_mixed,
    "congested.blob.v1": gen_congested,
    "sparse.field.v1": gen_sparse,
}


# ===========================================================================================
# 7.2 canonical encoding and digest
# ===========================================================================================
def column_order(profile):
    """Columns are written in field-id order (7.2)."""
    return sorted(profile.by_name[n]["id"] for n in FIELD_NAMES)


def row_bytes(profile, arr):
    """Row-major little-endian u32 bytes, 39 columns per row, in column_order()."""
    n = len(arr) // FIELD_COUNT
    order = column_order(profile)
    out = array.array("I", bytes(4 * n * FIELD_COUNT))
    for pos, fid in enumerate(order):
        out[pos::FIELD_COUNT] = arr[(fid - 1)::FIELD_COUNT]
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def canon_header(generator, params, seed, digest_schema, capacity, rows):
    obj = {
        "format": CANON_FORMAT,
        "generator": generator,
        "params": params,
        "seed": seed,
        "schema_digest": "sha256:" + digest_schema,
        "capacity": capacity,
        "rows": rows,
    }
    # CONVENTION: line 1 is the canonical JSON followed by one newline (LF).
    return canon_json(obj) + b"\n"


def scene_digest(profile, generator, params, seed, arr):
    n = len(arr) // FIELD_COUNT
    header = canon_header(generator, params, seed, profile.schema_digest, n, n)
    h = hashlib.sha256()
    h.update(header)
    h.update(row_bytes(profile, arr))
    return h.hexdigest()


def make_scene(profile, spec):
    """Returns (arr, params, digest) for a scene spec dict (name, generator, rows, seed)."""
    gen = GENERATORS.get(spec["generator"])
    if gen is None:
        raise ValueError("unknown generator %r" % spec["generator"])
    arr, params = gen(spec["rows"], spec["seed"])
    digest = scene_digest(profile, spec["generator"], params, spec["seed"], arr)
    return arr, params, digest


# ===========================================================================================
# Domain validation (2.2, 2.1, 7.3)
# ===========================================================================================
def validate_scene(profile, arr, capacity=None):
    """Checks every row against the 2.2 domains, the spawn-derived idle values (7.3) and the
    formation consistency rules. Raises ValueError with slot and field on the first violation."""
    n = len(arr) // FIELD_COUNT
    if capacity is None:
        capacity = n
    if len(arr) != n * FIELD_COUNT:
        raise ValueError("array length is not a multiple of 39")
    idle_zero = [IX[f["name"]] for f in profile.fields if f["domain"].get("idle") == 0]
    idle_none = [IX[f["name"]] for f in profile.fields if f["domain"].get("idle") == "NONE"]

    def fail(slot, name, msg):
        raise ValueError("slot %d field %s: %s" % (slot, name, msg))

    for slot in range(n):
        base = slot * FIELD_COUNT
        v = arr[base:base + FIELD_COUNT]
        d = dict(zip(FIELD_NAMES, v))
        if d["kind"] not in (1, 2):
            fail(slot, "kind", "must be 1 or 2")
        if d["team"] not in (0, 1):
            fail(slot, "team", "must be 0 or 1")
        if d["kind"] == 1:
            if not (1 <= d["hp"] <= HP_MAX):
                fail(slot, "hp", "unit hp outside 1..65535")
            if not (1 <= d["reload"] <= RELOAD_MAX):
                fail(slot, "reload", "outside 1..1024")
            if not (1 <= d["damage"] <= DAMAGE_MAX):
                fail(slot, "damage", "outside 1..65535")
            if not (1 <= d["flight"] <= FLIGHT_MAX):
                fail(slot, "flight", "outside 1..64")
            if not (d["range_sq"] == 0 or 1 <= d["range_sq"] <= RANGE_SQ_MAX):
                fail(slot, "range_sq", "must be 0 or 1..262144")
        else:
            if d["hp"] != 0:
                fail(slot, "hp", "anchor hp must be 0")
            if d["range_sq"] != 0:
                fail(slot, "range_sq", "anchor range_sq must be 0")
            for k, want in ANCHOR_IDLE.items():
                if k in ("hp", "range_sq"):
                    continue
                if d[k] != want:
                    fail(slot, k, "anchor idle value must be %d (7.3)" % want)
        if not (d["speed"] <= SPEED_MAX):
            fail(slot, "speed", "outside 0..1024")
        for name in ("pos_x", "pos_y", "order_x", "order_y", "formation_ox", "formation_oy"):
            if not (d[name] <= COORD_MAX):
                fail(slot, name, "outside 0..32767")
        if d["order_mode"] not in (0, 1, 2):
            fail(slot, "order_mode", "must be 0, 1 or 2")
        for i in idle_zero:
            if v[i] != 0:
                fail(slot, FIELD_NAMES[i], "system-owned idle value must be 0 at spawn")
        for i in idle_none:
            if v[i] != NONE:
                fail(slot, FIELD_NAMES[i], "idle value must be NONE at spawn")
        fs = d["formation_slot"]
        if fs == NONE:
            if d["formation_gen"] != 0:
                fail(slot, "formation_gen", "must be 0 when formation_slot is NONE")
        else:
            if fs >= capacity:
                fail(slot, "formation_slot", "outside 0..capacity-1")
            if d["formation_gen"] == 0:
                fail(slot, "formation_gen", "must be nonzero when formation_slot is set")
            a = fs * FIELD_COUNT
            if arr[a + IX["kind"]] != 2:
                fail(slot, "formation_slot", "does not name an anchor (kind 2) row")
            if arr[a + IX["team"]] != d["team"]:
                fail(slot, "formation_slot", "member and anchor teams differ")
    return True


# ===========================================================================================
# Loader-side JSON emitter (world.spawn_batch rows, OD-3b, 10.3)
# ===========================================================================================
def spawn_rows_json(arr, first, count):
    """Compact JSON array of spawn_batch rows: [{"values":{39 names},"external_id":slot}, ...]."""
    parts = []
    for slot in range(first, first + count):
        base = slot * FIELD_COUNT
        obj = {
            "values": {name: int(arr[base + i]) for i, name in enumerate(FIELD_NAMES)},
            "external_id": slot,
        }
        parts.append(json.dumps(obj, separators=(",", ":"), ensure_ascii=True))
    return "[" + ",".join(parts) + "]"


def emit_spawn_batches(arr, chunk=CHUNK_ROWS):
    """Yields (first_slot, count, rows_json) per call. Every call is at most 1,024 rows and at
    most 948,225 bytes of rows (10.3(d)); the rows array is ASCII, so len() is the byte count."""
    n = len(arr) // FIELD_COUNT
    if not (1 <= chunk <= CHUNK_ROWS):
        raise ValueError("chunk must be 1..1024")
    first = 0
    while first < n:
        count = min(chunk, n - first)
        text = spawn_rows_json(arr, first, count)
        if len(text) > MAX_BATCH_JSON_BYTES:
            raise AssertionError("batch exceeds the 948,225-byte bound")
        yield first, count, text
        first += count


# ===========================================================================================
# Lock
# ===========================================================================================
def build_lock(profile):
    scenes = []
    for spec in SCENE_SPECS:
        arr, params, digest = make_scene(profile, spec)
        validate_scene(profile, arr, capacity=spec["rows"])
        scenes.append({
            "name": spec["name"],
            "generator": spec["generator"],
            "params": params,
            "seed": spec["seed"],
            "capacity": spec["rows"],
            "rows": spec["rows"],
            "scene_digest": "sha256:" + digest,
            "schema_digest": "sha256:" + profile.schema_digest,
        })
    return {
        "format": LOCK_FORMAT,
        "generator": "bench/rts/gen/rtsgen.py",
        "generator_version": GENERATOR_VERSION,
        "numeric_profile": NUMERIC_PROFILE,
        "prng": PRNG_NAME,
        "canon_format": CANON_FORMAT,
        "profile_path": "profiles/rts/schema/rts_u32_v1.json",
        "schema_digest": "sha256:" + profile.schema_digest,
        "scenes": scenes,
    }


def lock_text(lock):
    return json.dumps(lock, sort_keys=True, indent=2, ensure_ascii=True) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("digest", help="print scene and schema digests for every committed scene")
    sub.add_parser("lock", help="write bench/rts/scenes.lock.json")
    em = sub.add_parser("emit", help="write spawn_batch row calls (JSON Lines) for one scene")
    em.add_argument("--scene", required=True)
    em.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    profile = load_profile()
    if args.cmd == "digest":
        for spec in SCENE_SPECS:
            _arr, _p, digest = make_scene(profile, spec)
            print("%s %s sha256:%s" % (spec["name"], spec["generator"], digest))
        print("schema sha256:%s" % profile.schema_digest)
        return 0
    if args.cmd == "lock":
        with open(LOCK_PATH, "w", encoding="utf-8") as fh:
            fh.write(lock_text(build_lock(profile)))
        print("wrote %s" % LOCK_PATH)
        return 0
    if args.cmd == "emit":
        spec = next((s for s in SCENE_SPECS if s["name"] == args.scene), None)
        if spec is None:
            raise SystemExit("unknown scene %r" % args.scene)
        arr, _p, _d = make_scene(profile, spec)
        with open(args.out, "w", encoding="ascii") as fh:
            for first, count, text in emit_spawn_batches(arr):
                fh.write(json.dumps({"first_slot": first, "count": count, "rows": json.loads(text)},
                                    separators=(",", ":")) + "\n")
        print("wrote %s" % args.out)
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
