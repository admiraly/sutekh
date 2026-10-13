# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""T-RTS-LOAD-1 (a), (b), (c) and (g) through a real `sutekh serve` (contract M3 10.3, OD-3b).

Every case goes through the agent protocol, never a direct worker call (tests/rts/load/PLAN.md). The world is
the full six-system RTS world (rts_session.SCHEDULE). Refusals are checked by revision, alive rows and the
event log; the accepted 1,024-row batch is also checked across a worker kill: the recovered world must equal a
control session that ran the same calls (one recovery-log entry replayed).

Usage: python3 -I tests/rts/load/test_load_engine.py <path to sutekh>
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "bench", "rts", "replay"))

import rts_session as rs  # noqa: E402

SPAWNED = 0xFFFFFFFE  # SU_EVENT_SPAWNED
CAPACITY = 2048

checks = 0
failures = 0


def check(cond: bool, what: str) -> None:
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def inspect(s: rs.Session, wid: str) -> dict:
    return s.call("world.inspect", {"world_id": wid, "limit": 1})


def batch_line(s: rs.Session, wid: str, rev: str, rows: list) -> bytes:
    return s.line("world.spawn_batch", {"world_id": wid, "expected_world_revision": rev, "rows": rows})


def pad_to(line: bytes, size: int) -> bytes:
    """Inserts spaces before the final brace so the line is exactly size bytes (JSON whitespace)."""
    assert line.endswith(b"}") and len(line) <= size
    return line[:-1] + b" " * (size - len(line)) + b"}"


def error_status(resp: dict) -> str:
    return resp.get("error", {}).get("code", "") if "error" in resp else ""


def max_rows(n: int, formation_slot: int | None = None) -> list:
    return [rs.max_row(formation_slot) for _ in range(n)]


def case_a_and_recovery(binary: str) -> None:
    """(a) accepted near-maximum 1,024-row request; one revision bump; 1,024 SPAWNED records; recovery.

    Slots 0..1023 are filled first (rows with formation_slot NONE), so the near-maximum rows can name live slot
    1023 with formation_gen 4294967295 (Q2: a formation_slot must name a live row)."""
    ctl = rs.Session(binary)
    rec = rs.Session(binary)
    try:
        wid, rev = ctl.create_rts(CAPACITY, event_log_max=4096)
        wid_r, rev_r = rec.create_rts(CAPACITY, event_log_max=4096)
        check((wid, rev) == (wid_r, rev_r), "both sessions create the same world identity")
        for s, w_rev in ((ctl, rev), (rec, rev_r)):
            r = s.call("world.spawn_batch", {"world_id": wid, "expected_world_revision": w_rev,
                                             "rows": max_rows(1024)})
            check(r["world_revision"] == f"{wid}.r2", "fill batch: slots 0..1023")
        rev = rev_r = f"{wid}.r2"
        rows = max_rows(1024, 1023)
        line = batch_line(ctl, wid, rev, rows)
        check(len(line) <= rs.REQUEST_BYTES_MAX, f"near-maximum request fits 1 MiB ({len(line)} bytes)")
        params_rows = len(line) - len(batch_line(ctl, wid, rev, []))
        check(params_rows <= 948225 + 2, f"1,024 maximum rows are at most 948,225 bytes ({params_rows})")
        for s, w_rev in ((ctl, rev), (rec, rev_r)):
            resp = s.raw(batch_line(s, wid, w_rev, rows))
            check("result" in resp, f"near-maximum batch accepted: {error_status(resp)} {resp.get('error')}")
            if "result" not in resp:
                return
            r = resp["result"]
            check(r["spawned"] == 1024 and len(r["entities"]) == 1024, "1,024 rows spawned")
            check(r["world_revision"] == f"{wid}.r3", f"one revision bump ({r['world_revision']})")
        ev, dropped = ctl.all_events(wid)
        spawned = [e for e in ev if e["kind"] == SPAWNED]
        check(len(spawned) == 2048 and dropped == 0, f"1,024 SU_EVENT_SPAWNED records per batch ({len(spawned)}, {dropped})")
        check([e["slot"] for e in spawned] == list(range(2048)), "rows take slots 0..2047 in order")

        # Recovery: the batch is one logged entry. No snapshot is taken before the kill (it would checkpoint).
        old = rec.kill_worker()
        resp = rec.raw(rec.line("world.inspect", {"world_id": wid, "limit": 1}))
        check(error_status(resp) == "SU_E_WORKER_FAILED", "first call after the kill reports the worker failure")
        recovery = resp.get("error", {}).get("recovery") or resp.get("recovery") or {}
        check(recovery.get("recovered") is True and recovery.get("replaced_pid_old") == old,
              f"world recovered in a replacement worker: {recovery}")
        rev2 = f"{wid}.r3"
        check(ctl.snapshot_sha(wid, rev2) == rec.snapshot_sha(wid, rev2), "recovered digest equals the control")
        check(ctl.all_events(wid) == rec.all_events(wid), "recovered event log equals the control")
        step = {"world_id": wid, "expected_world_revision": rev2, "ticks": 2}
        a = ctl.call("world.step", dict(step))
        b = rec.call("world.step", dict(step))
        check(a["world_revision"] == b["world_revision"], "both sessions step to the same revision")
        rev3 = a["world_revision"]
        check(ctl.snapshot_sha(wid, rev3) == rec.snapshot_sha(wid, rev3), "digests agree after two ticks")
    finally:
        ctl.close()
        rec.close()


