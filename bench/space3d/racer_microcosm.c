/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * 6dof_racer_microcosm: a synthetic 6DOF racing workload on the engine's own schedule executor.
 *
 * Schedule (src/runtime/schedule.h, run with su_sched_run_ticks, entity runtime enabled):
 *   0 racer.race     IR 0.3  gate progress, laps, collision counters, emit_event (gate passed, contact)
 *   1 racer.move     IR 0.3  collision response, pace target on the course, steering, orientation, motion
 *   2 space3d.query  host    swept contacts and gate crossings (docs/SPACE3D.md), consumed next tick
 * The capsules are verified by su_lir_verify and the host record by su_host_load, exactly as world.create does;
 * the schedule union checks every shared lane's value type. Scene: N racers, 16 gates and M obstacles placed
 * along a generated 41 km course (bench/space3d/course.json), seeded; no external assets.
 *
 *   racer_microcosm --root <repo> --n 32 [--ticks 1200] [--warmup 120] [--obstacles 48] [--seed 1]
 *                   [--no-query] [--replay] [--digest-every 120]
 *   racer_microcosm --root <repo> --n 32 --rollback 1,6,12 [--rollback-iters 200] [--warmup 120]
 * Prints one JSON object: identities, per-tick wall time distribution, digests, event counts, race totals.
 * --replay additionally runs the scene again from scratch and from a mid-run canonical checkpoint and reports
 * whether every digest matched. Timing is an observation on the machine that ran it, not a qualification.
 *
 * --rollback measures client-side rollback resimulation: after the warm-up the world is saved (canonical bytes),
 * restored and stepped K ticks once to obtain the reference digest, then restored and re-stepped K ticks (one
 * su_sched_run_ticks call) --rollback-iters times per K. Every iteration's digest must equal the reference
 * digest, or the driver exits 1. Save, restore and resimulation are timed separately. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "ir/su_num.h"
#include "ir/su_sha256.h"
#include "platform/su_platform.h"
#include "runtime/entity_tick.h"
#include "runtime/rts/host_decl.h"
#include "runtime/schedule.h"
#include "space3d_ops.h"
#include "sutekh/status.h"
#include "world/su_world.h"

#define ONE SU_Q32_ONE
#define GATES 16u
#define MAX_TICKS 200000u
#define VM_TIER_NAME "reference"

typedef struct Args {
    const char *root;
    uint32_t n, ticks, warmup, obstacles, digest_every;
    uint64_t seed;
    int no_query, replay, dump_racers;
    uint32_t rollback_k[8], rollback_nk, rollback_iters;
    int trace; /* racer index to trace per tick on stderr, -1 none */
} Args;

/* ---------- course ---------- */

typedef struct Course {
    SuV3 p[4][4];
} Course;

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    b = malloc((size_t)sz + 1u);
    if (b != NULL && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return b;
}

static int64_t rat_q(const SuJsonValue *pair) {
    const SuJsonValue *n = pair->u.array.items[0], *d = pair->u.array.items[1];
    int64_t num = (int64_t)n->u.number.magnitude, out = 0;
    uint64_t den = d->u.number.magnitude;
    if (n->u.number.negative) {
        num = -num;
    }
    if (!su_q32_from_ratio(num, den, &out)) {
        fprintf(stderr, "course: rational out of range\n");
        exit(2);
    }
    return out;
}

static int load_course(const char *root, Course *c) {
    char path[1024];
    size_t len = 0;
    uint8_t *b;
    SuJsonDoc doc;
    SuJsonError err;
    SuJsonLimits lim = {1u << 20, 32u};
    const SuJsonValue *segs;
    int s, k;
    snprintf(path, sizeof path, "%s/bench/space3d/course.json", root);
    b = slurp(path, &len);
    if (b == NULL || su_json_parse(b, len, &lim, &doc, &err) != SU_OK) {
        free(b);
        return 0;
    }
    segs = su_json_member(doc.root, "segments");
    for (s = 0; s < 4; s++) {
        for (k = 0; k < 4; k++) {
            const SuJsonValue *pt = segs->u.array.items[s]->u.array.items[k];
            c->p[s][k].x = rat_q(pt->u.array.items[0]);
            c->p[s][k].y = rat_q(pt->u.array.items[1]);
            c->p[s][k].z = rat_q(pt->u.array.items[2]);
        }
    }
    su_json_doc_free(&doc);
    free(b);
    return 1;
}

