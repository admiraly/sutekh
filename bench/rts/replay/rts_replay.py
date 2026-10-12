# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Replay recorder and replayer for `rts-replay/0.1` (contract M3 section 9.1, OD-17, M3-05).

The file is JSON Lines: a header, then command records and digest records. A replay re-runs the
header's scene and the command records against a backend, and compares each digest record with the
backend's digest at the same boundary. A backend is anything with the four methods of `Backend`.

The reference backend here is the Python semantic model (bench/rts/ref). Its digests are the model's
own encoding, not the engine's M2 digest, so byte equality with the engine is still an M3-06 check.
An engine backend (agent protocol client) plugs into the same interface once the engine methods
`world.set_fields` and `world.spawn_batch` exist; this module does not stub them.

Boundary semantics: a command with `t` = b is applied at boundary b, before tick b runs, in `seq`
order. A digest record with `t` = b is the state at boundary b after its commands are applied. The
final digest is at `t` = ticks.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REF = os.path.normpath(os.path.join(_HERE, "..", "ref"))
if _REF not in sys.path:
    sys.path.insert(0, _REF)

import spawn_batch_limits as limits  # noqa: E402
from digest import digest_record, state_digest  # noqa: E402
from model import FIELD_NAMES, World, tick, validate_spawn, ModelError  # noqa: E402

FORMAT = "rts-replay"
VERSION = "0.1"
NUMERIC_PROFILE = "rts_u32_v1"
CMD_SET_FIELDS = "world.set_fields"
CMD_SPAWN_BATCH = "world.spawn_batch"
CMD_DESPAWN = "world.despawn"
COMMANDS = (CMD_SET_FIELDS, CMD_SPAWN_BATCH, CMD_DESPAWN)
# Writable set of world.set_fields (contract 2.2). Every other field is refused.
WRITABLE_FIELDS = frozenset((
    "pos_x", "pos_y", "speed", "range_sq", "reload", "damage", "flight", "order_mode", "order_x",
    "order_y", "formation_slot", "formation_gen", "formation_ox", "formation_oy",
))
U32_MAX = 0xFFFFFFFF
NONE = 0xFFFFFFFF


class ReplayError(Exception):
    """Fail-closed refusal: the replay is not run, or a record is malformed."""

    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code = code
        self.detail = detail