def case_b_c_g(binary: str) -> None:
    s = rs.Session(binary)
    try:
        wid, rev = s.create_rts(CAPACITY)
        before = inspect(s, wid)

        # (b) one byte over the cap: refused by the size check, no state change.
        line = pad_to(batch_line(s, wid, rev, max_rows(1)), rs.REQUEST_BYTES_MAX + 1)
        check(len(line) == rs.REQUEST_BYTES_MAX + 1, "padded request is 1,048,577 bytes")
        resp = s.raw(line)
        check(error_status(resp) == "SU_E_RESOURCE_LIMIT", f"oversize request refused: {resp.get('error')}")
        check(inspect(s, wid) == before, "oversize refusal changes nothing")

        # (c) exactly the cap: passes the size check; this request is valid, so it is accepted.
        line = pad_to(batch_line(s, wid, rev, max_rows(1024)), rs.REQUEST_BYTES_MAX)
        check(len(line) == rs.REQUEST_BYTES_MAX, "boundary request is 1,048,576 bytes")
        resp = s.raw(line)
        check("result" in resp and resp["result"]["spawned"] == 1024,
              f"1,048,576-byte request accepted: {resp.get('error')}")
        rev = resp.get("result", {}).get("world_revision", rev)

        # (c) a refusal at the boundary is a domain code, never the size code.
        bad = max_rows(1024)
        bad[-1]["values"]["flight"] = 65
        line = pad_to(batch_line(s, wid, rev, bad), rs.REQUEST_BYTES_MAX)
        resp = s.raw(line)
        check(error_status(resp) == "SU_E_TYPE", f"boundary request with a bad last row is SU_E_TYPE: {resp}")

        # (g) the last row over free capacity: 1,024 free, 1,025 rows is over the row cap, so use 1,024 rows on
        # a world with 1,023 free slots.
        one = s.call("world.spawn_batch", {"world_id": wid, "expected_world_revision": rev, "rows": max_rows(1)})
        rev = one["world_revision"]
        before = inspect(s, wid)
        ev_before = s.all_events(wid)
        resp = s.raw(batch_line(s, wid, rev, max_rows(1024)))
        check(error_status(resp) == "SU_E_RESOURCE_LIMIT", f"1,024 rows into 1,023 free slots refused: {resp}")
        check(inspect(s, wid) == before and s.all_events(wid) == ev_before,
              "capacity refusal takes no slot, appends no record, keeps the revision")
        # (g) a domain-invalid last row.
        bad = max_rows(3)
        bad[-1]["values"]["reload"] = 0
        resp = s.raw(batch_line(s, wid, rev, bad))
        check(error_status(resp) == "SU_E_TYPE", f"domain-invalid last row refused: {resp}")
        check(inspect(s, wid) == before and s.all_events(wid) == ev_before, "domain refusal changes nothing")
    finally:
        s.close()


def spool_need(retain: int, hw_after: int, log_max: int) -> int:
    """Contract M3 12: bytes the spool must hold for retain + 1 checkpoints of a world with hw_after rows."""
    return (retain + 1) * (446 * hw_after + 90 * log_max) + 16384


def case_spool_precheck(binary: str) -> None:
    """Contract 12 spool precheck: with spool_bytes_max exactly the bound for 1,024 rows, a 1,024-row batch is
    admitted and one more row is refused with SU_E_RESOURCE_LIMIT and no state change. A replay is not
    prechecked: the recovered world equals a control session."""
    probe = rs.Session(binary)
    try:
        retain = probe.call("engine.info", {})["recovery"]["limits"]["checkpoint_retain"]
    finally:
        probe.close()
    limit = spool_need(retain, 1024, 1)
    env = {"SUTEKH_SPOOL_BYTES_MAX": str(limit)}
    s = rs.Session(binary, env)
    try:
        info = s.call("engine.info", {})
        got = info["recovery"]["limits"]["spool_bytes_max"]
        check(got == limit, f"supervisor runs with spool_bytes_max {limit}: {got}")
        wid, rev = s.create_rts(CAPACITY, event_log_max=1)
        r = s.call("world.spawn_batch", {"world_id": wid, "expected_world_revision": rev, "rows": max_rows(1024)})
        check(r["spawned"] == 1024, "1,024 rows admitted at the spool bound")
        rev = r["world_revision"]
        before = inspect(s, wid)
        resp = s.raw(batch_line(s, wid, rev, max_rows(1)))
        check(error_status(resp) == "SU_E_RESOURCE_LIMIT" and "spool" in resp["error"]["message"],
              f"row 1,025 refused by the spool precheck: {resp.get('error')}")
        resp = s.raw(s.line("world.spawn", {"world_id": wid, "expected_world_revision": rev,
                                            "values": rs.max_row(0)["values"]}))
        check(error_status(resp) == "SU_E_RESOURCE_LIMIT", f"world.spawn refused by the spool precheck: {resp}")
        check(inspect(s, wid) == before, "spool refusals change nothing")
    finally:
        s.close()


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    case_a_and_recovery(binary)
    case_b_c_g(binary)
    case_spool_precheck(binary)
    print(f"test_load_engine: {checks} checks, {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
