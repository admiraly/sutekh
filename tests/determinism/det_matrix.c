/* Determinism matrix driver (docs/DETERMINISM_MATRIX.md, issue #141).
 *
 * Runs the matrix workloads through the public host API (include/sutekh/host.h, replay.h) on one execution tier and
 * writes, per workload, the su_host_digest of every keyframe, the SHA-256 of the sequence of all per-tick digests
 * (trace), the fast digest and event cursor of the end state, and, for workloads driven only by the command
 * stream, the SHA-256 of the canonical replay bytes. Builds that agree on every line produced bit-identical states
 * on every tick of every workload. The committed expectation tests/determinism/expected_digests.txt is the
 * cross-build oracle: every build and tier must reproduce it exactly.
 *
 * Workloads (tests/determinism/gen_workloads.py makes the capsules and the RTS fixtures):
 *   rts_duel16, rts_mixed1024  the four RTS IR systems (damage, goal, combat, move; capsules/rts) on the locked
 *                              scenes duel16 and mixed-1024. The host stands in for the native operators (which the
 *                              host API does not carry): every tick it adds the previous tick's dmg_out into the
 *                              targets' incoming (rts.scatter's rule), every 4th tick it assigns nearest-enemy
 *                              targets, every 4th tick (offset 2) formation anchors, and every 50th tick a move
 *                              order; all of it as input records (command stream, replayed)
 *   racer16                    6dof_racer_microcosm: racer.race, racer.move (IR 0.3) and space3d.query, 16 racers,
 *                              vmax commands (replayed)
 *   sixdof                     q32-heavy 6DOF rigid bodies plus a probe block on saturation-edge operands (replayed)
 *   churn                      entity churn: IR despawn_self/emit_event, host spawns and despawns between ticks, slot
 *                              reuse, generation growth, event ring eviction (per-tick digests; host spawns are not
 *                              part of the replay command stream, so this workload is not replayed)
 *
 * Usage (from the repository root):
 *   det_matrix --fixtures DIR --tier reference|planned|native [--out FILE] [--expect FILE] [--replay-dir DIR]
 *              [--play-dir DIR] [--dump-dir DIR] [--only NAME]
 *     --out FILE        write the manifest (LF line ends); "-" for stdout
 *     --expect FILE     compare with FILE; the first differing line is reported and the run fails
 *     --replay-dir DIR  write DIR/<workload>.sureplay for the replayed workloads
 *     --play-dir DIR    play DIR/<workload>.sureplay (recorded by any build) on this build: every recorded keyframe
 *                       and per-tick digest is verified (SU_REPLAY_DESYNC names the first differing tick)
 *     --dump-dir DIR    write the snapshot of every keyframe to DIR/<workload>.<tick>.suhsnap (for comparing the
 *                       states of two builds field by field after a mismatch)
 *     --max-ticks T     reduced run (sanitizer builds): every workload stops at tick T at the latest (T a multiple of
 *                       its keyframe interval). --expect then compares only the keyframe digests up to T, which
 *                       equal the full run's; the rest of the manifest describes the shorter run
 *     --expect-same F   compare the whole manifest with F (a manifest of the same run on another tier or library)
 *   native: systems the native backend refuses (entity operations, host operators) stay on the planned tier; the
 *   manifest's tier lines (not compared) say where each system ran. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_sha256.h"
#include "s3mesh_blob.h"
#include "sutekh/host.h"
#include "sutekh/replay.h"

#define MAX_LINES 4096u
#define LINE_CAP 256u
#define TIERS_CAP 160u
#define NONE32 UINT32_C(0xFFFFFFFF)
#define ONE_RAW INT64_C(4294967296)

/* ---------- small utilities ---------- */

typedef struct Bytes {
    uint8_t *p;
    size_t n;
} Bytes;

static int slurp(const char *path, Bytes *b) {
    FILE *f = fopen(path, "rb");
    long n;
    b->p = NULL;
    b->n = 0;
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    b->p = malloc((size_t)n + 1u);
    if (b->p == NULL || fread(b->p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b->p);
        b->p = NULL;
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    fclose(f);
    b->p[n] = 0;
    b->n = (size_t)n;
    return 1;
}

static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(data, 1, n, f) == n;
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    return ok;
}

/* splitmix64 (the command streams' only randomness; seeded per workload). */
typedef struct Rng {
    uint64_t s;
} Rng;