def canonical_json(obj) -> str:
    """Sorted keys, no whitespace, decimal integers (9.2)."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


# ---------------------------------------------------------------- file format

def write_replay(path: str, header: dict, commands: list, digests: list) -> None:
    lines = [canonical_json(header)]
    lines += [canonical_json(c) for c in commands]
    lines += [canonical_json(d) for d in digests]
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_replay(path: str):
    """Parse and structurally validate a replay file. Returns (header, commands, digests)."""
    with open(path, "r", encoding="ascii") as f:
        raw = f.read().split("\n")
    if raw and raw[-1] == "":
        raw.pop()
    if not raw:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "empty replay file")
    recs = []
    for i, line in enumerate(raw, 1):
        try:
            obj = json.loads(line, object_pairs_hook=_no_dup_keys)
        except (ValueError, ReplayError) as exc:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"line {i}: {exc}") from None
        if canonical_json(obj) != line:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"line {i} is not canonical JSON")
        recs.append(obj)
    header, body = recs[0], recs[1:]
    validate_header(header)
    commands, digests = [], []
    last_t, last_seq = -1, -1
    for i, rec in enumerate(body, 2):
        if "state" in rec:
            digests.append(rec)
            continue
        if digests:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"line {i}: command after a digest record")
        t, seq = rec.get("t"), rec.get("seq")
        if not _is_u(t) or not _is_u(seq) or rec.get("cmd") not in COMMANDS:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"line {i}: bad command record")
        if t < last_t or (t == last_t and seq <= last_seq):
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"line {i}: commands out of (t, seq) order")
        if t >= header["ticks"]:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"line {i}: command after the last tick")
        last_t, last_seq = t, seq
        commands.append(rec)
    last_dt = -1
    for rec in digests:
        if not _is_u(rec["t"]) or rec["t"] <= last_dt or rec["t"] > header["ticks"]:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", "digest records out of order")
        last_dt = rec["t"]
    return header, commands, digests


def _no_dup_keys(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"duplicate key {k!r}")
        out[k] = v
    return out


def _is_u(v) -> bool:
    return type(v) is int and v >= 0


def validate_header(h: dict) -> None:
    """Fail closed on a header the replayer cannot honour (9.1)."""
    if h.get("format") != FORMAT or h.get("version") != VERSION:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "format or version")
    if h.get("numeric_profile") != NUMERIC_PROFILE:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "numeric_profile")
    caps = h.get("caps", {})
    # OD-3c: control-plane records are excluded from max_events_per_tick. True is a config error.
    if caps.get("control_events_counted") is not False:
        raise ReplayError("SU_E_INVALID_ARGUMENT", "caps.control_events_counted must be false")
    for key in ("ticks", "digest_every", "checkpoint_every"):
        if not _is_u(h.get(key)):
            raise ReplayError("SU_E_SCHEMA_MISMATCH", key)
    if h["digest_every"] < 1:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "digest_every must be at least 1")
    for key in ("schema_digest", "system_manifest_hash", "operator_manifest_hash", "scene"):
        if key not in h:
            raise ReplayError("SU_E_SCHEMA_MISMATCH", f"missing {key}")


# ---------------------------------------------------------------- backends

class Backend:
    """What the recorder and replayer need from an engine or a model."""

    def load_scene(self, scene: dict) -> None: ...

    def apply(self, cmd: dict) -> None: ...

    def step(self) -> None: ...

    def digest(self, t: int) -> dict: ...

    def scene_digest(self) -> str: ...

    @property
    def operator_manifest_hash(self) -> str: ...


def _model_operator_hash() -> str:
    # The model has no engine operator manifest. Its hash names the model and is not the engine's.
    return "sha256:" + hashlib.sha256(b"rts-model-operators/0.1 (not the engine manifest)").hexdigest()


class ModelBackend(Backend):
    """Reference backend: the Python semantic model. Digests are the model's encoding (digest.py)."""

    def __init__(self, capacity: int, event_log_max: int = 65536):
        self.world = World(capacity, event_log_max)

    def load_scene(self, scene: dict) -> None:
        for spec in scene_rows(scene):
            self.world.spawn(spec)

    def scene_digest(self) -> str:
        return state_digest(self.world)

    @property
    def operator_manifest_hash(self) -> str:
        return _model_operator_hash()

    def apply(self, cmd: dict) -> None:
        name, args = cmd["cmd"], cmd["args"]
        w = self.world
        if name == CMD_SPAWN_BATCH:
            rows = args.get("rows")
            if not isinstance(rows, list):
                raise ModelError("SU_E_TYPE", "rows")
            try:
                limits.check_row_count(len(rows))
            except limits.BatchRefused as exc:
                raise ModelError(exc.code, "rows", exc.detail)
            # Batch atomicity (D3): validate every row, then prechecks, then mutate.
            specs = []
            for r in rows:
                if not isinstance(r, dict) or not isinstance(r.get("values"), dict):
                    raise ModelError("SU_E_TYPE", "rows", "each row needs values")
                specs.append(validate_spawn(r["values"], w.cap))
            try:
                limits.check_prechecks(len(rows), w.cap, sum(w.alive), w.hw)
            except limits.BatchRefused as exc:
                raise ModelError(exc.code, "world", exc.detail)
            for r in rows:
                w.spawn(r["values"])
        elif name == CMD_DESPAWN:
            w.despawn(_handle(args.get("entity")))
        elif name == CMD_SET_FIELDS:
            _set_fields(w, args)
        else:  # pragma: no cover - read_replay rejects unknown commands first
            raise ModelError("SU_E_UNSUPPORTED_METHOD", name)

    def step(self) -> None:
        tick(self.world)

    def digest(self, t: int) -> dict:
        return digest_record(self.world, t)


def _handle(v) -> int:
    if type(v) is not int or not (0 <= v <= 0xFFFFFFFFFFFFFFFF):
        raise ModelError("SU_E_TYPE", "entity")
    return v


# rts_u32_v1 value domains (contract 2.2); mirrors su_rts_ctl_check_value in src/runtime/rts/control_domains.c.
# formation_slot's capacity bound and the formation/kind rules are resulting-row checks, not per-field.
FIELD_DOMAINS = {
    **{n: (lambda v: v <= 32767) for n in ("pos_x", "pos_y", "order_x", "order_y", "formation_ox", "formation_oy")},
    "speed": lambda v: v <= 1024,
    "range_sq": lambda v: v == 0 or 1 <= v <= 262144,
    "reload": lambda v: 1 <= v <= 1024,
    "damage": lambda v: 1 <= v <= 65535,
    "flight": lambda v: 1 <= v <= 64,
    "order_mode": lambda v: v <= 2,
}


