# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Generator of the synthetic 6dof_racer_microcosm scene (docs/SPACE3D.md, bench/space3d/README.md).

Writes, deterministically (no randomness here; scene randomness is seeded in the C driver):
  bench/space3d/course.json                  closed course: 4 cubic Bezier segments (exact rationals)
  capsules/space3d_racer/move.{capsule,lir}.json   IR 0.3: collision response, pace target on the course,
                                                   steering, orientation integration, motion
  capsules/space3d_racer/race.{capsule,lir}.json   IR 0.3: gate progress, laps, collision counters, events

    python3 -I bench/space3d/gen_racer.py            write the files
    python3 -I bench/space3d/gen_racer.py --check    exit 1 when a committed file differs from the output
Everything is synthetic: an ellipse-like loop of about 41 km with a vertical wave, no external assets.
"""
import argparse
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# Course: quarter-ellipse cubic Beziers (a = 8000 m, b = 5000 m, handle k = 0.5523), height wave +-250 m.
A, B, H = 8000, 5000, 250
K = (5523, 10000)


def kq(v):  # k * v as an exact rational [num, den]
    return [K[0] * v, K[1]]


def r(v):
    return [v, 1]


SEGMENTS = [
    [[r(A), r(0), r(0)], [r(A), kq(B), r(0)], [kq(A), r(B), r(H)], [r(0), r(B), r(H)]],
    [[r(0), r(B), r(H)], [kq(-A), r(B), r(H)], [r(-A), kq(B), r(0)], [r(-A), r(0), r(0)]],
    [[r(-A), r(0), r(0)], [r(-A), kq(-B), r(0)], [kq(-A), r(-B), r(-H)], [r(0), r(-B), r(-H)]],
    [[r(0), r(-B), r(-H)], [kq(A), r(-B), r(-H)], [r(A), kq(-B), r(0)], [r(A), r(0), r(0)]],
]

DT = (1, 120)            # 120 Hz
LOOKAHEAD = (3, 200)     # pace target 0.015 course units ahead (about 155 m)
ACCEL_PER_TICK = (5, 1)  # |delta v| <= 5 m/s per tick (600 m/s^2)
TURN_GAIN = (4, 1)       # angular velocity = 4/s * (forward x desired)
FOLLOW_GAIN = (6, 1)     # desired speed = min(vmax, 6/s * distance to the pace target)
RESTITUTION = (13, 10)   # v -= 1.3 (v.n) n on an approaching contact
PUSH_OUT = (20, 1)       # + 20 m/s along n per tick while the contact started overlapped (toi = 0)
GATES = 16

# space3d schema fields (docs/SPACE3D.md section 2) and the game fields of this scene (IDs >= 100).
F = {
    "kind": (1, "u32"), "next_gate": (2, "u32"), "gate_index": (3, "u32"),
    "pos_x": (10, "q32"), "pos_y": (12, "q32"), "pos_z": (14, "q32"),
    "prev_x": (16, "q32"), "prev_y": (18, "q32"), "prev_z": (20, "q32"),
    "rot_w": (22, "q32"), "rot_x": (24, "q32"), "rot_y": (26, "q32"), "rot_z": (28, "q32"),
    "hit_count": (40, "u32"), "c0_slot": (42, "u32"), "c0_toi": (44, "q32"),
    "c0_nx": (46, "q32"), "c0_ny": (48, "q32"), "c0_nz": (50, "q32"),
    "gate_dir": (62, "u32"),
    "vel_x": (100, "q32"), "vel_y": (102, "q32"), "vel_z": (104, "q32"),
    "angvel_x": (106, "q32"), "angvel_y": (108, "q32"), "angvel_z": (110, "q32"),
    "s": (112, "q32"), "pace": (114, "q32"), "vmax": (116, "q32"),
    "lap": (118, "u32"), "gates_passed": (119, "u32"), "collisions": (120, "u32"),
}


class Prog:
    def __init__(self):
        self.ins = []
        self.n = 0

    def op(self, op, **kw):
        if op.startswith("store_") or op == "emit_event":
            self.ins.append(dict(op=op, **kw))
            return None
        self.n += 1
        dst = "r%d" % self.n
        self.ins.append(dict(op=op, dst=dst, **kw))
        return dst

    def q(self, num, den=1):
        return self.op("const_q32", num=num, den=den)

    def u(self, v):
        return self.op("const_u32", value=v)

    def v3c(self, pt):
        return self.op("make_v3", x=self.q(*pt[0]), y=self.q(*pt[1]), z=self.q(*pt[2]))


def capsule(system_id, src, fields, reads_only, caps=None, max_events=None, max_insn=4096):
    out = []
    for name in fields:
        fid, t = F[name]
        out.append({"id": fid, "name": name, "type": t, "read": True, "write": name not in reads_only})
    c = {
        "capsule_format": "0.1", "ir_version": "0.3", "system_id": system_id, "source": src,
        "schema": {"id": "space3d", "version": 1, "layout_generation": 1}, "numeric_profile": "fixed_q32_v1",
        "fields": out,
        "schedule": {"phase": "simulate", "after": [], "before": [], "parallel": "serial"},
        "limits": {"max_instructions": max_insn, "max_rows": 65536, "allocation": "forbidden"},
        "tests": ["bench/space3d/racer_microcosm.c"],
    }
    if caps is not None:
        c["capabilities"] = caps
    if max_events is not None:
        c["limits"]["max_events_per_row"] = max_events
    return c


def lir(system_id, ins):
    return {"ir_version": "0.3", "system_id": system_id, "numeric_profile": "fixed_q32_v1", "query": "space3d",
            "instructions": ins}


def gen_move():
    p = Prog()
    dt = p.q(*DT)
    zero_q = p.q(0)
    one_q = p.q(1)
    is_mover = p.op("eq_u32", a=p.op("load_u32", field="kind"), b=p.u(1))
    pos = p.op("load_v3", x="pos_x", y="pos_y", z="pos_z")
    prev = p.op("load_v3", x="prev_x", y="prev_y", z="prev_z")
    vel = p.op("load_v3", x="vel_x", y="vel_y", z="vel_z")
    angvel = p.op("load_v3", x="angvel_x", y="angvel_y", z="angvel_z")
    q = p.op("make_quat", w=p.op("load_q32", field="rot_w"), v=p.op("load_v3", x="rot_x", y="rot_y", z="rot_z"))
    # Collision response from the previous tick's query: reflect the approaching part of the velocity; rewind
    # to the contact when it happened during the tick (toi > 0); push out along n when it started overlapped.
    hit = p.op("lt_u32", a=p.u(0), b=p.op("load_u32", field="hit_count"))
    n = p.op("load_v3", x="c0_nx", y="c0_ny", z="c0_nz")
    toi = p.op("load_q32", field="c0_toi")
    inside = p.op("and_bool", a=hit, b=p.op("le_q32", a=toi, b=zero_q))
    rewind = p.op("and_bool", a=hit, b=p.op("not_bool", a=inside))
    vn = p.op("v3_dot", a=vel, b=n)
    respond = p.op("and_bool", a=hit, b=p.op("lt_q32", a=vn, b=zero_q))
    vel_r = p.op("v3_sub", a=vel, b=p.op("v3_scale", v=n, s=p.op("mul_q32", a=vn, b=p.q(*RESTITUTION))))
    vel0 = p.op("select_v3", cond=respond, on_true=vel_r, on_false=vel)
    vel1 = p.op("select_v3", cond=inside, on_true=p.op("v3_add", a=vel0, b=p.op("v3_scale", v=n, s=p.q(*PUSH_OUT))),
                on_false=vel0)
    back = p.op("v3_scale", v=p.op("v3_sub", a=pos, b=prev), s=toi)
    p1 = p.op("select_v3", cond=rewind, on_true=p.op("v3_add", a=prev, b=back), on_false=pos)
    # Pace target: s advances at the racer's pace; the target is the course point LOOKAHEAD ahead.
    four = p.q(4)
    s = p.op("load_q32", field="s")
    s1 = p.op("add_q32", a=s, b=p.op("mul_q32", a=p.op("load_q32", field="pace"), b=dt))
    s2 = p.op("select_q32", cond=p.op("le_q32", a=four, b=s1), on_true=p.op("sub_q32", a=s1, b=four), on_false=s1)
    st = p.op("add_q32", a=s2, b=p.q(*LOOKAHEAD))
    st2 = p.op("select_q32", cond=p.op("le_q32", a=four, b=st), on_true=p.op("sub_q32", a=st, b=four), on_false=st)
    seg = p.op("q32_to_i32_floor", a=st2)
    t = p.op("sub_q32", a=st2, b=p.op("q32_from_i32", a=seg))
    mt = p.op("sub_q32", a=one_q, b=t)
    mt2 = p.op("mul_q32", a=mt, b=mt)
    t2 = p.op("mul_q32", a=t, b=t)
    three = p.q(3)
    w0 = p.op("mul_q32", a=mt2, b=mt)
    w1 = p.op("mul_q32", a=p.op("mul_q32", a=three, b=mt2), b=t)
    w2 = p.op("mul_q32", a=p.op("mul_q32", a=three, b=mt), b=t2)
    w3 = p.op("mul_q32", a=t2, b=t)
    pts = []
    for sg in SEGMENTS:
        c = [p.v3c(pt) for pt in sg]
        a01 = p.op("v3_add", a=p.op("v3_scale", v=c[0], s=w0), b=p.op("v3_scale", v=c[1], s=w1))
        a23 = p.op("v3_add", a=p.op("v3_scale", v=c[2], s=w2), b=p.op("v3_scale", v=c[3], s=w3))
        pts.append(p.op("v3_add", a=a01, b=a23))
    target = pts[0]
    for k in (1, 2, 3):
        target = p.op("select_v3", cond=p.op("eq_i32", a=seg, b=p.op("const_i32", value=k)), on_true=pts[k],
                      on_false=target)
    # Steering towards the target: speed min(vmax, gain * distance), bounded acceleration.
    to = p.op("v3_sub", a=target, b=p1)
    d = p.op("v3_normalize", a=to)
    speed = p.op("min_q32", a=p.op("load_q32", field="vmax"), b=p.op("mul_q32", a=p.op("v3_len", a=to),
                                                                     b=p.q(*FOLLOW_GAIN)))
    desired = p.op("v3_scale", v=d, s=speed)
    vel2 = p.op("v3_add", a=vel1, b=p.op("v3_clamp_len", v=p.op("v3_sub", a=desired, b=vel1), max=p.q(*ACCEL_PER_TICK)))
    # Orientation: turn the local +x axis towards the desired direction; integrate the local angular velocity.
    fwd = p.op("quat_rotate", q=q, v=p.op("make_v3", x=one_q, y=zero_q, z=zero_q))
    w_world = p.op("v3_scale", v=p.op("v3_cross", a=fwd, b=d), s=p.q(*TURN_GAIN))
    w_local = p.op("quat_rotate", q=p.op("quat_conj", a=q), v=w_world)
    q2 = p.op("quat_integrate", q=q, w=w_local, dt=dt)
    p2 = p.op("v3_add", a=p1, b=p.op("v3_scale", v=vel2, s=dt))
    # Stores: movers take the new state; every other row keeps its values.
    p.op("store_v3", x="prev_x", y="prev_y", z="prev_z", src=p.op("select_v3", cond=is_mover, on_true=p1, on_false=prev))
    p.op("store_v3", x="pos_x", y="pos_y", z="pos_z", src=p.op("select_v3", cond=is_mover, on_true=p2, on_false=pos))
    p.op("store_v3", x="vel_x", y="vel_y", z="vel_z", src=p.op("select_v3", cond=is_mover, on_true=vel2, on_false=vel))
    p.op("store_v3", x="angvel_x", y="angvel_y", z="angvel_z",
         src=p.op("select_v3", cond=is_mover, on_true=w_local, on_false=angvel))
    qo = p.op("select_quat", cond=is_mover, on_true=q2, on_false=q)
    for axis, name in enumerate(("rot_w", "rot_x", "rot_y", "rot_z")):
        p.op("store_q32", field=name, src=p.op("quat_get", q=qo, axis=axis))
    p.op("store_q32", field="s", src=p.op("select_q32", cond=is_mover, on_true=s2, on_false=s))
    fields = ["kind", "pos_x", "pos_y", "pos_z", "prev_x", "prev_y", "prev_z", "rot_w", "rot_x", "rot_y", "rot_z",
              "hit_count", "c0_toi", "c0_nx", "c0_ny", "c0_nz", "vel_x", "vel_y", "vel_z", "angvel_x", "angvel_y",
              "angvel_z", "s", "pace", "vmax"]
    ro = {"kind", "hit_count", "c0_toi", "c0_nx", "c0_ny", "c0_nz", "pace", "vmax"}
    return capsule("racer.move", "move.lir.json", fields, ro), lir("racer.move", p.ins)


def gen_race():
    p = Prog()
    one = p.u(1)
    zero = p.u(0)
    fwd = p.op("eq_u32", a=p.op("load_u32", field="gate_dir"), b=one)
    ng = p.op("load_u32", field="next_gate")
    ng1 = p.op("add_u32", a=ng, b=one)
    wrap = p.op("eq_u32", a=ng1, b=p.u(GATES))
    ng3 = p.op("select_u32", cond=fwd, on_true=p.op("select_u32", cond=wrap, on_true=zero, on_false=ng1), on_false=ng)
    lap = p.op("load_u32", field="lap")
    lap2 = p.op("select_u32", cond=p.op("and_bool", a=fwd, b=wrap), on_true=p.op("add_u32", a=lap, b=one), on_false=lap)
    gp = p.op("load_u32", field="gates_passed")
    gp2 = p.op("select_u32", cond=fwd, on_true=p.op("add_u32", a=gp, b=one), on_false=gp)
    hc = p.op("load_u32", field="hit_count")
    hit = p.op("lt_u32", a=zero, b=hc)
    col = p.op("load_u32", field="collisions")
    col2 = p.op("select_u32", cond=hit, on_true=p.op("add_u32", a=col, b=one), on_false=col)
    p.op("emit_event", cond=fwd, kind=1, a=ng, b=lap2)
    p.op("emit_event", cond=hit, kind=2, a=p.op("load_u32", field="c0_slot"), b=hc)
    p.op("store_u32", field="next_gate", src=ng3)
    p.op("store_u32", field="lap", src=lap2)
    p.op("store_u32", field="gates_passed", src=gp2)
    p.op("store_u32", field="collisions", src=col2)
    fields = ["next_gate", "hit_count", "c0_slot", "gate_dir", "lap", "gates_passed", "collisions"]
    ro = {"hit_count", "c0_slot", "gate_dir"}
    return (capsule("racer.race", "race.lir.json", fields, ro, caps=["emit_event"], max_events=2),
            lir("racer.race", p.ins))


def outputs():
    move_c, move_l = gen_move()
    race_c, race_l = gen_race()
    course = {"format": "6dof_racer_microcosm.course/1", "segments": SEGMENTS, "gates": GATES, "dt": DT,
              "lookahead": LOOKAHEAD, "accel_per_tick": ACCEL_PER_TICK, "turn_gain": TURN_GAIN,
              "restitution": RESTITUTION, "follow_gain": FOLLOW_GAIN, "push_out": PUSH_OUT, "note": "rationals are [num, den]; q32 = RNE(num * 2^32 / den)"}
    d = "capsules/space3d_racer/"
    return {
        "bench/space3d/course.json": course,
        d + "move.capsule.json": move_c, d + "move.lir.json": move_l,
        d + "race.capsule.json": race_c, d + "race.lir.json": race_l,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    bad = 0
    for rel, obj in outputs().items():
        text = json.dumps(obj, indent=1) + "\n"
        path = os.path.join(ROOT, rel)
        if args.check:
            try:
                cur = open(path, encoding="utf-8").read()
            except OSError:
                cur = None
            if cur != text:
                print("differs:", rel)
                bad += 1
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    if not args.check:
        o = outputs()
        print("move: %d instructions; race: %d instructions" % (
            len(o["capsules/space3d_racer/move.lir.json"]["instructions"]),
            len(o["capsules/space3d_racer/race.lir.json"]["instructions"])))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