static uint64_t rng_next(Rng *r) {
    uint64_t z = (r->s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint32_t rng_below(Rng *r, uint32_t n) {
    return (uint32_t)(rng_next(r) % n);
}

/* (lo + below(whole)) + below(frac) / 2^32 as raw Q32.32, in that draw order. The draws are separate statements: two
 * calls in one expression would be evaluated in an unspecified order (C17 6.5p3), and compilers do differ, so a
 * host that derives commands from a random stream must sequence the draws (docs/DETERMINISM_MATRIX.md, D2). */
static int64_t rng_q32(Rng *r, uint32_t whole, int64_t lo, uint32_t frac) {
    int64_t w = (int64_t)rng_below(r, whole);
    int64_t f = frac != 0u ? (int64_t)rng_below(r, frac) : 0;
    return (lo + w) * INT64_C(4294967296) + f;
}

/* Two's complement reinterpretation without implementation-defined conversion. */
static int64_t i64_of(uint64_t u) {
    int64_t v;
    memcpy(&v, &u, sizeof v);
    return v;
}

static uint64_t u64_of(int64_t v) {
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static int g_failed = 0;

static void fail(const char *what, uint32_t st, const SuHostError *e) {
    fprintf(stderr, "det_matrix: %s: %s (%s)\n", what, su_host_status_name(st), e != NULL ? e->message : "");
    g_failed = 1;
}

static uint32_t field(const SuHostWorld *w, const char *name) {
    uint32_t id = 0;
    char lo[64];
    if (su_host_field_id(w, name, &id) == SU_OK) {
        return id;
    }
    snprintf(lo, sizeof lo, "%s.lo", name);
    if (su_host_field_id(w, lo, &id) == SU_OK) {
        return id;
    }
    fprintf(stderr, "det_matrix: field %s missing\n", name);
    g_failed = 1;
    return 0;
}

/* ---------- manifest ---------- */

typedef struct Manifest {
    char (*line)[LINE_CAP];
    uint32_t n;
} Manifest;

static void mf_add(Manifest *m, const char *fmt, const char *a, uint64_t b, const char *c) {
    if (m->n >= MAX_LINES) {
        fprintf(stderr, "det_matrix: manifest full\n");
        g_failed = 1;
        return;
    }
    snprintf(m->line[m->n], LINE_CAP, fmt, a, (unsigned long long)b, c);
    m->n++;
}

/* ---------- input batching ---------- */

typedef struct Batch {
    SuHostInput in[SU_HOST_INPUT_MAX_WRITES];
    uint32_t n;
    uint32_t dropped; /* writes beyond the per-tick cap (deterministic: the first SU_HOST_INPUT_MAX_WRITES win) */
} Batch;

static void b_put(Batch *b, uint64_t ent, uint32_t fid, uint32_t v) {
    if (b->n >= SU_HOST_INPUT_MAX_WRITES) {
        b->dropped++;
        return;
    }
    b->in[b->n].entity = ent;
    b->in[b->n].field_id = fid;
    b->in[b->n].value = v;
    b->n++;
}

static void b_put64(Batch *b, uint64_t ent, uint32_t fid, int64_t v) {
    uint64_t u = u64_of(v);
    if (b->n + 2u > SU_HOST_INPUT_MAX_WRITES) {
        b->dropped += 2u;
        return;
    }
    b_put(b, ent, fid, (uint32_t)u);
    b_put(b, ent, fid + 1u, (uint32_t)(u >> 32));
}

static void b_push(Batch *b, SuHostWorld *w, uint64_t tick) {
    SuHostError e;
    uint32_t st;
    if (b->n == 0u) {
        return;
    }
    st = su_host_input_push(w, tick, b->in, b->n, &e);
    if (st != SU_OK) {
        fail("input push", st, &e);
    }
    b->n = 0;
}

static const uint32_t *col(const SuHostWorld *w, uint32_t fid) {
    const uint32_t *d = NULL;
    uint64_t rows = 0;
    if (su_host_column(w, fid, &d, &rows) != SU_OK) {
        fprintf(stderr, "det_matrix: column %u\n", (unsigned)fid);
        g_failed = 1;
        return NULL;
    }
    return d;
}

/* ---------- workloads ---------- */

typedef struct Ctx Ctx;
typedef struct Workload {
    const char *name;
    uint64_t ticks;
    uint64_t keyframe;
    int replayed;
    void (*stats)(Ctx *c, const SuHostWorld *w); /* optional: accumulates the activity line */
    SuHostWorld *(*create)(Ctx *c);
    void (*inputs)(Ctx *c, SuHostWorld *w, uint64_t tick); /* queue the record of tick (before it runs) */
    void (*between)(Ctx *c, SuHostWorld *w);               /* host actions after a tick (not replayed) */
    uint32_t arg;                                          /* workload variant (Ctx.arg while it runs) */
} Workload;

struct Ctx {
    const char *fixtures;
    Rng rng;
    Batch batch;
    /* RTS */
    Bytes rts_cap[4], rts_lir[4], rts_fx;
    uint32_t rts_rows;
    uint32_t *team, *kind, *range_sq, *formation_slot, *formation_gen; /* side table (not bound by the IR systems) */
    struct {
        uint32_t hp, pos_x, pos_y, order_mode, order_x, order_y, anchor_x, anchor_y, anchor_valid, target_slot,
            target_gen, target_valid, dmg_out, dmg_target, dmg_target_gen,
            incoming;
    } rf;
    uint32_t *incoming_acc;
    /* racer */
    Bytes race_cap, race_lir, move_cap, move_lir, query, racer_fx;
    /* sixdof, churn */
    Bytes six_cap, six_lir, churn_cap, churn_lir;
    uint32_t arg;  /* the running workload's variant */
    uint32_t ccap; /* churn capacity */
    /* space3d operator variants and the motion systems */
    Bytes s3_rec[4], s3_move_cap, s3_move_lir, s3_movec_cap, s3_movec_lir;
    TBlob s3_mesh, s3_compound;
    uint64_t s3_contacts, s3_two, s3_gates, s3_prox, s3_sub, s3_trunc; /* operator activity (manifest line) */
    /* IR 0.4 probes */
    Bytes bits_cap, bits_lir, gather_cap, gather_lir;
};

static SuHostWorld *create_world(const SuHostCapsule *caps, uint32_t n, const Bytes *fx, uint64_t capacity,
                                 uint32_t hz, uint64_t max_struct, uint64_t max_events, uint64_t log_max,
                                 uint64_t seed) {
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t st;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = n;
    d.capsules = caps;
    if (fx != NULL) {
        d.fixture_json = fx->p;
        d.fixture_len = fx->n;
    }
    d.max_entities = capacity;
    d.tick_rate_hz = hz;
    d.max_struct_per_tick = max_struct;
    d.max_events_per_tick = max_events;
    d.event_log_max = log_max;
    d.seed = seed;
    st = su_host_world_create(&d, &w, &e);
    if (st != SU_OK) {
        fail("world create", st, &e);
        return NULL;
    }
    return w;
}

static SuHostCapsule ir_capsule(const Bytes *cap, const Bytes *lir) {
    SuHostCapsule c;
    c.capsule_json = cap->p;
    c.capsule_len = cap->n;
    c.lir_json = lir != NULL ? lir->p : NULL;
    c.lir_len = lir != NULL ? lir->n : 0u;
    return c;
}

/* ---- RTS ---- */

static const char *const k_rts_sys[4] = {"damage", "goal", "combat", "move"};

static int rts_load(Ctx *c, const char *scene_file) {
    char path[1024];
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        free(c->rts_cap[i].p);
        free(c->rts_lir[i].p);
        snprintf(path, sizeof path, "tests/determinism/rts/%s.capsule.json", k_rts_sys[i]);
        if (!slurp(path, &c->rts_cap[i])) {
            return 0;
        }
        snprintf(path, sizeof path, "capsules/rts/%s.lir.json", k_rts_sys[i]);
        if (!slurp(path, &c->rts_lir[i])) {
            return 0;
        }
    }
    free(c->rts_fx.p);
    snprintf(path, sizeof path, "%s/%s.json", c->fixtures, scene_file);
    if (!slurp(path, &c->rts_fx)) {
        return 0;
    }
    {
        Bytes side;
        char *s;
        char *end;
        uint32_t r;
        snprintf(path, sizeof path, "%s/%s.side.txt", c->fixtures, scene_file);
        if (!slurp(path, &side)) {
            return 0;
        }
        s = (char *)side.p;
        c->rts_rows = (uint32_t)strtoull(s, &end, 10);
        {
            uint32_t **cols[5];
            uint32_t k;
            cols[0] = &c->team;
            cols[1] = &c->kind;
            cols[2] = &c->range_sq;
            cols[3] = &c->formation_slot;
            cols[4] = &c->formation_gen;
            free(c->incoming_acc);
            c->incoming_acc = calloc(c->rts_rows, sizeof *c->incoming_acc);
            for (k = 0; k < 5u; k++) {
                free(*cols[k]);
                *cols[k] = calloc(c->rts_rows, sizeof **cols[k]);
                if (*cols[k] == NULL || c->incoming_acc == NULL) {
                    free(side.p);
                    return 0;
                }
            }
            for (r = 0; r < c->rts_rows; r++) {
                for (k = 0; k < 5u; k++) {
                    s = end;
                    (*cols[k])[r] = (uint32_t)strtoull(s, &end, 10);
                }
            }
        }
        free(side.p);
    }
    return 1;
}

static SuHostWorld *rts_create(Ctx *c) {
    SuHostCapsule caps[4];
    SuHostWorld *w;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        caps[i] = ir_capsule(&c->rts_cap[i], &c->rts_lir[i]);
    }
    w = create_world(caps, 4, &c->rts_fx, c->rts_rows, 30, c->rts_rows, 0, 4096, 20261026u);
    if (w == NULL) {
        return NULL;
    }
    c->rf.hp = field(w, "hp");
    c->rf.pos_x = field(w, "pos_x");
    c->rf.pos_y = field(w, "pos_y");
    c->rf.order_mode = field(w, "order_mode");
    c->rf.order_x = field(w, "order_x");
    c->rf.order_y = field(w, "order_y");
    c->rf.anchor_x = field(w, "anchor_x");
    c->rf.anchor_y = field(w, "anchor_y");
    c->rf.anchor_valid = field(w, "anchor_valid");
    c->rf.target_slot = field(w, "target_slot");
    c->rf.target_gen = field(w, "target_gen");
    c->rf.target_valid = field(w, "target_valid");
    c->rf.dmg_out = field(w, "dmg_out");
    c->rf.dmg_target = field(w, "dmg_target");
    c->rf.dmg_target_gen = field(w, "dmg_target_gen");
    c->rf.incoming = field(w, "incoming");
    return w;
}

/* Writes v to (slot, fid) when it differs from the committed value (keeps records small). */
static void rts_set(Ctx *c, const SuHostWorld *w, uint64_t ent, uint32_t slot, uint32_t fid, uint32_t v) {
    const uint32_t *d = col(w, fid);
    if (d != NULL && d[slot] != v) {
        b_put(&c->batch, ent, fid, v);
    }
}

static void rts_inputs(Ctx *c, SuHostWorld *w, uint64_t tick) {
    uint32_t n = c->rts_rows;
    uint32_t s, t;
    uint8_t *alive = malloc(n);
    uint64_t *ent = malloc((size_t)n * sizeof *ent);
    const uint32_t *px = col(w, c->rf.pos_x), *py = col(w, c->rf.pos_y), *hp = col(w, c->rf.hp);
    const uint32_t *rsq = c->range_sq;
    if (alive == NULL || ent == NULL || px == NULL || py == NULL || hp == NULL) {
        free(alive);
        free(ent);
        g_failed = 1;
        return;
    }
    (void)su_host_alive(w, 0, n, alive);
    for (s = 0; s < n; s++) {
        ent[s] = 0;
        if (alive[s]) {
            (void)su_host_entity_at(w, s, &ent[s]);
        }
    }
    /* rts.scatter's rule on the previous tick's combat output: saturating sum of dmg_out into each live unit target
     * of matching generation, in ascending shooter slot order. */
    {
        const uint32_t *out = col(w, c->rf.dmg_out), *tgt = col(w, c->rf.dmg_target);
        const uint32_t *tg = col(w, c->rf.dmg_target_gen);
        memset(c->incoming_acc, 0, (size_t)n * sizeof *c->incoming_acc);
        for (s = 0; s < n; s++) {
            uint32_t ts;
            if (!alive[s] || out[s] == 0u || tgt[s] == NONE32 || tgt[s] >= n) {
                continue;
            }
            ts = tgt[s];
            if (!alive[ts] || (uint32_t)(ent[ts] >> 32) != tg[s] || c->kind[ts] != 1u) {
                continue;
            }
            c->incoming_acc[ts] =
                c->incoming_acc[ts] + out[s] < c->incoming_acc[ts] ? NONE32 : c->incoming_acc[ts] + out[s];
        }
        for (s = 0; s < n; s++) {
            if (alive[s]) {
                rts_set(c, w, ent[s], s, c->rf.incoming, c->incoming_acc[s]);
            }
        }
    }
    /* Nearest live enemy unit (squared distance, ties to the lowest slot); valid within range_sq. */
    if (tick % 4u == 0u) {
        for (s = 0; s < n; s++) {
            uint64_t best = UINT64_MAX;
            uint32_t bs = NONE32;
            if (!alive[s] || c->kind[s] != 1u) {
                continue;
            }
            for (t = 0; t < n; t++) {
                uint64_t dx, dy, d2;
                if (!alive[t] || c->kind[t] != 1u || c->team[t] == c->team[s]) {
                    continue;
                }
                dx = px[t] > px[s] ? (uint64_t)px[t] - px[s] : (uint64_t)px[s] - px[t]; /* < 2^32 */
                dy = py[t] > py[s] ? (uint64_t)py[t] - py[s] : (uint64_t)py[s] - py[t];
                d2 = dx * dx + dy * dy; /* < 2^65 cannot occur: positions are below 2^31 (rts_u32_v1 domain) */
                if (d2 < best) {
                    best = d2;
                    bs = t;
                }
            }
            if (bs != NONE32 && best <= rsq[s]) {
                rts_set(c, w, ent[s], s, c->rf.target_slot, bs);
                rts_set(c, w, ent[s], s, c->rf.target_gen, (uint32_t)(ent[bs] >> 32));
                rts_set(c, w, ent[s], s, c->rf.target_valid, 1u);
            } else {
                rts_set(c, w, ent[s], s, c->rf.target_slot, NONE32);
                rts_set(c, w, ent[s], s, c->rf.target_gen, 0u);
                rts_set(c, w, ent[s], s, c->rf.target_valid, 0u);
            }
        }
    }
    /* Formation anchors: the anchor row's position when it is alive with the recorded generation. */
    if (tick % 4u == 2u) {
        const uint32_t *fs = c->formation_slot, *fg = c->formation_gen;
        for (s = 0; s < n; s++) {
            uint32_t a;
            if (!alive[s] || fs[s] == NONE32 || fs[s] >= n) {
                continue;
            }
            a = fs[s];
            if (alive[a] && (uint32_t)(ent[a] >> 32) == fg[s]) {
                rts_set(c, w, ent[s], s, c->rf.anchor_x, px[a]);
                rts_set(c, w, ent[s], s, c->rf.anchor_y, py[a]);
                rts_set(c, w, ent[s], s, c->rf.anchor_valid, 1u);
            } else {
                rts_set(c, w, ent[s], s, c->rf.anchor_valid, 0u);
            }
        }
    }
    /* A player command every 50 ticks: attack-move of every 7th live row of one team to a random point. */
    if (tick % 50u == 25u) {
        uint32_t team, ox, oy, phase;
        team = rng_below(&c->rng, 2u);
        ox = 4096u + rng_below(&c->rng, 24576u);
        oy = 4096u + rng_below(&c->rng, 24576u);
        phase = rng_below(&c->rng, 7u);
        for (s = phase; s < n; s += 7u) {
            if (alive[s] && c->team[s] == team) {
                rts_set(c, w, ent[s], s, c->rf.order_mode, 2u);
                rts_set(c, w, ent[s], s, c->rf.order_x, ox);
                rts_set(c, w, ent[s], s, c->rf.order_y, oy);
            }
        }
    }
    free(alive);
    free(ent);
    b_push(&c->batch, w, tick);
}

/* ---- racer ---- */

static SuHostWorld *racer_create(Ctx *c) {
    SuHostCapsule caps[3];
    caps[0] = ir_capsule(&c->race_cap, &c->race_lir);
    caps[1] = ir_capsule(&c->move_cap, &c->move_lir);
    caps[2] = ir_capsule(&c->query, NULL);
    /* The entity caps of racer_microcosm (bench/space3d/racer_microcosm.c) for 16 racers. */
    return create_world(caps, 3, &c->racer_fx, 0, 120, 1, 2u * 16u + 2u, 1048576u, 6u);
}

static void racer_inputs(Ctx *c, SuHostWorld *w, uint64_t tick) {
    uint32_t vmax = field(w, "vmax");
    uint32_t kind = field(w, "kind");
    const uint32_t *k = col(w, kind);
    uint32_t s;
    if (tick % 90u != 45u || k == NULL) {
        return;
    }
    /* Pace commands: a new top speed (60..1000 m/s, non-integer) for a third of the racers. */
    for (s = 0; s < (uint32_t)su_host_capacity(w); s++) {
        uint64_t e = 0;
        if (k[s] != 1u || su_host_entity_at(w, s, &e) != SU_OK || rng_below(&c->rng, 3u) != 0u) {
            continue;
        }
        b_put64(&c->batch, e, vmax, rng_q32(&c->rng, 940u, 60, 1u << 31));
    }
    b_push(&c->batch, w, tick);
}

/* ---- sixdof ---- */

#define SIX_ROWS 48u

static const int64_t k_edges[] = {
    0, 1, -1, 2, ONE_RAW, -ONE_RAW, ONE_RAW / 2, -(ONE_RAW / 2), ONE_RAW + 1, ONE_RAW - 1,
    INT64_MAX, INT64_MIN, INT64_MIN + 1, INT64_MAX - 1, INT64_C(1) << 62, -(INT64_C(1) << 62), (INT64_C(1) << 62) - 1,
    -(INT64_C(1) << 62) - 1, INT64_C(1) << 52, (INT64_C(1) << 52) - 1, (INT64_C(1) << 52) + 1, INT64_C(1) << 40,
    INT64_C(3037000499), INT64_C(3037000500), INT64_C(0x7FFFFFFF), INT64_C(0x80000000), INT64_C(0xFFFFFFFF),
    INT64_C(3074457345618258602), INT64_C(-3074457345618258603), INT64_C(46340) << 32, (INT64_C(46341) << 32),
    INT64_C(0x0000B504F333F9DE), INT64_C(0x00000000B504F334), INT64_C(-0x00000000B504F334), INT64_C(1) << 31,
    INT64_C(3) << 31, -(INT64_C(3) << 31), INT64_C(5) << 31,
};
#define EDGE_COUNT (sizeof k_edges / sizeof k_edges[0])

/* An edge value, or a random raw value of random magnitude and sign. */
static int64_t six_operand(Rng *r) {
    uint64_t x = rng_next(r);
    uint32_t pick = (uint32_t)(x & 3u);
    if (pick == 0u) {
        return k_edges[(x >> 8) % EDGE_COUNT];
    }
    {
        uint64_t m = rng_next(r) >> (uint32_t)((x >> 8) % 64u);
        int64_t v = i64_of(m >> 1);
        return ((x >> 16) & 1u) != 0u ? -v : v;
    }
}

static void six_spawn_values(SuHostValue *v, uint32_t *n, uint32_t fid, int64_t x) {
    uint64_t u = u64_of(x);
    v[*n].field_id = fid;
    v[*n].value = (uint32_t)u;
    v[*n + 1u].field_id = fid + 1u;
    v[*n + 1u].value = (uint32_t)(u >> 32);
    *n += 2u;
}

static SuHostWorld *six_create(Ctx *c) {
    SuHostCapsule cap = ir_capsule(&c->six_cap, &c->six_lir);
    SuHostWorld *w = create_world(&cap, 1, NULL, 64, 120, 0, 0, 64, 7u);
    uint32_t s;
    if (w == NULL) {
        return NULL;
    }
    for (s = 0; s < SIX_ROWS; s++) {
        SuHostValue v[32];
        uint32_t n = 0;
        uint64_t ent = 0;
        SuHostError e;
        uint32_t st;
        int64_t vmax;
        v[n].field_id = field(w, "kind");
        v[n].value = s % 6u == 5u ? 0u : 1u; /* every 6th row inactive (select paths) */
        n++;
        six_spawn_values(v, &n, field(w, "pos_x"), rng_q32(&c->rng, 200000u, -100000, 0));
        six_spawn_values(v, &n, field(w, "pos_y"), six_operand(&c->rng) / 4);
        six_spawn_values(v, &n, field(w, "pos_z"), (int64_t)rng_below(&c->rng, 1u << 30));
        six_spawn_values(v, &n, field(w, "vel_x"), rng_q32(&c->rng, 2000u, -1000, 0));
        /* identity, or an unnormalized random quaternion */
        six_spawn_values(v, &n, field(w, "rot_w"), s % 4u == 0u ? ONE_RAW : six_operand(&c->rng));
        six_spawn_values(v, &n, field(w, "rot_x"), s % 4u == 0u ? 0 : six_operand(&c->rng) >> 8);
        six_spawn_values(v, &n, field(w, "angvel_z"), (int64_t)rng_below(&c->rng, 16u) * ONE_RAW - 8 * ONE_RAW);
        /* top speed: ordinary, huge (positions saturate at MAX/MIN), zero (clamp to the origin rule) */
        vmax = s % 8u == 3u ? INT64_MAX : s % 8u == 6u ? 0 : (int64_t)(100u + rng_below(&c->rng, 900u)) * ONE_RAW;
        six_spawn_values(v, &n, field(w, "vmax"), vmax);
        st = su_host_spawn(w, v, n, s + 1u, &ent, &e);
        if (st != SU_OK) {
            fail("sixdof spawn", st, &e);
        }
    }
    return w;
}

static void six_inputs(Ctx *c, SuHostWorld *w, uint64_t tick) {
    uint32_t pa = field(w, "probe_a"), pb = field(w, "probe_b"), pc = field(w, "probe_c"), pn = field(w, "probe_n");
    uint32_t tx = field(w, "thrust_x"), tq = field(w, "torque_x");
    uint32_t s;
    for (s = 0; s < SIX_ROWS; s++) {
        uint64_t e = 0;
        if (su_host_entity_at(w, s, &e) != SU_OK) {
            continue;
        }
        b_put64(&c->batch, e, pa, six_operand(&c->rng));
        b_put64(&c->batch, e, pb, six_operand(&c->rng));
        b_put64(&c->batch, e, pc, six_operand(&c->rng));
        b_put(&c->batch, e, pn, rng_below(&c->rng, 72u)); /* shift counts past 63 included */
        if ((tick + s) % 8u == 0u) {
            uint32_t ax;
            /* thrust up to +-2^20 m/s^2 for the huge-vmax rows, else +-200; torque up to +-40 rad/s^2 */
            int64_t tmax = s % 8u == 3u ? INT64_C(1) << 20 : 200;
            for (ax = 0; ax < 3u; ax++) {
                b_put64(&c->batch, e, tx + 2u * ax, rng_q32(&c->rng, (uint32_t)(2 * tmax), -tmax, 0));
                b_put64(&c->batch, e, tq + 2u * ax, rng_q32(&c->rng, 80u, -40, 1u << 30));
            }
        }
    }
    b_push(&c->batch, w, tick);
}

/* ---- churn ---- */

/* Capacity Ctx.ccap: 96 (churn) or 3072 (churn3k: three SU_CHUNK_SLOTS chunks, so the per-chunk structural
 * command and event buffers are merged across chunks every tick). */
#define CHURN_CAP (c->ccap)

static void churn_spawn(Ctx *c, SuHostWorld *w) {
    SuHostValue v[24];
    uint32_t n = 0;
    int64_t drain;
    uint64_t ent = 0;
    SuHostError e;
    uint32_t st;
    v[n].field_id = field(w, "kind");
    v[n].value = 1u + rng_below(&c->rng, 3u);
    n++;
    v[n].field_id = field(w, "life");
    v[n].value = 0u; /* life 0 (one spawn in nine): dies in its first tick */
    if (rng_below(&c->rng, 9u) != 0u) {
        v[n].value = 1u + rng_below(&c->rng, 200u);
    }
    n++;
    six_spawn_values(v, &n, field(w, "energy"), rng_q32(&c->rng, 64u, 0, 1u << 31));
    drain = (int64_t)rng_below(&c->rng, 1u << 30);
    if (rng_below(&c->rng, 5u) == 0u) {
        drain += INT64_MAX / 2; /* drains a full tank in one tick */
    }
    six_spawn_values(v, &n, field(w, "drain"), drain);
    six_spawn_values(v, &n, field(w, "pos_x"), six_operand(&c->rng));
    six_spawn_values(v, &n, field(w, "vel_y"), six_operand(&c->rng) >> 4);
    six_spawn_values(v, &n, field(w, "vel_z"), (int64_t)rng_below(&c->rng, 1000u) * ONE_RAW);
    st = su_host_spawn(w, v, n, rng_below(&c->rng, 1000u), &ent, &e);
    if (st != SU_OK && st != SU_E_RESOURCE_LIMIT) {
        fail("churn spawn", st, &e);
    }
}

static SuHostWorld *churn_create(Ctx *c) {
    SuHostCapsule cap = ir_capsule(&c->churn_cap, &c->churn_lir);
    SuHostWorld *w;
    uint32_t s;
    c->ccap = c->arg;
    w = create_world(&cap, 1, NULL, CHURN_CAP, 60, CHURN_CAP, 2u * CHURN_CAP, CHURN_CAP == 96u ? 128u : 4096u, 11u);
    if (w == NULL) {
        return NULL;
    }
    for (s = 0; s < (CHURN_CAP == 96u ? 64u : CHURN_CAP - 272u); s++) {
        churn_spawn(c, w);
    }
    return w;
}

static void churn_inputs(Ctx *c, SuHostWorld *w, uint64_t tick) {
    uint32_t energy = field(w, "energy");
    uint32_t s;
    if (tick % 3u != 0u) {
        return;
    }
    for (s = 0; s < CHURN_CAP; s++) {
        uint64_t e = 0;
        if (su_host_entity_at(w, s, &e) == SU_OK && rng_below(&c->rng, 4u) == 0u) {
            b_put64(&c->batch, e, energy, (int64_t)rng_below(&c->rng, 32u) * ONE_RAW);
        }
    }
    b_push(&c->batch, w, tick);
}

/* Between ticks: spawn 0..3 entities into the lowest free slots; every 5th tick despawn a random live entity. */
static void churn_between(Ctx *c, SuHostWorld *w) {
    uint32_t k = rng_below(&c->rng, CHURN_CAP == 96u ? 4u : 48u), i, d, nd = CHURN_CAP / 96u;
    uint64_t tick = su_host_tick(w);
    for (i = 0; i < k; i++) {
        uint32_t s;
        uint64_t e = 0;
        for (s = 0; s < CHURN_CAP && su_host_entity_at(w, s, &e) == SU_OK; s++) {
        }
        if (s == CHURN_CAP) {
            break;
        }
        churn_spawn(c, w);
    }
    for (d = 0; tick % 5u == 0u && d < nd; d++) { /* every 5th tick: nd random live entities despawned */
        uint32_t start = rng_below(&c->rng, CHURN_CAP), s;
        for (s = 0; s < CHURN_CAP; s++) {
            uint64_t e = 0;
            if (su_host_entity_at(w, (start + s) % CHURN_CAP, &e) == SU_OK) {
                SuHostError err;
                uint32_t st = su_host_despawn(w, e, &err);
                if (st != SU_OK) {
                    fail("churn despawn", st, &err);
                }
                break;
            }
        }
    }
}

/* ---- space3d operator variants ----
 * One world per operator: space3d.query (arg 0), query_prox (1), query_shape (2), query_compound (3), each with
 * det.s3move (det.s3move_c for compound) before it. 96 movers fly through 40 m of spheres, rotating boxes, rotating
 * capsules, rotating static meshes (two triangle meshes from operator data) and gates in a 24 m arena over a 1 km
 * floor box, and
 * bounce on the operator's contacts of the previous tick, so every result lane feeds back into motion. Three boxes
 * have D1-style extents: the two with INT64_MIN extents get empty bounds and never reach the narrow phase (the
 * INT64_MIN product itself is covered by test_det_regress); the one with -1 m extents reaches it (6 to 2,650 sphere
 * sweeps per run, counted 2026-10-26). Commands every 30 ticks kick movers. */

#define S3_ROWS 224u
#define S3_MOVERS 96u

static const char *const k_s3_rec[4] = {"profiles/space3d/space3d_query.capsule.json",
                                        "profiles/space3d/space3d_query_prox.capsule.json",
                                        "profiles/space3d/space3d_query_shape.capsule.json",
                                        "profiles/space3d/space3d_query_compound.capsule.json"};

static void s3_blobs(Ctx *c) {
    SuV3 v[16];
    uint32_t t[72];
    TGeom g[2];
    TPart p[2];
    TMesh m[2], cm[2];
    TSub u[5];
    SuV3 h0, h1;
    uint32_t i;
    h0.x = h0.y = h0.z = 3 * ONE_RAW;
    h1.x = 6 * ONE_RAW;
    h1.y = ONE_RAW / 2;
    h1.z = 4 * ONE_RAW;
    tbox_verts(h0, v);
    tbox_verts(h1, v + 8);
    tbox_tris(t);
    tbox_tris(t + 36);
    for (i = 0; i < 2u; i++) {
        g[i].nv = 8;
        g[i].nt = 12;
        g[i].v = v + 8u * i;
        g[i].t = t + 36u * i;
        p[i].geom = i;
        p[i].pos.x = p[i].pos.y = p[i].pos.z = 0;
        p[i].rot.w = ONE_RAW;
        p[i].rot.x = p[i].rot.y = p[i].rot.z = 0;
        p[i].scale = ONE_RAW;
        m[i].first = i;
        m[i].count = 1;
    }
    p[1].rot.w = INT64_C(3037000500); /* 90 degrees about y */
    p[1].rot.y = INT64_C(3037000500);
    tb_build(&c->s3_mesh, g, 2, p, 2, m, 2);
    /* Compounds: 0 = a long capsule and a sphere; 1 = a cross of three capsules. */
    memset(u, 0, sizeof u);
    u[0].a.z = -2 * ONE_RAW;
    u[0].b.z = 2 * ONE_RAW;
    u[0].r = ONE_RAW / 2;
    u[1].a.y = u[1].b.y = ONE_RAW;
    u[1].r = ONE_RAW * 3 / 4;
    u[2].a.x = -2 * ONE_RAW;
    u[2].b.x = 2 * ONE_RAW;
    u[2].r = ONE_RAW / 4;
    u[3].a.y = -2 * ONE_RAW;
    u[3].b.y = 2 * ONE_RAW;
    u[3].r = ONE_RAW / 4;
    u[4].a.z = -ONE_RAW;
    u[4].b.z = ONE_RAW;
    u[4].r = ONE_RAW / 3;
    cm[0].first = 0;
    cm[0].count = 2;
    cm[1].first = 2;
    cm[1].count = 3;
    tb_build2(&c->s3_compound, g, 2, p, 2, m, 2, u, 5, cm, 2);
}

/* Appends (fid, value) for a lane the world has; skips lanes the variant's record does not declare. */
static void s3_put(const SuHostWorld *w, SuHostValue *v, uint32_t *n, const char *name, int64_t x, int is64) {
    uint32_t id = 0;
    char lo[64];
    snprintf(lo, sizeof lo, "%s.lo", name);
    if (su_host_field_id(w, is64 ? lo : name, &id) != SU_OK) {
        return;
    }
    if (is64) {
        six_spawn_values(v, n, id, x);
    } else {
        v[*n].field_id = id;
        v[*n].value = (uint32_t)u64_of(x);
        (*n)++;
    }
}

/* Uniform raw q32 in [-m, m] metres. */
static int64_t s3_coord(Rng *r, uint32_t m) {
    return rng_q32(r, 2u * m, -(int64_t)m, 1u << 31) - (int64_t)(1u << 30);
}

static void s3_spawn(Ctx *c, SuHostWorld *w, uint32_t kind, uint32_t slot) {
    SuHostValue v[48];
    uint32_t n = 0;
    uint64_t ent = 0;
    SuHostError e;
    uint32_t st;
    int64_t px = s3_coord(&c->rng, 22), py = s3_coord(&c->rng, 22), pz = s3_coord(&c->rng, 22);
    s3_put(w, v, &n, "kind", kind, 0);
    s3_put(w, v, &n, "pos_x", px, 1);
    s3_put(w, v, &n, "pos_y", py, 1);
    s3_put(w, v, &n, "pos_z", pz, 1);
    s3_put(w, v, &n, "prev_x", px, 1);
    s3_put(w, v, &n, "prev_y", py, 1);
    s3_put(w, v, &n, "prev_z", pz, 1);
    s3_put(w, v, &n, "rot_w", ONE_RAW, 1);
    s3_put(w, v, &n, "angvel_x", rng_q32(&c->rng, 4u, -2, 0), 1);
    s3_put(w, v, &n, "angvel_y", rng_q32(&c->rng, 4u, -2, 1u << 31), 1);
    s3_put(w, v, &n, "angvel_z", rng_q32(&c->rng, 2u, -1, 1u << 30), 1);
    if (kind == 1u) {
        s3_put(w, v, &n, "vel_x", rng_q32(&c->rng, 50u, -25, 1u << 31), 1);
        s3_put(w, v, &n, "vel_y", rng_q32(&c->rng, 50u, -25, 1u << 31), 1);
        s3_put(w, v, &n, "vel_z", rng_q32(&c->rng, 50u, -25, 1u << 31), 1);
        s3_put(w, v, &n, "radius", ONE_RAW + rng_q32(&c->rng, 1u, 0, 1u << 31), 1);
        s3_put(w, v, &n, "next_gate", rng_below(&c->rng, 6u), 0);
        s3_put(w, v, &n, "prox_margin", rng_q32(&c->rng, 3u, 1, 0), 1);
        s3_put(w, v, &n, "z0", -rng_q32(&c->rng, 2u, 0, 1u << 31), 1);
        s3_put(w, v, &n, "z1", rng_q32(&c->rng, 2u, 0, 1u << 31), 1);
        s3_put(w, v, &n, "compound", slot % 3u == 2u ? 0xFFFFFFFFu : slot % 3u, 0); /* 0xFFFFFFFF: plain capsule */
        s3_put(w, v, &n, "rot0_w", ONE_RAW, 1);
    } else if (kind == 2u) {
        s3_put(w, v, &n, "radius", rng_q32(&c->rng, 4u, 2, 1u << 31), 1);
    } else if (kind == 3u) {
        s3_put(w, v, &n, "half_x", rng_q32(&c->rng, 4u, 0, 1u << 31) + ONE_RAW / 2, 1);
        s3_put(w, v, &n, "half_y", rng_q32(&c->rng, 4u, 0, 1u << 31) + ONE_RAW / 2, 1);
        s3_put(w, v, &n, "half_z", rng_q32(&c->rng, 4u, 0, 1u << 31) + ONE_RAW / 2, 1);
    } else if (kind == 4u) {
        s3_put(w, v, &n, "radius", ONE_RAW / 2 + rng_q32(&c->rng, 2u, 0, 0), 1);
        s3_put(w, v, &n, "half_z", rng_q32(&c->rng, 4u, 1, 1u << 31), 1);
    } else if (kind == 8u) {
        s3_put(w, v, &n, "gate_index", slot % 2u, 0); /* mesh index */
    } else { /* gates 5..7 */
        s3_put(w, v, &n, "gate_index", slot % 6u, 0);
        s3_put(w, v, &n, "half_x", rng_q32(&c->rng, 4u, 3, 0), 1);
        s3_put(w, v, &n, "half_y", rng_q32(&c->rng, 4u, 3, 0), 1);
        s3_put(w, v, &n, "radius", rng_q32(&c->rng, 3u, 3, 0), 1);
    }
    st = su_host_spawn(w, v, n, slot, &ent, &e);
    if (st != SU_OK) {
        fail("space3d spawn", st, &e);
    }
}

/* A static box at (x, y, z) with the given half extents (raw), not rotating. */
static void s3_box(SuHostWorld *w, int64_t x, int64_t y, int64_t z, int64_t hx, int64_t hy, int64_t hz) {
    SuHostValue v[32];
    uint32_t n = 0;
    uint64_t ent = 0;
    SuHostError e;
    uint32_t st;
    s3_put(w, v, &n, "kind", 3, 0);
    s3_put(w, v, &n, "pos_x", x, 1);
    s3_put(w, v, &n, "pos_y", y, 1);
    s3_put(w, v, &n, "pos_z", z, 1);
    s3_put(w, v, &n, "prev_x", x, 1);
    s3_put(w, v, &n, "prev_y", y, 1);
    s3_put(w, v, &n, "prev_z", z, 1);
    s3_put(w, v, &n, "rot_w", ONE_RAW, 1);
    s3_put(w, v, &n, "half_x", hx, 1);
    s3_put(w, v, &n, "half_y", hy, 1);
    s3_put(w, v, &n, "half_z", hz, 1);
    st = su_host_spawn(w, v, n, 0, &ent, &e);
    if (st != SU_OK) {
        fail("space3d box", st, &e);
    }
}

static SuHostWorld *s3_create(Ctx *c) {
    SuHostCapsule caps[2];
    SuHostWorld *w;
    SuHostError e;
    uint32_t st, s, i;
    static const uint32_t layout[][2] = {{1u, S3_MOVERS}, {2u, 36u}, {3u, 30u}, {4u, 24u}, {8u, 8u},
                                         {5u, 2u},        {6u, 2u},  {7u, 2u}};
    caps[0] = c->arg == 3u ? ir_capsule(&c->s3_movec_cap, &c->s3_movec_lir)
                           : ir_capsule(&c->s3_move_cap, &c->s3_move_lir);
    caps[1] = ir_capsule(&c->s3_rec[c->arg], NULL);
    w = create_world(caps, 2, NULL, S3_ROWS, 60, 1, 4u * S3_ROWS, 4096, 31u);
    if (w == NULL) {
        return NULL;
    }
    st = c->arg == 3u ? su_host_set_operator_data(w, NULL, c->s3_compound.p, c->s3_compound.n, &e)
                      : su_host_set_operator_data(w, NULL, c->s3_mesh.p, c->s3_mesh.n, &e);
    if (st != SU_OK) {
        fail("operator data", st, &e);
    }
    s = 0;
    for (i = 0; i < sizeof layout / sizeof layout[0]; i++) {
        uint32_t k;
        for (k = 0; k < layout[i][1]; k++) {
            s3_spawn(c, w, layout[i][0], s++);
        }
    }
    /* Floor: the top face of a 1 km box at y = -24 m. */
    s3_box(w, 0, -INT64_C(1024) * ONE_RAW, 0, INT64_C(1000) * ONE_RAW, INT64_C(1000) * ONE_RAW,
           INT64_C(1000) * ONE_RAW);
    /* D1-style extents (empty bounds: never narrow-phase candidates). */
    s3_box(w, 0, 0, 0, INT64_MIN, ONE_RAW, ONE_RAW);
    s3_box(w, 5 * ONE_RAW, 0, 0, -ONE_RAW, -ONE_RAW, -ONE_RAW);
    s3_box(w, 0, 5 * ONE_RAW, 0, INT64_MIN, INT64_MIN, INT64_MIN);
    return w;
}

/* Operator activity after a tick: contacts reported, movers with two contacts, gate crossings, proximity hits,
 * compound sub-shape contacts and truncation flags. Written to the manifest, so the expectation pins it and a
 * workload whose operator went quiet is visible. */
static void s3_stats(Ctx *c, const SuHostWorld *w) {
    uint32_t idc = 0, idg = 0, idp = 0, ids = 0, idf = 0, s;
    const uint32_t *hc, *gd, *ps = NULL, *sb = NULL, *fl;
    uint64_t rows = su_host_capacity(w);
    if (su_host_field_id(w, "hit_count", &idc) != SU_OK || su_host_field_id(w, "gate_dir", &idg) != SU_OK ||
        su_host_field_id(w, "hit_flags", &idf) != SU_OK) {
        return;
    }
    hc = col(w, idc);
    gd = col(w, idg);
    fl = col(w, idf);
    if (su_host_field_id(w, "prox_slot", &idp) == SU_OK) {
        ps = col(w, idp);
    }
    if (su_host_field_id(w, "c0_sub", &ids) == SU_OK) {
        sb = col(w, ids);
    }
    for (s = 0; s < rows; s++) {
        c->s3_contacts += hc[s];
        c->s3_two += hc[s] == 2u;
        c->s3_gates += gd[s] != 0u;
        c->s3_trunc += fl[s] != 0u;
        c->s3_prox += ps != NULL && ps[s] != NONE32;
        c->s3_sub += sb != NULL && sb[s] != NONE32 && (sb[s] & 0xFFFFu) != 0xFFFFu;
    }
}

static void s3_inputs(Ctx *c, SuHostWorld *w, uint64_t tick) {
    uint32_t vx = field(w, "vel_x"), s;
    if (tick % 30u != 15u) {
        return;
    }
    /* A command: a velocity kick (up to 40 m/s per axis) for every 4th mover, from a random phase. */
    for (s = rng_below(&c->rng, 4u); s < S3_MOVERS; s += 4u) {
        uint64_t e = 0;
        uint32_t ax;
        if (su_host_entity_at(w, s, &e) != SU_OK) {
            continue;
        }
        for (ax = 0; ax < 3u; ax++) {
            b_put64(&c->batch, e, vx + 2u * ax, rng_q32(&c->rng, 80u, -40, 1u << 31));
        }
    }
    b_push(&c->batch, w, tick);
}

/* ---- IR 0.4 probes: det.bits04 (every family op, bit ops, conversions; native-lowerable) and det.gather04
 * (gathers, find_eq_u32, row filter; VM only). Edge operands every tick. ---- */

#define PROBE_ROWS 64u

static const int32_t k_i32_edges[] = {0, 1, -1, 2, -2, INT32_MIN, INT32_MAX, INT32_MIN + 1, INT32_MAX - 1,
                                      46340, 46341, -46341, 65536, -65536, 31, 32, 33, 63, 64};
static const uint32_t k_u32_edges[] = {0u, 1u, 2u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x80000000u, 0x7FFFFFFFu,
                                       31u, 32u, 33u, 63u, 64u, 65535u, 65536u};

static uint32_t probe_u32(Rng *r) {
    uint64_t x = rng_next(r);
    return (x & 3u) == 0u ? k_u32_edges[(x >> 8) % (sizeof k_u32_edges / sizeof k_u32_edges[0])]
                          : (uint32_t)(rng_next(r) >> (uint32_t)((x >> 8) % 33u));
}

static int32_t probe_i32(Rng *r) {
    uint64_t x = rng_next(r);
    uint32_t u;
    int32_t v;
    if ((x & 3u) == 0u) {
        return k_i32_edges[(x >> 8) % (sizeof k_i32_edges / sizeof k_i32_edges[0])];
    }
    u = (uint32_t)(rng_next(r) >> (uint32_t)((x >> 8) % 33u));
    memcpy(&v, &u, sizeof v);
    return v;
}

static void probe_values(Ctx *c, const SuHostWorld *w, uint64_t e, int spawn, SuHostValue *v, uint32_t *n) {
    static const char *const i32n[3] = {"ia", "ib", "ic"};
    static const char *const i64n[3] = {"la", "lb", "lc"};
    static const char *const q32n[3] = {"qa", "qb", "qc"};
    uint32_t k;
    (void)e;
    (void)spawn;
    s3_put(w, v, n, "n", rng_below(&c->rng, 72u), 0);
    s3_put(w, v, n, "ua", probe_u32(&c->rng), 0);
    s3_put(w, v, n, "ub", probe_u32(&c->rng), 0);
    s3_put(w, v, n, "key", 1u + rng_below(&c->rng, 24u), 0); /* never 0: dead rows hold 0 */
    s3_put(w, v, n, "slot_a", rng_below(&c->rng, 8u) == 0u ? probe_u32(&c->rng) : rng_below(&c->rng, PROBE_ROWS), 0);
    for (k = 0; k < 3u; k++) {
        int32_t x = probe_i32(&c->rng);
        uint32_t u;
        memcpy(&u, &x, sizeof u);
        s3_put(w, v, n, i32n[k], u, 0);
        s3_put(w, v, n, i64n[k], six_operand(&c->rng), 1);
        s3_put(w, v, n, q32n[k], six_operand(&c->rng), 1);
    }
}

static SuHostWorld *probe_create(Ctx *c) {
    SuHostCapsule caps[2];
    SuHostWorld *w;
    uint32_t s;
    caps[0] = ir_capsule(&c->bits_cap, &c->bits_lir);
    caps[1] = ir_capsule(&c->gather_cap, &c->gather_lir);
    w = create_world(caps, 2, NULL, PROBE_ROWS, 120, 0, 0, 64, 41u);
    if (w == NULL) {
        return NULL;
    }
    for (s = 0; s < PROBE_ROWS - 4u; s++) { /* the last four slots stay dead (gathers and find_eq see zeros) */
        SuHostValue v[32];
        uint32_t n = 0;
        uint64_t ent = 0;
        SuHostError e;
        uint32_t st;
        s3_put(w, v, &n, "kind", s % 4u == 3u ? 2u : 1u, 0); /* kind 2 rows are outside gather04's row filter */
        probe_values(c, w, 0, 1, v, &n);
        st = su_host_spawn(w, v, n, s, &ent, &e);
        if (st != SU_OK) {
            fail("probe spawn", st, &e);
        }
    }
    return w;
}

static void probe_inputs(Ctx *c, SuHostWorld *w, uint64_t tick) {
    uint32_t s;
    (void)tick;
    for (s = 0; s < PROBE_ROWS; s++) {
        SuHostValue v[32];
        uint32_t n = 0, k;
        uint64_t e = 0;
        if (su_host_entity_at(w, s, &e) != SU_OK) {
            continue;
        }
        probe_values(c, w, e, 0, v, &n);
        for (k = 0; k < n; k++) {
            b_put(&c->batch, e, v[k].field_id, v[k].value);
        }
    }
    b_push(&c->batch, w, tick);
}

/* ---------- the runner ---------- */

/* The compiler that built this driver (the library is built by the same toolchain in every CI leg). */
static const char *compiler_id(void) {
#if defined(__clang__) && defined(_MSC_VER)
    return "clang " __clang_version__ " (msvc target)";
#elif defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
#define SU_STR2(x) #x
#define SU_STR(x) SU_STR2(x)
    return "msvc " SU_STR(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

static const char *tier_name(uint32_t t) {
    switch (t) {
    case SU_HOST_TIER_REFERENCE:
        return "reference";
    case SU_HOST_TIER_PLANNED:
        return "planned";
    case SU_HOST_TIER_NATIVE:
        return "native";
    case SU_HOST_TIER_OPERATOR:
        return "operator";
    default:
        return "?";
    }
}

/* --tier mixed: per system, in schedule order, native / reference / planned in turn (reference / planned without the
 * native backend), so adjacent systems of one world run on different tiers. */
#define TIER_MIXED 99u

/* Puts every IR system on tier (native: where the backend lowers it, else planned; mixed: see TIER_MIXED) and
 * writes the placement ("system=tier,...") to out. The placement is compared with expected_tiers.txt, so a native
 * refusal that silently falls back to planned is a failure, not a pass. */
static void apply_tier(SuHostWorld *w, uint32_t tier_mode, char *out, size_t cap) {
    uint32_t i, n = su_host_system_count(w);
    size_t used = 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        uint32_t cur = 0;
        SuHostError e;
        const char *sid = NULL;
        (void)su_host_system_info(w, i, &sid, NULL);
        (void)su_host_system_tier(w, i, &cur);
        if (cur != SU_HOST_TIER_OPERATOR) {
            static const uint32_t cycle3[3] = {SU_HOST_TIER_NATIVE, SU_HOST_TIER_REFERENCE, SU_HOST_TIER_PLANNED};
            static const uint32_t cycle2[2] = {SU_HOST_TIER_REFERENCE, SU_HOST_TIER_PLANNED};
            uint32_t tier = tier_mode != TIER_MIXED ? tier_mode
                            : su_host_native_available() ? cycle3[i % 3u]
                                                         : cycle2[i % 2u];
            uint32_t want = tier;
            uint32_t st = su_host_system_set_tier(w, i, want, &e);
            if (st == SU_E_CAPABILITY_DENIED && tier == SU_HOST_TIER_NATIVE) {
                want = SU_HOST_TIER_PLANNED;
                st = su_host_system_set_tier(w, i, want, &e);
            }
            if (st != SU_OK) {
                fail("set tier", st, &e);
            }
            (void)su_host_system_tier(w, i, &cur);
        }
        used += (size_t)snprintf(out + used, used < cap ? cap - used : 0u, "%s%s=%s", i == 0u ? "" : ",",
                                 sid != NULL ? sid : "?", tier_name(cur));
        if (used >= cap) {
            used = cap - 1u;
        }
    }
}

static void hex32(const uint8_t d[32], char out[65]) {
    su_host_hex(d, out);
}

typedef struct Opts {
    const char *fixtures;
    uint32_t tier;
    const char *out;
    const char *expect;
    const char *replay_dir;
    const char *play_dir;
    const char *dump_dir;
    const char *only;
    const char *expect_tiers;
    const char *expect_same;
    uint64_t max_ticks; /* 0: every workload's full length; otherwise its first max_ticks ticks (reduced run) */
    int rollback;
} Opts;

static const char *mode_name(uint32_t tier_mode) {
    if (tier_mode == TIER_MIXED) {
        return su_host_native_available() ? "mixed" : "mixed_vm";
    }
    return tier_name(tier_mode);
}

/* Mid-run rollback (docs/DETERMINISM_MATRIX.md section 2): the snapshot of tick r is restored (a) in place into the
 * world that ran, (b) into a fresh world on another tier, and the same command stream (the generator state of tick r
 * included) is re-simulated to the end; every per-tick digest must equal the first pass. */
static int rollback_check(const Workload *wl, Ctx *c, const Opts *o, SuHostWorld *w, const uint8_t *snap,
                          size_t snap_len, Rng at_r, uint64_t r, uint8_t (*digs)[32]) {
    int pass;
    int ok = 1;
    for (pass = 0; pass < 2 && ok; pass++) {
        SuHostWorld *x = w;
        SuHostError e;
        uint32_t st;
        uint64_t t;
        char tl[TIERS_CAP];
        if (pass == 1) {
            x = wl->create(c);
            if (x == NULL) {
                return 0;
            }
            apply_tier(x, o->tier == SU_HOST_TIER_REFERENCE ? SU_HOST_TIER_PLANNED : SU_HOST_TIER_REFERENCE, tl,
                       sizeof tl);
            st = su_host_snapshot_restore(x, snap, snap_len, &e);
        } else {
            st = su_host_snapshot_restore_inplace(x, snap, snap_len, &e);
        }
        c->rng = at_r;
        c->batch.n = 0;
        if (st != SU_OK || su_host_tick(x) != r) {
            fail(pass == 0 ? "rollback restore in place" : "rollback restore into a fresh world", st, &e);
            ok = 0;
        }
        for (t = r; ok && t < wl->ticks; t++) {
            uint64_t done = 0;
            uint8_t d[32];
            wl->inputs(c, x, su_host_tick(x));
            st = su_host_step(x, 1, &done, &e);
            if (st != SU_OK || done != 1u) {
                fail("rollback step", st, &e);
                ok = 0;
                break;
            }
            if (wl->between != NULL) {
                wl->between(c, x);
            }
            (void)su_host_digest(x, d);
            if (memcmp(d, digs[t + 1u], 32) != 0) {
                fprintf(stderr, "det_matrix: %s: rollback %s from tick %llu diverges at tick %llu\n", wl->name,
                        pass == 0 ? "in place" : "into a fresh world on another tier", (unsigned long long)r,
                        (unsigned long long)(t + 1u));
                g_failed = 1;
                ok = 0;
            }
        }
        if (x != w) {
            su_host_world_destroy(x);
        }
    }
    return ok;
}

static void dump_snapshot(const SuHostWorld *w, const char *dir, const char *name) {
    size_t size = 0, got = 0;
    uint8_t *buf;
    char path[1024];
    if (su_host_snapshot_size(w, &size) != SU_OK || (buf = malloc(size)) == NULL) {
        g_failed = 1;
        return;
    }
    if (su_host_snapshot_write(w, buf, size, &got) == SU_OK) {
        snprintf(path, sizeof path, "%s/%s.%llu.suhsnap", dir, name, (unsigned long long)su_host_tick(w));
        if (!write_file(path, buf, got)) {
            fprintf(stderr, "det_matrix: cannot write %s\n", path);
            g_failed = 1;
        }
    }
    free(buf);
}

static void run_workload(const Workload *wl, Ctx *c, const Opts *o, Manifest *m, Manifest *tiers) {
    SuHostWorld *w;
    SuReplayWriter *rw = NULL;
    SuHostError e;
    SuSha256 trace;
    uint8_t d[32];
    char hex[65];
    char tl[TIERS_CAP];
    uint64_t t;
    uint32_t st;
    uint64_t roll_r;
    uint8_t (*digs)[32];
    uint8_t *roll_snap = NULL;
    size_t roll_len = 0;
    Rng roll_rng;

    roll_rng.s = 0;
    c->rng.s = UINT64_C(0x5EED000000000000) ^ (uint64_t)strlen(wl->name) * UINT64_C(0x9E3779B97F4A7C15);
    {
        const char *p;
        for (p = wl->name; *p != '\0'; p++) {
            c->rng.s = c->rng.s * UINT64_C(1099511628211) + (uint8_t)*p;
        }
    }
    c->batch.n = 0;
    c->batch.dropped = 0;
    c->arg = wl->arg;
    c->s3_contacts = c->s3_two = c->s3_gates = c->s3_prox = c->s3_sub = c->s3_trunc = 0;
    w = wl->create(c);
    if (w == NULL) {
        g_failed = 1;
        return;
    }
    apply_tier(w, o->tier, tl, sizeof tl);
    if (tiers->n < 64u) {
        snprintf(tiers->line[tiers->n++], LINE_CAP, "%s %s %s", mode_name(o->tier), wl->name, tl);
    }
    roll_r = (wl->ticks / 2u / wl->keyframe) * wl->keyframe + wl->keyframe / 3u; /* mid-run, off a keyframe */
    digs = o->rollback ? calloc((size_t)wl->ticks + 1u, sizeof *digs) : NULL;
    if (wl->replayed) {
        SuReplayOptions ro;
        memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.keyframe_interval = wl->keyframe;
        ro.digest_interval = 1;
        st = su_replay_writer_begin(w, &ro, &rw, &e);
        if (st != SU_OK) {
            fail("replay begin", st, &e);
            su_host_world_destroy(w);
            return;
        }
    }
    su_sha256_init(&trace);
    (void)su_host_digest(w, d);
    su_sha256_update(&trace, d, 32);
    hex32(d, hex);
    mf_add(m, "%s kf %llu %s", wl->name, 0, hex);
    for (t = 0; t < wl->ticks && !g_failed; t++) {
        uint64_t done = 0;
        wl->inputs(c, w, su_host_tick(w));
        st = rw != NULL ? su_replay_writer_step(rw, w, 1, &done, &e) : su_host_step(w, 1, &done, &e);
        if (st != SU_OK || done != 1u) {
            fail("step", st, &e);
            break;
        }
        if (wl->between != NULL) {
            wl->between(c, w);
        }
        if (wl->stats != NULL) {
            wl->stats(c, w);
        }
        (void)su_host_digest(w, d);
        su_sha256_update(&trace, d, 32);
        if (digs != NULL) {
            memcpy(digs[t + 1u], d, 32);
            if (t + 1u == roll_r) {
                size_t got = 0;
                roll_rng = c->rng;
                if (su_host_snapshot_size(w, &roll_len) == SU_OK && (roll_snap = malloc(roll_len)) != NULL) {
                    (void)su_host_snapshot_write(w, roll_snap, roll_len, &got);
                }
            }
        }
        if ((t + 1u) % wl->keyframe == 0u || t + 1u == wl->ticks) {
            hex32(d, hex);
            mf_add(m, "%s kf %llu %s", wl->name, su_host_tick(w), hex);
            if (o->dump_dir != NULL) {
                dump_snapshot(w, o->dump_dir, wl->name);
            }
        }
    }
    su_sha256_final(&trace, d);
    hex32(d, hex);
    mf_add(m, "%s trace %llu %s", wl->name, wl->ticks, hex);
    {
        uint64_t fast = 0;
        char fx[17];
        (void)su_host_digest_fast(w, &fast);
        snprintf(fx, sizeof fx, "%016llx", (unsigned long long)fast);
        mf_add(m, "%s fast %llu %s", wl->name, su_host_tick(w), fx);
        snprintf(fx, sizeof fx, "%llu", (unsigned long long)su_host_event_next_seq(w));
        mf_add(m, "%s events %llu %s", wl->name, su_host_tick(w), fx);
        snprintf(fx, sizeof fx, "%u", (unsigned)c->batch.dropped);
        mf_add(m, "%s dropped_writes %llu %s", wl->name, su_host_tick(w), fx);
        if (wl->stats != NULL) {
            char act[160];
            snprintf(act, sizeof act, "contacts=%llu,two=%llu,gates=%llu,prox=%llu,sub=%llu,flags=%llu",
                     (unsigned long long)c->s3_contacts, (unsigned long long)c->s3_two,
                     (unsigned long long)c->s3_gates, (unsigned long long)c->s3_prox,
                     (unsigned long long)c->s3_sub, (unsigned long long)c->s3_trunc);
            mf_add(m, "%s activity %llu %s", wl->name, su_host_tick(w), act);
        }
    }
    if (rw != NULL) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        st = su_replay_writer_finish(rw, w, &bytes, &len);
        if (st != SU_OK) {
            fail("replay finish", st, NULL);
        } else {
            SuSha256 h;
            su_sha256_init(&h);
            su_sha256_update(&h, bytes, len);
            su_sha256_final(&h, d);
            hex32(d, hex);
            mf_add(m, "%s replay %llu %s", wl->name, (uint64_t)len, hex);
            if (o->replay_dir != NULL) {
                char path[1024];
                snprintf(path, sizeof path, "%s/%s.sureplay", o->replay_dir, wl->name);
                if (!write_file(path, bytes, len)) {
                    fprintf(stderr, "det_matrix: cannot write %s\n", path);
                    g_failed = 1;
                }
            }
            /* Same-build playback from the start keyframe (every recorded digest verified). */
            {
                SuReplay *r = NULL;
                SuHostWorld *pw = wl->create(c);
                uint64_t done = 0;
                st = su_replay_open(bytes, len, &r, &e);
                if (st == SU_OK && pw != NULL) {
                    apply_tier(pw, o->tier, tl, sizeof tl);
                    st = su_replay_seek(r, pw, 0, &e);
                    if (st == SU_OK) {
                        st = su_replay_play(r, pw, UINT64_MAX, &done, &e);
                    }
                }
                if (st != SU_OK || pw == NULL || done != wl->ticks) {
                    fail("same-build playback", st, &e);
                }
                su_replay_close(r);
                su_host_world_destroy(pw);
            }
            su_replay_free(bytes);
        }
        su_replay_writer_destroy(rw);
    }
    if (digs != NULL && !g_failed) {
        if (roll_snap == NULL || !rollback_check(wl, c, o, w, roll_snap, roll_len, roll_rng, roll_r, digs)) {
            fprintf(stderr, "det_matrix: %s: rollback check failed\n", wl->name);
            g_failed = 1;
        } else {
            printf("det_matrix: %s: rollback from tick %llu (in place, and into a fresh world on another tier): every "
                   "per-tick digest matches\n",
                   wl->name, (unsigned long long)roll_r);
        }
    }
    free(roll_snap);
    free(digs);
    su_host_world_destroy(w);
}

/* Plays DIR/<name>.sureplay recorded by another build. */
static void play_foreign(const Workload *wl, Ctx *c, const Opts *o) {
    char path[1024];
    char tl[TIERS_CAP];
    Bytes b;
    SuReplay *r = NULL;
    SuReplayInfo info;
    SuHostWorld *w;
    SuHostError e;
    uint64_t done = 0;
    uint32_t st;
    snprintf(path, sizeof path, "%s/%s.sureplay", o->play_dir, wl->name);
    if (!slurp(path, &b)) {
        g_failed = 1;
        return;
    }
    c->rng.s = 0;
    c->arg = wl->arg;
    w = wl->create(c);
    st = su_replay_open(b.p, b.n, &r, &e);
    if (st != SU_OK || w == NULL) {
        fail("open foreign replay", st, &e);
    } else {
        apply_tier(w, o->tier, tl, sizeof tl);
        (void)su_replay_info(r, &info);
        st = su_replay_seek(r, w, 0, &e);
        if (st == SU_OK) {
            st = su_replay_play(r, w, UINT64_MAX, &done, &e);
        }
        if (st != SU_OK) {
            fprintf(stderr, "det_matrix: %s: foreign replay diverges at tick %llu (%s)\n", wl->name,
                    (unsigned long long)e.tick, e.message);
            g_failed = 1;
        } else if (done != wl->ticks || info.end_tick != wl->ticks ||
                   info.keyframe_count != wl->ticks / wl->keyframe + 1u ||
                   info.digest_count != wl->ticks - wl->ticks / wl->keyframe) {
            fprintf(stderr, "det_matrix: %s: foreign replay is not this workload's recording (%llu ticks played, %u "
                            "keyframes, %u digests)\n",
                    wl->name, (unsigned long long)done, (unsigned)info.keyframe_count, (unsigned)info.digest_count);
            g_failed = 1;
        } else {
            printf("det_matrix: %s: played %s (%llu ticks, %u keyframes, %u digests): every digest matches\n",
                   wl->name, path, (unsigned long long)done, (unsigned)info.keyframe_count,
                   (unsigned)info.digest_count);
        }
    }
    su_replay_close(r);
    su_host_world_destroy(w);
    free(b.p);
}

/* Every placement line ("<mode> <workload> <system=tier,...>") must equal the line of the same mode and workload in
 * the expectation file; a missing line is a failure too. */
static int compare_tiers(const Manifest *t, const char *path) {
    Bytes b;
    uint32_t k;
    int ok = 1;
    if (!slurp(path, &b)) {
        return 0;
    }
    for (k = 0; k < t->n; k++) {
        const char *ln = t->line[k];
        const char *sp = strchr(ln, ' ');
        const char *sp2 = sp != NULL ? strchr(sp + 1, ' ') : NULL;
        const char *s = (const char *)b.p;
        int found = 0;
        size_t key = sp2 != NULL ? (size_t)(sp2 - ln) + 1u : strlen(ln);
        while (*s != '\0') {
            const char *nl = strchr(s, '\n');
            size_t len = nl != NULL ? (size_t)(nl - s) : strlen(s);
            if (len > 0u && s[len - 1u] == '\r') {
                len--;
            }
            if (len >= key && memcmp(s, ln, key) == 0) {
                found = 1;
                if (len != strlen(ln) || memcmp(s, ln, len) != 0) {
                    fprintf(stderr, "det_matrix: PLACEMENT MISMATCH\n  expected: %.*s\n  got:      %s\n", (int)len, s,
                            ln);
                    ok = 0;
                }
                break;
            }
            if (nl == NULL) {
                break;
            }
            s = nl + 1;
        }
        if (!found) {
            fprintf(stderr, "det_matrix: no expected placement for: %s\n", ln);
            ok = 0;
        }
    }
    free(b.p);
    if (ok) {
        printf("det_matrix: %u placements equal %s\n", (unsigned)t->n, path);
    }
    return ok;
}

/* In a reduced run (--max-ticks T) compared with a full-length expectation, only the keyframe lines of ticks up to T
 * are the same: the first T ticks of every workload are the full run's first T ticks, while the trace, end-state and
 * replay lines describe the shorter run. Every line counts otherwise. */
static int prefix_line(const char *ln, size_t len, uint64_t max_ticks) {
    char name[64], kind[16], buf[LINE_CAP];
    unsigned long long tick = 0;
    if (max_ticks == 0u) {
        return 1;
    }
    if (len >= sizeof buf) {
        len = sizeof buf - 1u;
    }
    memcpy(buf, ln, len);
    buf[len] = '\0';
    return sscanf(buf, "%63s %15s %llu", name, kind, &tick) == 3 && strcmp(kind, "kf") == 0 && tick <= max_ticks;
}

/* Compares the manifest with the expectation, line by line (CR ignored). max_ticks != 0: only the keyframe prefix
 * (prefix_line) of both. */
static int compare(const Manifest *m, const char *path, const Opts *o, uint64_t max_ticks) {
    Bytes b;
    char *s, *nl;
    uint32_t i = 0;
    int bad = 0;
    if (!slurp(path, &b)) {
        return 0;
    }
    while (i < m->n && !prefix_line(m->line[i], strlen(m->line[i]), max_ticks)) {
        i++;
    }
    s = (char *)b.p;
    while (*s != '\0' && !bad) {
        size_t len;
        nl = strchr(s, '\n');
        len = nl != NULL ? (size_t)(nl - s) : strlen(s);
        if (len > 0u && s[len - 1u] == '\r') {
            len--;
        }
        if (len > 0u && s[0] != '#') {
            if (o->only != NULL && (strncmp(s, o->only, strlen(o->only)) != 0 || s[strlen(o->only)] != ' ')) {
                /* another workload */
            } else if (!prefix_line(s, len, max_ticks)) {
                /* not part of a reduced run's comparable prefix */
            } else if (i >= m->n || strlen(m->line[i]) != len || memcmp(m->line[i], s, len) != 0) {
                fprintf(stderr, "det_matrix: MISMATCH\n  expected: %.*s\n  got:      %s\n", (int)len, s,
                        i < m->n ? m->line[i] : "(nothing)");
                bad = 1;
                if (i < m->n) {
                    char name[64];
                    unsigned long long tick = 0;
                    if (sscanf(m->line[i], "%63s kf %llu", name, &tick) == 2) {
                        fprintf(stderr, "  first differing keyframe: %s tick %llu%s\n", name, tick,
                                o->dump_dir != NULL ? " (keyframe snapshots are in the dump directory)" : "");
                    }
                    fprintf(stderr, "  exact tick: record with --replay-dir on one build, play with --play-dir on "
                                    "the other\n");
                }
            } else {
                i++;
                while (i < m->n && !prefix_line(m->line[i], strlen(m->line[i]), max_ticks)) {
                    i++;
                }
            }
        }
        if (nl == NULL) {
            break;
        }
        s = nl + 1;
    }
    if (!bad && i != m->n) {
        fprintf(stderr, "det_matrix: MISMATCH: %u lines produced, %u expected\n", (unsigned)m->n, (unsigned)i);
        bad = 1;
    }
    free(b.p);
    return !bad;
}

int main(int argc, char **argv) {
    static Ctx c;
    Opts o;
    Manifest m, tiers;
    int i;
    uint32_t k;
    Workload wls[] = {
        {"rts_duel16", 600, 60, 1, NULL, rts_create, rts_inputs, NULL, 0},
        {"rts_mixed1024", 600, 60, 1, NULL, rts_create, rts_inputs, NULL, 0},
        {"racer16", 720, 60, 1, NULL, racer_create, racer_inputs, NULL, 0},
        {"sixdof", 1200, 60, 1, NULL, six_create, six_inputs, NULL, 0},
        {"churn", 900, 60, 0, NULL, churn_create, churn_inputs, churn_between, 96},
        {"churn3k", 300, 60, 0, NULL, churn_create, churn_inputs, churn_between, 3072},
        {"s3_query", 600, 60, 1, s3_stats, s3_create, s3_inputs, NULL, 0},
        {"s3_prox", 600, 60, 1, s3_stats, s3_create, s3_inputs, NULL, 1},
        {"s3_shape", 600, 60, 1, s3_stats, s3_create, s3_inputs, NULL, 2},
        {"s3_compound", 600, 60, 1, s3_stats, s3_create, s3_inputs, NULL, 3},
        {"probe04", 600, 60, 1, NULL, probe_create, probe_inputs, NULL, 0},
    };
    const uint32_t nwl = (uint32_t)(sizeof wls / sizeof wls[0]);

    memset(&o, 0, sizeof o);
    o.tier = SU_HOST_TIER_PLANNED;
    o.rollback = 1;
    for (i = 1; i < argc; i += 2) { /* every option takes one value */
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v == NULL) {
            fprintf(stderr, "det_matrix: %s needs a value\n", a);
            return 2;
        }
        if (strcmp(a, "--fixtures") == 0) {
            o.fixtures = v;
        } else if (strcmp(a, "--tier") == 0) {
            if (strcmp(v, "reference") == 0) {
                o.tier = SU_HOST_TIER_REFERENCE;
            } else if (strcmp(v, "planned") == 0) {
                o.tier = SU_HOST_TIER_PLANNED;
            } else if (strcmp(v, "native") == 0) {
                o.tier = SU_HOST_TIER_NATIVE;
            } else if (strcmp(v, "mixed") == 0) {
                o.tier = TIER_MIXED;
            } else {
                fprintf(stderr, "det_matrix: unknown tier %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--out") == 0) {
            o.out = v;
        } else if (strcmp(a, "--expect") == 0) {
            o.expect = v;
        } else if (strcmp(a, "--replay-dir") == 0) {
            o.replay_dir = v;
        } else if (strcmp(a, "--play-dir") == 0) {
            o.play_dir = v;
        } else if (strcmp(a, "--dump-dir") == 0) {
            o.dump_dir = v;
        } else if (strcmp(a, "--only") == 0) {
            o.only = v;
        } else if (strcmp(a, "--expect-tiers") == 0) {
            o.expect_tiers = v;
        } else if (strcmp(a, "--rollback") == 0) {
            o.rollback = strcmp(v, "0") != 0;
        } else if (strcmp(a, "--expect-same") == 0) {
            o.expect_same = v;
        } else if (strcmp(a, "--max-ticks") == 0) {
            char *end = NULL;
            o.max_ticks = strtoull(v, &end, 10);
            if (v[0] < '1' || v[0] > '9' || end == NULL || *end != '\0') {
                fprintf(stderr, "det_matrix: --max-ticks needs a positive tick count\n");
                return 2;
            }
        } else {
            fprintf(stderr, "det_matrix: unknown option %s\n", a);
            return 2;
        }
    }
    if (o.fixtures == NULL) {
        fprintf(stderr, "usage: det_matrix --fixtures DIR --tier reference|planned|native|mixed [--out FILE]"
                        " [--expect FILE] [--expect-tiers FILE] [--replay-dir DIR] [--play-dir DIR] [--dump-dir DIR]"
                        " [--only NAME] [--rollback 0|1] [--max-ticks T] [--expect-same FILE]\n");
        return 2;
    }
    if (o.tier == SU_HOST_TIER_NATIVE && !su_host_native_available()) {
        printf("det_matrix: this library has no native backend (SUTEKH_NATIVE=OFF); tier native not run\n");
        return 77;
    }
    c.fixtures = o.fixtures;
    printf("det_matrix: built by %s; native backend %s\n", compiler_id(), su_host_native_available() ? "yes" : "no");
    {
        const char *path = "tests/determinism/fixtures/racer16.json";
        uint32_t v;
        for (v = 0; v < 4u; v++) {
            if (!slurp(k_s3_rec[v], &c.s3_rec[v])) {
                return 2;
            }
        }
        if (!slurp("tests/determinism/s3move.capsule.json", &c.s3_move_cap) ||
            !slurp("tests/determinism/s3move.lir.json", &c.s3_move_lir) ||
            !slurp("tests/determinism/s3move_c.capsule.json", &c.s3_movec_cap) ||
            !slurp("tests/determinism/s3move_c.lir.json", &c.s3_movec_lir) ||
            !slurp("tests/determinism/bits04.capsule.json", &c.bits_cap) ||
            !slurp("tests/determinism/bits04.lir.json", &c.bits_lir) ||
            !slurp("tests/determinism/gather04.capsule.json", &c.gather_cap) ||
            !slurp("tests/determinism/gather04.lir.json", &c.gather_lir)) {
            return 2;
        }
        s3_blobs(&c);
        if (!slurp("capsules/space3d_racer/race.capsule.json", &c.race_cap) ||
            !slurp("capsules/space3d_racer/race.lir.json", &c.race_lir) ||
            !slurp("capsules/space3d_racer/move.capsule.json", &c.move_cap) ||
            !slurp("capsules/space3d_racer/move.lir.json", &c.move_lir) ||
            !slurp("profiles/space3d/space3d_query.capsule.json", &c.query) || !slurp(path, &c.racer_fx) ||
            !slurp("tests/determinism/sixdof.capsule.json", &c.six_cap) ||
            !slurp("tests/determinism/sixdof.lir.json", &c.six_lir) ||
            !slurp("tests/determinism/churn.capsule.json", &c.churn_cap) ||
            !slurp("tests/determinism/churn.lir.json", &c.churn_lir)) {
            return 2;
        }
    }
    m.line = calloc(MAX_LINES, LINE_CAP);
    tiers.line = calloc(64, LINE_CAP);
    m.n = tiers.n = 0;
    if (m.line == NULL || tiers.line == NULL) {
        free(m.line);
        free(tiers.line);
        return 2;
    }
    if (o.max_ticks != 0u) {
        for (k = 0; k < nwl; k++) {
            if (wls[k].ticks > o.max_ticks) {
                if (o.max_ticks % wls[k].keyframe != 0u) {
                    fprintf(stderr, "det_matrix: --max-ticks %llu is not a multiple of %s's keyframe interval %llu\n",
                            (unsigned long long)o.max_ticks, wls[k].name, (unsigned long long)wls[k].keyframe);
                    free(m.line);
                    free(tiers.line);
                    return 2;
                }
                wls[k].ticks = o.max_ticks;
            }
        }
        printf("det_matrix: reduced run: every workload stops at tick %llu at the latest\n",
               (unsigned long long)o.max_ticks);
    }
    for (k = 0; k < nwl && !g_failed; k++) {
        if (o.only != NULL && strcmp(o.only, wls[k].name) != 0) {
            continue;
        }
        if (wls[k].create == rts_create &&
            !rts_load(&c, strcmp(wls[k].name, "rts_duel16") == 0 ? "rts_duel16" : "rts_mixed1024")) {
            free(m.line);
            free(tiers.line);
            return 2;
        }
        if (o.play_dir != NULL) {
            if (wls[k].replayed) {
                play_foreign(&wls[k], &c, &o);
            }
            continue;
        }
        run_workload(&wls[k], &c, &o, &m, &tiers);
    }
    if (o.play_dir == NULL) {
        for (k = 0; k < tiers.n; k++) {
            printf("det_matrix: placement %s\n", tiers.line[k]);
        }
        if (o.expect_tiers != NULL && !g_failed && !compare_tiers(&tiers, o.expect_tiers)) {
            g_failed = 1;
        }
        if (o.out != NULL) {
            FILE *f = strcmp(o.out, "-") == 0 ? stdout : fopen(o.out, "wb");
            if (f == NULL) {
                fprintf(stderr, "det_matrix: cannot write %s\n", o.out);
                free(m.line);
                free(tiers.line);
                return 2;
            }
            fprintf(f, "# sutekh determinism matrix v1 (tests/determinism/det_matrix.c): workload kind tick value\n");
            for (k = 0; k < m.n; k++) {
                fprintf(f, "%s\n", m.line[k]);
            }
            if (f != stdout) {
                fclose(f);
            }
        }
        if (o.expect != NULL && !g_failed) {
            if (!compare(&m, o.expect, &o, o.max_ticks)) {
                g_failed = 1;
            } else if (o.max_ticks != 0u) {
                printf("det_matrix: tier %s: keyframe digests up to tick %llu equal %s\n", mode_name(o.tier),
                       (unsigned long long)o.max_ticks, o.expect);
            } else {
                printf("det_matrix: tier %s: %u lines equal %s\n", mode_name(o.tier), (unsigned)m.n, o.expect);
            }
        }
        if (o.expect_same != NULL && !g_failed) {
            if (!compare(&m, o.expect_same, &o, 0u)) {
                g_failed = 1;
            } else {
                printf("det_matrix: tier %s: %u lines equal %s\n", mode_name(o.tier), (unsigned)m.n, o.expect_same);
            }
        }
    }
    printf("det_matrix: %s\n", g_failed ? "FAILED" : "ok");
    free(m.line);
    free(tiers.line);
    return g_failed ? 1 : 0;
}