/* Course point and unit tangent at s in [0, 4) (cubic Bezier segment floor(s), parameter frac(s)). */
static void course_at(const Course *c, int64_t s, SuV3 *pos, SuV3 *tan) {
    int64_t four = 4 * ONE;
    int32_t seg;
    int64_t t, mt, mt2, t2, three = 3 * ONE, six = 6 * ONE;
    const SuV3 *p;
    while (s >= four) {
        s -= four;
    }
    while (s < 0) {
        s += four;
    }
    seg = su_q32_to_i32_floor(s);
    p = c->p[seg];
    t = su_q32_sub(s, su_q32_from_i32(seg));
    mt = su_q32_sub(ONE, t);
    mt2 = su_q32_mul(mt, mt);
    t2 = su_q32_mul(t, t);
    *pos = su_v3_add(su_v3_add(su_v3_scale(p[0], su_q32_mul(mt2, mt)), su_v3_scale(p[1], su_q32_mul(su_q32_mul(three, mt2), t))),
                     su_v3_add(su_v3_scale(p[2], su_q32_mul(su_q32_mul(three, mt), t2)), su_v3_scale(p[3], su_q32_mul(t2, t))));
    /* B'(t) = 3 mt^2 (P1-P0) + 6 mt t (P2-P1) + 3 t^2 (P3-P2) */
    *tan = su_v3_normalize(su_v3_add(su_v3_add(su_v3_scale(su_v3_sub(p[1], p[0]), su_q32_mul(three, mt2)),
                                               su_v3_scale(su_v3_sub(p[2], p[1]), su_q32_mul(su_q32_mul(six, mt), t))),
                                     su_v3_scale(su_v3_sub(p[3], p[2]), su_q32_mul(three, t2))));
}

/* Unit quaternion rotating the unit axis `from` onto the unit vector `to` (half-way construction). */
static SuQuat quat_from_to(SuV3 from, SuV3 to) {
    SuQuat q;
    SuV3 c = su_v3_cross(from, to);
    q.w = su_q32_add(ONE, su_v3_dot(from, to));
    q.x = c.x;
    q.y = c.y;
    q.z = c.z;
    return su_quat_normalize(q);
}

/* ---------- scene columns ---------- */

typedef struct Cols {
    uint32_t nfields;
    uint32_t ids[SU_LIMIT_FIELDS];
    uint32_t *col[SU_LIMIT_FIELDS];
} Cols;

/* Column of a field ID, or NULL when no system of the schedule binds the field (--no-query drops the fields
 * only space3d.query reads; the scene sets them only when they exist). */
static uint32_t *colp(Cols *c, uint32_t id) {
    uint32_t i;
    for (i = 0; i < c->nfields; i++) {
        if (c->ids[i] == id) {
            return c->col[i];
        }
    }
    return NULL;
}

static void set32(Cols *c, uint32_t id, uint32_t r, uint32_t v) {
    uint32_t *col = colp(c, id);
    if (col != NULL) {
        col[r] = v;
    }
}

static void setq(Cols *c, uint32_t id, uint32_t r, int64_t v) {
    set32(c, id, r, (uint32_t)((uint64_t)v & 0xFFFFFFFFu));
    set32(c, id + 1u, r, (uint32_t)((uint64_t)v >> 32));
}

static void set3(Cols *c, uint32_t id, uint32_t r, SuV3 v) {
    setq(c, id, r, v.x);
    setq(c, id + 2u, r, v.y);
    setq(c, id + 4u, r, v.z);
}

static void setrot(Cols *c, uint32_t r, SuQuat q) {
    setq(c, SPACE3D_F_ROT_W, r, q.w);
    setq(c, SPACE3D_F_ROT_X, r, q.x);
    setq(c, SPACE3D_F_ROT_Y, r, q.y);
    setq(c, SPACE3D_F_ROT_Z, r, q.z);
}