def _set_fields(w: World, args: dict) -> None:
    ents = args.get("entities")
    if not isinstance(ents, list) or not (1 <= len(ents) <= limits.ROWS_MAX):
        raise ModelError("SU_E_TYPE", "entities")
    seen, plans = set(), []
    for e in ents:
        handle = _handle(e.get("entity"))
        if handle in seen:
            raise ModelError("SU_E_INVALID_ARGUMENT", "duplicate entity in one call")
        seen.add(handle)
        slot, gen = handle & U32_MAX, handle >> 32
        if slot >= w.cap or not w.alive[slot] or w.gen[slot] != gen:
            raise ModelError("SU_E_STALE_HANDLE", "entity")
        values = e.get("values")
        if not isinstance(values, dict) or not values:
            raise ModelError("SU_E_TYPE", "values")
        row = w.row(slot)
        for name, v in values.items():
            if name not in FIELD_NAMES:
                raise ModelError("SU_E_SCHEMA_MISMATCH", name)
            if name not in WRITABLE_FIELDS:
                raise ModelError("SU_E_TYPE", name, "not writable by set_fields")
            if type(v) is not int or not (0 <= v <= U32_MAX):
                raise ModelError("SU_E_TYPE", name, "u32 integer")
            if name in FIELD_DOMAINS and not FIELD_DOMAINS[name](v):
                raise ModelError("SU_E_TYPE", name, "outside the rts_u32_v1 domain (2.2)")
            row[name] = v
        # Resulting-row rules (D2), on the committed row with the payload applied.
        if (row["formation_slot"] == NONE) != (row["formation_gen"] == 0):
            raise ModelError("SU_E_TYPE", "formation_gen", "formation_gen = 0 iff slot NONE")
        if row["kind"] == 2 and row["range_sq"] != 0:
            raise ModelError("SU_E_TYPE", "range_sq", "must be 0 on kind 2")
        plans.append((slot, values))
    for slot, values in plans:
        for name, v in values.items():
            w.cols[name][slot] = v


def scene_rows(scene: dict) -> list:
    """Regenerate the initial scene from `generator`, `params` and `seed` (9.1, N12).

    Only generators this module knows are accepted. Unknown names fail closed."""
    gen = scene.get("generator")
    if gen != "rts-units-splitmix64/0.1":
        raise ReplayError("SU_E_SCHEMA_MISMATCH", f"unknown scene generator {gen!r}")
    params, seed = scene.get("params", {}), scene.get("seed")
    n = params.get("units")
    if not _is_u(seed) or not _is_u(n) or n < 1 or n > scene["capacity"]:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "scene seed or units")
    rng = _SplitMix64(seed)
    rows = []
    for i in range(n):
        rows.append({
            "kind": 1, "team": i & 1, "hp": 1 + rng.below(100),
            "pos_x": rng.below(512 * 64), "pos_y": rng.below(512 * 64),
            "speed": rng.below(8), "range_sq": 0, "reload": 1 + rng.below(4),
            "damage": 1 + rng.below(5), "flight": 1 + rng.below(8),
        })
    return rows


class _SplitMix64:
    """SplitMix64 (the constants of the M0 PRNG); platform independent, unlike random.Random."""

    def __init__(self, seed: int):
        self.s = seed & 0xFFFFFFFFFFFFFFFF

    def next(self) -> int:
        self.s = (self.s + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return z ^ (z >> 31)

    def below(self, n: int) -> int:
        return self.next() % n


def scene_header(generator: str, params: dict, seed: int, capacity: int, scene_digest: str) -> dict:
    return {"generator": generator, "params": params, "seed": seed, "capacity": capacity,
            "scene_digest": scene_digest}


# ---------------------------------------------------------------- recorder

class Recorder:
    """Runs a backend, records each command at its boundary and digests every `digest_every` ticks."""

    def __init__(self, backend: Backend, scene: dict, ticks: int, digest_every: int,
                 checkpoint_every: int = 32, caps: dict = None, engine: dict = None):
        self.backend = backend
        self.scene = scene
        self.ticks = ticks
        self.digest_every = digest_every
        self.checkpoint_every = checkpoint_every
        self.caps = caps or {"max_struct_per_tick": backend.world.cap,
                             "max_events_per_tick": backend.world.cap,
                             "event_log_max": backend.world.event_log_max,
                             "control_events_counted": False}
        self.engine = engine or {"artifact_hashes": [], "source_revision": "model"}
        self.commands: list = []
        self.digests: list = []
        self._seq = 0
        self._pending: dict = {}  # boundary t -> commands, applied at that boundary
        self.backend.load_scene(scene)
        self.scene_digest = self.backend.scene_digest()  # taken before any command or tick

    def command(self, t: int, name: str, args: dict) -> None:
        if name not in COMMANDS:
            raise ReplayError("SU_E_UNSUPPORTED_METHOD", name)
        if not (0 <= t < self.ticks):
            raise ReplayError("SU_E_INVALID_ARGUMENT", "t outside 0..ticks-1")
        rec = {"t": t, "seq": self._seq, "cmd": name, "args": args}
        self._seq += 1
        self._pending.setdefault(t, []).append(rec)

    def run(self) -> None:
        for t in range(self.ticks + 1):
            for rec in self._pending.pop(t, []):
                self.backend.apply(rec)
                self.commands.append(rec)
            if t % self.digest_every == 0 or t == self.ticks:
                self.digests.append(self.backend.digest(t))
            if t < self.ticks:
                self.backend.step()

    def header(self) -> dict:
        return {
            "format": FORMAT, "version": VERSION, "numeric_profile": NUMERIC_PROFILE,
            "scene": scene_header(self.scene["generator"], self.scene.get("params", {}),
                                  self.scene["seed"], self.scene["capacity"], self.scene_digest),
            "schema_digest": "sha256:" + "0" * 64,  # model has no engine schema descriptor
            "system_manifest_hash": "sha256:" + "0" * 64,
            "operator_manifest_hash": self.backend.operator_manifest_hash,
            "engine": self.engine, "caps": self.caps,
            "spool": {"checkpoint_retain": 0, "spool_bytes_max": 0},
            "checkpoint_every": self.checkpoint_every, "digest_every": self.digest_every,
            "ticks": self.ticks,
        }

    def write(self, path: str) -> None:
        write_replay(path, self.header(), self.commands, self.digests)


# ---------------------------------------------------------------- replayer

class Divergence:
    def __init__(self, t: int, field: str, expected, actual):
        self.t, self.field, self.expected, self.actual = t, field, expected, actual

    def __str__(self) -> str:
        return f"first divergence at tick {self.t}: {self.field}: expected {self.expected}, actual {self.actual}"


def replay(path: str, backend: Backend) -> dict:
    """Validate the file against the backend, re-run it, compare digests.

    Returns {"ok": bool, "divergence": Divergence | None, "digests_checked": n}. Raises ReplayError
    on a header or record the replayer refuses to run. A missing digest record is a mismatch."""
    header, commands, digests = read_replay(path)
    if backend.operator_manifest_hash != header["operator_manifest_hash"]:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "operator_manifest_hash differs; fails closed")
    scene = header["scene"]
    if (backend.world.cap if hasattr(backend, "world") else None) != scene["capacity"]:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "capacity differs from the scene header")
    backend.load_scene({"generator": scene["generator"], "params": scene["params"],
                        "seed": scene["seed"], "capacity": scene["capacity"]})
    if backend.scene_digest() != scene["scene_digest"]:
        raise ReplayError("SU_E_SCHEMA_MISMATCH", "scene_digest differs; the scene is not reproduced")
    by_t: dict = {}
    for c in commands:
        by_t.setdefault(c["t"], []).append(c)
    want = {d["t"]: d for d in digests}
    checked = 0
    for t in range(header["ticks"] + 1):
        for c in by_t.get(t, []):
            try:
                backend.apply(c)
            except ModelError as exc:
                return {"ok": False, "digests_checked": checked,
                        "divergence": Divergence(t, f"command seq {c['seq']}", "applied", exc.code)}
        if t in want:
            got = backend.digest(t)
            checked += 1
            diff = _first_difference(want[t], got)
            if diff:
                return {"ok": False, "digests_checked": checked,
                        "divergence": Divergence(t, *diff)}
        if t < header["ticks"]:
            backend.step()
    required = [t for t in range(header["ticks"] + 1)
                if t % header["digest_every"] == 0 or t == header["ticks"]]
    missing = [t for t in required if t not in want]
    if missing:
        return {"ok": False, "digests_checked": checked,
                "divergence": Divergence(missing[0], "missing digest record", "present", "absent")}
    return {"ok": True, "digests_checked": checked, "divergence": None}


def _first_difference(expected: dict, actual: dict):
    for key in ("state", "events", "counts"):
        if expected.get(key) != actual.get(key):
            return key, expected.get(key), actual.get(key)
    return None


def main(argv: list) -> int:
    """`replay FILE` against the model backend. Exit status is nonzero on any mismatch or refusal."""
    if len(argv) != 2 or argv[0] != "replay":
        print("usage: rts_replay.py replay FILE", file=sys.stderr)
        return 2
    header, _, _ = read_replay(argv[1])
    backend = ModelBackend(header["scene"]["capacity"], header["caps"]["event_log_max"])
    try:
        result = replay(argv[1], backend)
    except ReplayError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 1
    if not result["ok"]:
        print(f"mismatch: {result['divergence']}", file=sys.stderr)
        return 1
    print(f"ok: {result['digests_checked']} digest records match")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