static uint64_t g_rng;
static uint64_t rng(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static int64_t rq_range(int64_t lo_m, int64_t hi_m) { /* q32 uniform in [lo, hi] metres */
    return lo_m * ONE + (int64_t)(rng() % (uint64_t)((hi_m - lo_m) * ONE + 1));
}

#define F_VEL 100u
#define F_S 112u
#define F_PACE 114u
#define F_VMAX 116u

/* Racers 0..n-1, gates n..n+15, obstacles after. */
static void build_scene(const Args *a, const Course *course, Cols *c, uint32_t rows) {
    uint32_t i;
    SuV3 xa = {ONE, 0, 0}, za = {0, 0, ONE};
    g_rng = a->seed * UINT64_C(1000003) + 17u;
    for (i = 0; i < a->n; i++) {
        /* Spread along the loop; start slightly behind the pace target. */
        int64_t s0 = (int64_t)((uint64_t)(4u * ONE) / a->n * i);
        uint32_t vmax_m = 850u + (i * 37u) % 150u, g;
        int64_t vmax = su_q32_from_i32((int32_t)vmax_m), pace = 0;
        SuV3 pos, tan, lat;
        course_at(course, s0, &pos, &tan);
        lat = su_v3_normalize(su_v3_cross(tan, za));
        pos = su_v3_add(pos, su_v3_scale(lat, rq_range(-20, 20)));
        set32(c, SPACE3D_F_KIND, i, SPACE3D_KIND_MOVER);
        set3(c, SPACE3D_F_POS_X, i, pos);
        set3(c, SPACE3D_F_PREV_X, i, pos);
        set3(c, F_VEL, i, su_v3_scale(tan, 600 * ONE));
        setrot(c, i, quat_from_to(xa, tan));
        setq(c, SPACE3D_F_RADIUS, i, 6 * ONE);
        setq(c, F_S, i, s0);
        /* pace = 0.9 * vmax * 4 / 41387 course units per second (41,387 m is the loop length estimate) */
        su_q32_from_ratio((int64_t)vmax_m * 4 * 90, UINT64_C(41387) * 100u, &pace);
        setq(c, F_PACE, i, pace);
        setq(c, F_VMAX, i, vmax);
        g = (uint32_t)(su_q32_to_i32_floor(s0 * 4) + 1) % GATES;
        set32(c, SPACE3D_F_NEXT_GATE, i, g);
    }
    for (i = 0; i < GATES; i++) {
        uint32_t r = a->n + i;
        SuV3 pos, tan;
        course_at(course, (int64_t)i * (ONE / 4), &pos, &tan);
        set32(c, SPACE3D_F_KIND, r, SPACE3D_KIND_GATE_ELLIPSE); /* semi-axes 120 m x 80 m */
        set32(c, SPACE3D_F_GATE_INDEX, r, i);
        set3(c, SPACE3D_F_POS_X, r, pos);
        setrot(c, r, quat_from_to(za, tan));
        setq(c, SPACE3D_F_HALF_X, r, 120 * ONE);
        setq(c, SPACE3D_F_HALF_Y, r, 80 * ONE);
    }
    for (i = 0; i < a->obstacles; i++) {
        uint32_t r = a->n + GATES + i, kind = SPACE3D_KIND_SPHERE + i % 3u;
        int64_t s = (int64_t)(rng() % (uint64_t)(4 * ONE));
        SuV3 pos, tan, lat;
        SuQuat q0, q;
        course_at(course, s, &pos, &tan);
        lat = su_v3_normalize(su_v3_cross(tan, za));
        pos = su_v3_add(pos, su_v3_scale(lat, (rng() & 1u) ? rq_range(40, 160) : rq_range(-160, -40)));
        pos.z = su_q32_add(pos.z, rq_range(-30, 30));
        q0.w = rq_range(-1, 1);
        q0.x = rq_range(-1, 1);
        q0.y = rq_range(-1, 1);
        q0.z = rq_range(-1, 1);
        q = su_quat_normalize(q0);
        set32(c, SPACE3D_F_KIND, r, kind);
        set3(c, SPACE3D_F_POS_X, r, pos);
        setrot(c, r, q);
        setq(c, SPACE3D_F_RADIUS, r, rq_range(10, 40));
        setq(c, SPACE3D_F_HALF_X, r, rq_range(10, 40));
        setq(c, SPACE3D_F_HALF_Y, r, rq_range(10, 40));
        setq(c, SPACE3D_F_HALF_Z, r, rq_range(10, 60));
    }
    (void)rows;
}

/* ---------- schedule and world ---------- */

static SuLirModule *load_ir(const char *root, const char *name) {
    char cp[1024], lp[1024];
    size_t cn = 0, ln = 0;
    uint8_t *cb, *lb;
    SuLirModule *m = calloc(1, sizeof *m);
    SuDiag d;
    uint32_t st;
    snprintf(cp, sizeof cp, "%s/capsules/space3d_racer/%s.capsule.json", root, name);
    snprintf(lp, sizeof lp, "%s/capsules/space3d_racer/%s.lir.json", root, name);
    cb = slurp(cp, &cn);
    lb = slurp(lp, &ln);
    if (m == NULL || cb == NULL || lb == NULL) {
        fprintf(stderr, "cannot read %s\n", cp);
        exit(2);
    }
    st = su_lir_verify(cb, cn, lb, ln, m, &d);
    if (st != SU_OK) {
        fprintf(stderr, "%s: %s at %s: %s\n", name, su_status_name(st), d.json_pointer, d.message);
        exit(2);
    }
    free(cb);
    free(lb);
    return m;
}

typedef struct Sim {
    SuSchedule s;
    SuWorld *w;
    SuWorldDesc desc;
    SuWorldFieldDesc fd[SU_LIMIT_FIELDS];
    uint32_t *ids;
    uint32_t rows;
    Cols cols;
} Sim;

static void sim_schedule(Sim *sim, const Args *a) {
    char path[1024], msg[256];
    SuLirModule *decl = NULL;
    uint32_t sv = 0, gen = 0, st;
    su_sched_init(&sim->s);
    st = su_sched_add(&sim->s, load_ir(a->root, "race"), 1, 1, 0, 1, 0);
    if (st == SU_OK) st = su_sched_add(&sim->s, load_ir(a->root, "move"), 1, 1, 0, 1, 0);
    if (st == SU_OK && !a->no_query) {
        snprintf(path, sizeof path, "%s/profiles/space3d/space3d_query.capsule.json", a->root);
        st = su_host_load(path, NULL, &g_space3d_query_desc, &decl, &sv, &gen, msg, sizeof msg);
        if (st != SU_OK) {
            fprintf(stderr, "host record: %s\n", msg);
            exit(2);
        }
        st = su_sched_add_host(&sim->s, &g_space3d_query_desc, decl, sv, gen, 0, 1, 0);
    }
    if (st == SU_OK) st = su_sched_finalize(&sim->s);
    if (st == SU_OK) st = su_sched_enable_entities(&sim->s, 1u, 2u * (uint64_t)a->n + 2u, UINT64_C(1048576));
    if (st != SU_OK) {
        fprintf(stderr, "schedule: %s\n", su_status_name(st));
        exit(2);
    }
}

static void sim_desc(Sim *sim) {
    uint32_t i;
    for (i = 0; i < sim->s.field_count; i++) {
        sim->fd[i].id = sim->s.fields[i].id;
        sim->fd[i].type = sim->s.fields[i].type;
        sim->fd[i].access = sim->s.fields[i].access;
        sim->fd[i].name = sim->s.fields[i].name;
    }
    memset(&sim->desc, 0, sizeof sim->desc);
    sim->desc.schema_id = sim->s.schema_id;
    sim->desc.schema_version = sim->s.schema_version;
    sim->desc.layout_generation = sim->s.layout_generation;
    sim->desc.row_count = sim->rows;
    sim->desc.entity_ids = sim->ids;
    sim->desc.fields = sim->fd;
    sim->desc.field_count = sim->s.field_count;
}

static void sim_create(Sim *sim, const Args *a, const Course *course) {
    uint32_t i, st;
    memset(sim, 0, sizeof *sim);
    sim->rows = a->n + GATES + a->obstacles;
    sim_schedule(sim, a);
    sim->ids = calloc(sim->rows, sizeof *sim->ids);
    for (i = 0; i < sim->rows; i++) {
        sim->ids[i] = i + 1u;
    }
    sim->cols.nfields = sim->s.field_count;
    for (i = 0; i < sim->s.field_count; i++) {
        sim->cols.ids[i] = sim->s.fields[i].id;
        sim->cols.col[i] = calloc(sim->rows, sizeof(uint32_t));
    }
    build_scene(a, course, &sim->cols, sim->rows);
    sim_desc(sim);
    st = su_world_create(&sim->desc, &sim->w);
    for (i = 0; st == SU_OK && i < sim->s.field_count; i++) {
        st = su_world_init_committed(sim->w, sim->cols.ids[i], sim->cols.col[i], sim->rows);
    }
    if (st != SU_OK) {
        fprintf(stderr, "world: %s\n", su_status_name(st));
        exit(2);
    }
}

static void sim_free(Sim *sim) {
    uint32_t i;
    su_world_destroy(sim->w);
    su_sched_free(&sim->s);
    for (i = 0; i < sim->cols.nfields; i++) {
        free(sim->cols.col[i]);
    }
    free(sim->ids);
}

static void state_digest(const SuWorld *w, char hex[65], uint8_t **keep, size_t *keep_len) {
    size_t n = 0, got = 0;
    uint8_t *buf, h[32];
    SuSha256 c;
    su_world_canonical_size(w, &n);
    buf = malloc(n > 0 ? n : 1u);
    su_world_canonical_write(w, buf, n, &got);
    su_sha256_init(&c);
    su_sha256_update(&c, buf, got);
    su_sha256_final(&c, h);
    su_sha256_hex(h, hex);
    if (keep != NULL) {
        *keep = buf;
        *keep_len = got;
    } else {
        free(buf);
    }
}

static void event_digest(SuSchedule *s, char hex[65], uint64_t *count) {
    SuEventLog *log = su_ent_rt_log(s->ent);
    uint64_t len = 0;
    uint8_t *buf, h[32];
    SuSha256 c;
    su_evlog_dump(log, NULL, 0, &len);
    buf = malloc(len > 0 ? (size_t)len : 1u);
    su_evlog_dump(log, buf, len, &len);
    su_sha256_init(&c);
    su_sha256_update(&c, buf, (size_t)len);
    su_sha256_final(&c, h);
    su_sha256_hex(h, hex);
    *count = su_evlog_next_seq(log);
    free(buf);
}

static uint32_t step(Sim *sim, uint32_t ticks) {
    uint64_t done = 0;
    uint32_t fi = 0, st = su_sched_run_ticks(sim->w, &sim->s, ticks, NULL, NULL, &done, &fi);
    if (st != SU_OK) {
        fprintf(stderr, "tick failed: %s (system %u)\n", su_status_name(st), (unsigned)fi);
    }
    return st;
}

static int64_t read_q(const SuWorld *w, uint32_t id, uint32_t r) {
    const uint32_t *lo, *hi;
    uint64_t rows;
    su_world_read_committed(w, id, &lo, &rows);
    su_world_read_committed(w, id + 1u, &hi, &rows);
    return su_i64_from_bits(((uint64_t)hi[r] << 32) | (uint64_t)lo[r]);
}

static uint32_t read_u(const SuWorld *w, uint32_t id, uint32_t r) {
    const uint32_t *c;
    uint64_t rows;
    su_world_read_committed(w, id, &c, &rows);
    return c[r];
}

/* Diagnostic (stderr): racer r's prev, pos, next gate, signed distance to that gate's plane and gate-local
 * coordinates of pos, and the query's gate_dir for the tick just published. */
static void trace_racer(const Sim *sim, uint32_t n, uint32_t r) {
    uint32_t ng = read_u(sim->w, SPACE3D_F_NEXT_GATE, r), g = n + ng;
    SuV3 p, gp, rel;
    SuQuat q;
    double d, lx, ly;
    p.x = read_q(sim->w, SPACE3D_F_POS_X, r);
    p.y = read_q(sim->w, SPACE3D_F_POS_Y, r);
    p.z = read_q(sim->w, SPACE3D_F_POS_Z, r);
    gp.x = read_q(sim->w, SPACE3D_F_POS_X, g);
    gp.y = read_q(sim->w, SPACE3D_F_POS_Y, g);
    gp.z = read_q(sim->w, SPACE3D_F_POS_Z, g);
    q.w = read_q(sim->w, SPACE3D_F_ROT_W, g);
    q.x = read_q(sim->w, SPACE3D_F_ROT_X, g);
    q.y = read_q(sim->w, SPACE3D_F_ROT_Y, g);
    q.z = read_q(sim->w, SPACE3D_F_ROT_Z, g);
    rel = su_v3_sub(p, gp);
    {
        SuV3 ez = {0, 0, ONE}, ex = {ONE, 0, 0}, ey = {0, ONE, 0};
        d = (double)su_v3_dot(rel, su_quat_rotate(q, ez)) / 4294967296.0;
        lx = (double)su_v3_dot(rel, su_quat_rotate(q, ex)) / 4294967296.0;
        ly = (double)su_v3_dot(rel, su_quat_rotate(q, ey)) / 4294967296.0;
    }
    fprintf(stderr, "tick %llu racer %u next_gate %u gate_dir %u plane_d %.2f lx %.2f ly %.2f kind_g %u\n",
            (unsigned long long)su_world_tick(sim->w), (unsigned)r, (unsigned)ng,
            (unsigned)read_u(sim->w, SPACE3D_F_GATE_DIR, r), d, lx, ly, (unsigned)read_u(sim->w, SPACE3D_F_KIND, g));
}

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return (a > b) - (a < b);
}

static uint64_t pct(const uint64_t *sorted, uint32_t n, uint32_t p) { /* nearest rank */
    uint32_t k = (uint32_t)(((uint64_t)p * n + 99u) / 100u);
    return sorted[k == 0 ? 0 : k - 1u];
}

static uint32_t sum_col(const SuWorld *w, uint32_t id, uint32_t n) {
    const uint32_t *c;
    uint64_t rows;
    uint32_t i, s = 0;
    if (su_world_read_committed(w, id, &c, &rows) != SU_OK) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        s += c[i];
    }
    return s;
}

static void ns_stats(const char *name, uint64_t *v, uint32_t n, int comma) {
    qsort(v, n, sizeof *v, cmp_u64);
    printf("\"%s\":{\"min\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64
           ",\"max\":%" PRIu64 "}%s",
           name, v[0], pct(v, n, 50), pct(v, n, 95), pct(v, n, 99), v[n - 1u], comma ? "," : "");
}

/* Rollback resimulation (see the header). Returns 0 when every resimulated digest matched. */
static int run_rollback(const Args *a, const Course *course) {
    Sim sim;
    uint8_t *snap, *scratch;
    size_t snap_len = 0, got = 0;
    uint64_t *t_save, *t_restore, *t_resim, *t_total;
    uint64_t snap_tick;
    uint32_t it, k;
    int ok = 1;
    sim_create(&sim, a, course);
    if (step(&sim, a->warmup) != SU_OK) {
        return 1;
    }
    snap_tick = su_world_tick(sim.w);
    su_world_canonical_size(sim.w, &snap_len);
    snap = malloc(snap_len);
    scratch = malloc(snap_len);
    t_save = calloc(a->rollback_iters, sizeof *t_save);
    t_restore = calloc(a->rollback_iters, sizeof *t_restore);
    t_resim = calloc(a->rollback_iters, sizeof *t_resim);
    t_total = calloc(a->rollback_iters, sizeof *t_total);
    if (snap == NULL || scratch == NULL || t_save == NULL || t_restore == NULL || t_resim == NULL || t_total == NULL ||
        su_world_canonical_write(sim.w, snap, snap_len, &got) != SU_OK || got != snap_len) {
        fprintf(stderr, "rollback: cannot save the anchor state\n");
        return 1;
    }
    /* Save cost: canonical write of the live state into a preallocated buffer (one history entry per tick). */
    for (it = 0; it < a->rollback_iters; it++) {
        uint64_t t0 = 0, t1 = 0;
        su_platform_monotonic_ns(&t0);
        su_world_canonical_write(sim.w, scratch, snap_len, &got);
        su_platform_monotonic_ns(&t1);
        t_save[it] = t1 - t0;
    }
    printf("{\"workload\":\"6dof_racer_rollback\",\"n\":%u,\"gates\":%u,\"obstacles\":%u,\"rows\":%u,"
           "\"seed\":%" PRIu64 ",\"tick_hz\":120,\"query\":%s,\"warmup_ticks\":%u,\"anchor_tick\":%" PRIu64
           ",\"iterations\":%u,\"snapshot_bytes\":%zu,\"vm\":\"%s\",",
           a->n, GATES, a->obstacles, sim.rows, a->seed, a->no_query ? "false" : "true", a->warmup, snap_tick,
           a->rollback_iters, snap_len, VM_TIER_NAME);
    printf("\"systems\":[");
    for (k = 0; k < sim.s.count; k++) {
        printf("%s{\"system_id\":\"%s\",\"semantic_hash\":\"%s\"}", k ? "," : "", sim.s.sys[k].system_id,
               sim.s.sys[k].mod->semantic_hash_hex);
    }
    printf("],");
    ns_stats("save_ns", t_save, a->rollback_iters, 1);
    printf("\"k\":[");
    for (k = 0; k < a->rollback_nk; k++) {
        uint32_t kt = a->rollback_k[k], mismatches = 0;
        char ref[65];
        SuWorld *rw = NULL;
        /* Reference: restore once and step K ticks. */
        if (su_world_restore_canonical(&sim.desc, snap, snap_len, &rw) != SU_OK) {
            fprintf(stderr, "rollback: restore failed\n");
            return 1;
        }
        su_world_destroy(sim.w);
        sim.w = rw;
        if (step(&sim, kt) != SU_OK) {
            return 1;
        }
        state_digest(sim.w, ref, NULL, NULL);
        for (it = 0; it < a->rollback_iters; it++) {
            uint64_t t0 = 0, t1 = 0, t2 = 0;
            char d[65];
            su_platform_monotonic_ns(&t0);
            if (su_world_restore_canonical(&sim.desc, snap, snap_len, &rw) != SU_OK) {
                fprintf(stderr, "rollback: restore failed\n");
                return 1;
            }
            su_world_destroy(sim.w);
            sim.w = rw;
            su_platform_monotonic_ns(&t1);
            if (step(&sim, kt) != SU_OK) {
                return 1;
            }
            su_platform_monotonic_ns(&t2);
            t_restore[it] = t1 - t0;
            t_resim[it] = t2 - t1;
            t_total[it] = t2 - t0;
            state_digest(sim.w, d, NULL, NULL);
            if (strcmp(d, ref) != 0 || su_world_tick(sim.w) != snap_tick + kt) {
                mismatches++;
            }
        }
        if (mismatches != 0u) {
            ok = 0;
        }
        printf("%s{\"k\":%u,\"final_state_sha256\":\"%s\",\"mismatches\":%u,", k ? "," : "", kt, ref, mismatches);
        ns_stats("restore_ns", t_restore, a->rollback_iters, 1);
        ns_stats("resim_ns", t_resim, a->rollback_iters, 1);
        ns_stats("total_ns", t_total, a->rollback_iters, 1);
        printf("\"resim_per_tick_p50_ns\":%" PRIu64 ",\"total_p50_fraction_of_budget\":%.4f,"
               "\"total_p99_fraction_of_budget\":%.4f}",
               pct(t_resim, a->rollback_iters, 50) / kt, (double)pct(t_total, a->rollback_iters, 50) / 8333333.0,
               (double)pct(t_total, a->rollback_iters, 99) / 8333333.0);
    }
    printf("],\"budget_ns_120hz\":8333333,\"digests_identical\":%s,", ok ? "true" : "false");
#if defined(__clang__)
    printf("\"compiler\":\"clang %s\",", __clang_version__);
#elif defined(__GNUC__)
    printf("\"compiler\":\"gcc %d.%d.%d\",", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    printf("\"compiler\":\"msvc %d\",", _MSC_VER);
#endif
#ifdef NDEBUG
    printf("\"build\":\"optimized (NDEBUG)\"}\n");
#else
    printf("\"build\":\"debug\"}\n");
#endif
    free(snap);
    free(scratch);
    free(t_save);
    free(t_restore);
    free(t_resim);
    free(t_total);
    sim_free(&sim);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    Args a;
    Course course;
    Sim sim;
    static uint64_t ns[MAX_TICKS];
    static char digests[MAX_TICKS / 60u + 4u][65];
    uint32_t nd = 0, t, i;
    char final_state[65], final_events[65];
    uint64_t events = 0, total_ns = 0;
    uint8_t *ckpt = NULL;
    size_t ckpt_len = 0;
    uint32_t ckpt_tick;
    int replay_ok = -1, ckpt_ok = -1;

    memset(&a, 0, sizeof a);
    a.root = ".";
    a.n = 32;
    a.ticks = 1200;
    a.warmup = 120;
    a.obstacles = 48;
    a.seed = 1;
    a.digest_every = 120;
    a.trace = -1;
    a.rollback_iters = 200;
    for (i = 1; i < (uint32_t)argc; i++) {
        const char *k = argv[i];
        const char *v = i + 1u < (uint32_t)argc ? argv[i + 1u] : "";
        if (strcmp(k, "--root") == 0) { a.root = v; i++; }
        else if (strcmp(k, "--n") == 0) { a.n = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(k, "--ticks") == 0) { a.ticks = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(k, "--warmup") == 0) { a.warmup = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(k, "--obstacles") == 0) { a.obstacles = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(k, "--seed") == 0) { a.seed = strtoull(v, NULL, 10); i++; }
        else if (strcmp(k, "--digest-every") == 0) { a.digest_every = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(k, "--no-query") == 0) { a.no_query = 1; }
        else if (strcmp(k, "--replay") == 0) { a.replay = 1; }
        else if (strcmp(k, "--dump-racers") == 0) { a.dump_racers = 1; }
        else if (strcmp(k, "--trace") == 0) { a.trace = (int)strtol(v, NULL, 10); i++; }
        else if (strcmp(k, "--rollback") == 0) {
            const char *p = v;
            char *e = NULL;
            a.rollback_nk = 0;
            while (*p != '\0' && a.rollback_nk < 8u) {
                a.rollback_k[a.rollback_nk++] = (uint32_t)strtoul(p, &e, 10);
                if (e == p || (*e != ',' && *e != '\0')) {
                    a.rollback_k[0] = 0; /* rejected below */
                    break;
                }
                p = *e == ',' ? e + 1 : e;
            }
            i++;
        }
        else if (strcmp(k, "--rollback-iters") == 0) { a.rollback_iters = (uint32_t)strtoul(v, NULL, 10); i++; }
        else { fprintf(stderr, "unknown argument %s\n", k); return 2; }
    }
    if (a.n == 0 || a.ticks == 0 || a.ticks + a.warmup > MAX_TICKS || a.digest_every == 0 ||
        a.n + GATES + a.obstacles > SPACE3D_MAX_ROWS) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }
    if (!load_course(a.root, &course)) {
        fprintf(stderr, "cannot read bench/space3d/course.json under %s\n", a.root);
        return 2;
    }

    if (a.rollback_nk > 0u) {
        for (i = 0; i < a.rollback_nk; i++) {
            if (a.rollback_k[i] == 0u || a.rollback_k[i] > 1024u) {
                fprintf(stderr, "bad --rollback\n");
                return 2;
            }
        }
        if (a.rollback_iters == 0u) {
            fprintf(stderr, "bad --rollback-iters\n");
            return 2;
        }
        return run_rollback(&a, &course);
    }

    /* Run A: warm-up, then timed single ticks; digests every digest_every ticks; checkpoint at the midpoint. */
    sim_create(&sim, &a, &course);
    if (step(&sim, a.warmup) != SU_OK) {
        return 1;
    }
    ckpt_tick = a.warmup + a.ticks / 2u;
    for (t = 0; t < a.ticks; t++) {
        uint64_t t0 = 0, t1 = 0;
        su_platform_monotonic_ns(&t0);
        if (step(&sim, 1) != SU_OK) {
            return 1;
        }
        su_platform_monotonic_ns(&t1);
        ns[t] = t1 - t0;
        if (a.trace >= 0 && (uint32_t)a.trace < a.n) {
            trace_racer(&sim, a.n, (uint32_t)a.trace);
        }
        total_ns += ns[t];
        if ((a.warmup + t + 1u) % a.digest_every == 0u) {
            state_digest(sim.w, digests[nd++], NULL, NULL);
        }
        if (a.replay && a.warmup + t + 1u == ckpt_tick) {
            char tmp[65];
            state_digest(sim.w, tmp, &ckpt, &ckpt_len);
        }
    }
    state_digest(sim.w, final_state, NULL, NULL);
    event_digest(&sim.s, final_events, &events);
    if (a.dump_racers) {
        /* Diagnostic (stderr): per racer position, speed, course parameter, next gate and its distance. */
        for (i = 0; i < a.n; i++) {
            const uint32_t *c;
            uint64_t rows;
            int64_t q[12];
            uint32_t ids[6] = {SPACE3D_F_POS_X, SPACE3D_F_POS_Y, SPACE3D_F_POS_Z, F_VEL, F_VEL + 2u, F_VEL + 4u}, k;
            uint32_t ng, gp;
            SuV3 p, v, gpos;
            for (k = 0; k < 6u; k++) {
                su_world_read_committed(sim.w, ids[k], &c, &rows);
                q[2 * k] = c[i];
                su_world_read_committed(sim.w, ids[k] + 1u, &c, &rows);
                q[2 * k + 1] = c[i];
            }
            p.x = su_i64_from_bits(((uint64_t)q[1] << 32) | (uint64_t)q[0]);
            p.y = su_i64_from_bits(((uint64_t)q[3] << 32) | (uint64_t)q[2]);
            p.z = su_i64_from_bits(((uint64_t)q[5] << 32) | (uint64_t)q[4]);
            v.x = su_i64_from_bits(((uint64_t)q[7] << 32) | (uint64_t)q[6]);
            v.y = su_i64_from_bits(((uint64_t)q[9] << 32) | (uint64_t)q[8]);
            v.z = su_i64_from_bits(((uint64_t)q[11] << 32) | (uint64_t)q[10]);
            su_world_read_committed(sim.w, SPACE3D_F_NEXT_GATE, &c, &rows);
            ng = c[i];
            su_world_read_committed(sim.w, 119u, &c, &rows);
            gp = c[i];
            {
                SuV3 tan;
                course_at(&course, (int64_t)ng * (ONE / 4), &gpos, &tan);
            }
            fprintf(stderr, "racer %u: pos %.1f %.1f %.1f speed %.1f next_gate %u gates %u dist_to_next %.1f\n",
                    (unsigned)i, (double)p.x / 4294967296.0, (double)p.y / 4294967296.0, (double)p.z / 4294967296.0,
                    (double)su_v3_len(v) / 4294967296.0, (unsigned)ng, (unsigned)gp,
                    (double)su_v3_len(su_v3_sub(gpos, p)) / 4294967296.0);
        }
    }

    if (a.replay) {
        /* Run B: the same scene from scratch, untimed; every digest and the event log must match. */
        Sim b;
        uint32_t k = 0;
        char fs[65], fe[65];
        uint64_t ev = 0;
        replay_ok = 1;
        sim_create(&b, &a, &course);
        for (t = 0; t < a.warmup + a.ticks; t++) {
            if (step(&b, 1) != SU_OK) {
                return 1;
            }
            if ((t + 1u) % a.digest_every == 0u && t + 1u > a.warmup) {
                char d[65];
                state_digest(b.w, d, NULL, NULL);
                if (k >= nd || strcmp(d, digests[k]) != 0) {
                    replay_ok = 0;
                }
                k++;
            }
        }
        state_digest(b.w, fs, NULL, NULL);
        event_digest(&b.s, fe, &ev);
        if (strcmp(fs, final_state) != 0 || strcmp(fe, final_events) != 0 || ev != events || k != nd) {
            replay_ok = 0;
        }
        sim_free(&b);
        /* Run C: restore the world from the canonical checkpoint of run A, continue to the end. The entity runtime
         * starts empty, so only the world state is compared (the event log is compared by run B). */
        {
            Sim c;
            SuWorld *rw = NULL;
            char cs[65];
            uint32_t st;
            sim_create(&c, &a, &course);
            st = su_world_restore_canonical(&c.desc, ckpt, ckpt_len, &rw);
            if (st != SU_OK) {
                fprintf(stderr, "restore: %s\n", su_status_name(st));
                ckpt_ok = 0;
            } else {
                su_world_destroy(c.w);
                c.w = rw;
                if (step(&c, a.warmup + a.ticks - ckpt_tick) != SU_OK) {
                    return 1;
                }
                state_digest(c.w, cs, NULL, NULL);
                ckpt_ok = strcmp(cs, final_state) == 0;
            }
            sim_free(&c);
        }
    }

    {
        uint64_t *sorted = malloc((size_t)a.ticks * sizeof *sorted);
        const Space3dStats *qs = &space3d_query_ctx()->last;
        memcpy(sorted, ns, (size_t)a.ticks * sizeof *sorted);
        qsort(sorted, a.ticks, sizeof *sorted, cmp_u64);
        printf("{\"workload\":\"6dof_racer_microcosm\",\"n\":%u,\"gates\":%u,\"obstacles\":%u,\"rows\":%u,"
               "\"seed\":%" PRIu64 ",\"tick_hz\":120,\"warmup_ticks\":%u,\"timed_ticks\":%u,\"query\":%s,",
               a.n, GATES, a.obstacles, sim.rows, a.seed, a.warmup, a.ticks, a.no_query ? "false" : "true");
        printf("\"systems\":[");
        for (i = 0; i < sim.s.count; i++) {
            printf("%s{\"system_id\":\"%s\",\"kind\":\"%s\",\"semantic_hash\":\"%s\"}", i ? "," : "",
                   sim.s.sys[i].system_id, sim.s.sys[i].kind == SU_SYS_KIND_HOST ? "host" : "ir",
                   sim.s.sys[i].mod->semantic_hash_hex);
        }
        printf("],\"lanes\":%u,", sim.s.field_count);
        printf("\"tick_ns\":{\"min\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64
               ",\"max\":%" PRIu64 ",\"mean\":%" PRIu64 "},",
               sorted[0], pct(sorted, a.ticks, 50), pct(sorted, a.ticks, 95), pct(sorted, a.ticks, 99),
               sorted[a.ticks - 1u], total_ns / a.ticks);
        printf("\"budget_ns_120hz\":8333333,\"p99_fraction_of_budget\":%.4f,",
               (double)pct(sorted, a.ticks, 99) / 8333333.0);
        printf("\"final_tick\":%" PRIu64 ",\"final_state_sha256\":\"%s\",\"event_log_sha256\":\"%s\",\"events\":%" PRIu64
               ",\"digest_every\":%u,\"state_digests\":[",
               su_world_tick(sim.w), final_state, final_events, events, a.digest_every);
        for (i = 0; i < nd; i++) {
            printf("%s\"%s\"", i ? "," : "", digests[i]);
        }
        printf("],\"race\":{\"gates_passed\":%u,\"laps\":%u,\"collision_ticks\":%u},", sum_col(sim.w, 119u, a.n),
               sum_col(sim.w, 118u, a.n), sum_col(sim.w, 120u, a.n));
        printf("\"last_query\":{\"movers\":%u,\"collidables\":%u,\"sweep_axis\":%u,\"sweep_pairs\":%" PRIu64
               ",\"candidates\":%" PRIu64 ",\"narrow_tests\":%" PRIu64 ",\"contacts\":%" PRIu64 "},",
               qs->movers, qs->collidables, qs->sweep_axis, qs->sweep_pairs, qs->candidates, qs->narrow_tests,
               qs->contacts);
        printf("\"replay_from_start_identical\":%s,\"replay_from_checkpoint_identical\":%s,\"checkpoint_tick\":%u,",
               replay_ok < 0 ? "null" : (replay_ok ? "true" : "false"), ckpt_ok < 0 ? "null" : (ckpt_ok ? "true" : "false"),
               a.replay ? ckpt_tick : 0u);
#if defined(__clang__)
        printf("\"compiler\":\"clang %s\",", __clang_version__);
#elif defined(__GNUC__)
        printf("\"compiler\":\"gcc %d.%d.%d\",", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
        printf("\"compiler\":\"msvc %d\",", _MSC_VER);
#endif
#ifdef NDEBUG
        printf("\"build\":\"optimized (NDEBUG)\"}\n");
#else
        printf("\"build\":\"debug\"}\n");
#endif
        free(sorted);
    }
    free(ckpt);
    sim_free(&sim);
    return (replay_ok == 0 || ckpt_ok == 0) ? 1 : 0;
}
