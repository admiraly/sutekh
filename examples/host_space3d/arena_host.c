/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Embedding Sutekh with a host operator: a headless game loop over the in-process host API (sutekh_host DLL).
 *
 * The game has its own schema ("arena", examples/host_space3d/gen_arena.py) and binds space3d.query to it
 * schema-relatively (arena_query.capsule.json: every operator lane "binds" a game lane). Four ships fly through
 * three disc gates; one meets a sphere, two meet each other. Each tick the host pushes the ships' input axes,
 * steps, and reads that tick's events: kind 1 gate passed (a = gate index, b = score), kind 2 bump (a = other
 * slot, b = contacts), both emitted by arena.score from the operator's results of the previous tick.
 *
 * --rollback-check runs a client-side rollback drill every 30 ticks: restore (in place) the snapshot of 8 ticks
 * ago, re-push those ticks' inputs, re-simulate 8 ticks in one call, and require every per-tick fast digest and
 * every event to equal the first pass. --native puts arena.move on the native tier when the library has it.
 *
 * --respond (post-query ordering, 2026-10-18) uses space3d.query_prox and adds arena.respond, an IR system ordered
 * after the operator in the same tick: a ship that moved into its first contact is put back to the contact point
 * and bounced, so the committed state of the contact tick does not penetrate; a ship with an obstacle within its
 * 3 m proximity margin and no contact emits a near-miss event (kind 3). Every tick the host measures the deepest
 * penetration of a ship into the sphere or another ship in committed state; with --respond it must stay within
 * 1e-6 m (reported, and checked, in both modes).
 *
 *   arena_host [--root <repo>] [--ticks 600] [--rollback-check] [--native] [--respond] [--coast] [--quiet]
 *              [--expect-digest <hex>] */
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"

#define SHIPS 4u
#define GATES 3u
#define HIST 16u /* ring of per-tick snapshots and digests (rollback window 8 < HIST) */
#define DRILL_BACK 8u
#define Q32(m) ((int64_t)(m) * INT64_C(4294967296))

typedef struct Ids {
    uint32_t kind, next_gate, gate_index, pos[3], rot[4], radius, in_ax, in_ay, in_az, vel[3], score, bumps;
    uint32_t prox_margin; /* --respond only */
} Ids;

static int g_respond = 0;

static uint8_t *slurp(const char *root, const char *rel, size_t *len) {
    char path[1024];
    FILE *f;
    long sz;
    uint8_t *b = NULL;
    snprintf(path, sizeof path, "%s/examples/host_space3d/%s", root, rel);
    f = fopen(path, "rb");
    *len = 0;
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0) {
        rewind(f);
        b = malloc((size_t)sz + 1u);
        if (b != NULL && fread(b, 1, (size_t)sz, f) == (size_t)sz) {
            *len = (size_t)sz;
        } else {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    return b;
}

static int id_of(const SuHostWorld *w, const char *name, uint32_t *id) {
    if (su_host_field_id(w, name, id) != SU_OK) {
        fprintf(stderr, "field %s missing\n", name);
        return 0;
    }
    return 1;
}

static int resolve(const SuHostWorld *w, Ids *ids) {
    static const char *const axes[3] = {"x", "y", "z"};
    static const char *const quat[4] = {"w", "x", "y", "z"};
    char name[64];
    int ok = 1, k;
    ok &= id_of(w, "a_kind", &ids->kind);
    ok &= id_of(w, "a_next_gate", &ids->next_gate);
    ok &= id_of(w, "a_gate_index", &ids->gate_index);
    ok &= id_of(w, "a_radius.lo", &ids->radius);
    ok &= id_of(w, "in_ax", &ids->in_ax);
    ok &= id_of(w, "in_ay", &ids->in_ay);
    ok &= id_of(w, "in_az", &ids->in_az);
    ok &= id_of(w, "score", &ids->score);
    ok &= id_of(w, "bumps", &ids->bumps);
    if (g_respond) {
        ok &= id_of(w, "a_prox_margin.lo", &ids->prox_margin);
    }
    for (k = 0; k < 3; k++) {
        snprintf(name, sizeof name, "a_pos_%s.lo", axes[k]);
        ok &= id_of(w, name, &ids->pos[k]);
        snprintf(name, sizeof name, "vel_%s.lo", axes[k]);
        ok &= id_of(w, name, &ids->vel[k]);
    }
    for (k = 0; k < 4; k++) {
        snprintf(name, sizeof name, "a_rot_%s.lo", quat[k]);
        ok &= id_of(w, name, &ids->rot[k]);
    }
    return ok;
}

/* Appends the two u32 cells of a q32 field (low lane id) to a spawn value list. */
static void put_q(SuHostValue *v, uint32_t *n, uint32_t lo_id, int64_t raw) {
    v[*n].field_id = lo_id;
    v[*n].value = (uint32_t)((uint64_t)raw & 0xFFFFFFFFu);
    (*n)++;
    v[*n].field_id = lo_id + 1u;
    v[*n].value = (uint32_t)((uint64_t)raw >> 32);
    (*n)++;
}

static int spawn(SuHostWorld *w, const Ids *ids, uint32_t kind, int64_t x, int64_t y, int64_t z, int64_t radius,
                 int64_t vx, uint32_t gate_index, uint64_t *handle) {
    SuHostValue v[32];
    SuHostError err;
    uint32_t n = 0, st;
    /* Gates face +x: the rotation of +z onto +x (90 degrees about y), q = (cos 45, 0, sin 45, 0). */
    const int64_t half_sqrt2 = INT64_C(3037000500); /* round(2^32 * sqrt(2) / 2) */
    v[n].field_id = ids->kind;
    v[n++].value = kind;
    v[n].field_id = ids->gate_index;
    v[n++].value = gate_index;
    put_q(v, &n, ids->pos[0], x);
    put_q(v, &n, ids->pos[1], y);
    put_q(v, &n, ids->pos[2], z);
    put_q(v, &n, ids->radius, radius);
    put_q(v, &n, ids->rot[0], kind >= 5u ? half_sqrt2 : Q32(1));
    put_q(v, &n, ids->rot[2], kind >= 5u ? half_sqrt2 : 0);
    put_q(v, &n, ids->vel[0], vx);
    if (g_respond && kind == 1u) {
        put_q(v, &n, ids->prox_margin, Q32(3));
    }
    st = su_host_spawn(w, v, n, 0u, handle, &err);
    if (st != SU_OK) {
        fprintf(stderr, "spawn: %s (%s)\n", su_host_status_name(st), err.message);
        return 0;
    }
    return 1;
}

/* Scripted inputs of tick t (Q16.16 m/s^2 per axis): ship 0 and 1 accelerate along +x for a second, ship 2
 * weaves in y, ship 3 accelerates along -x towards ship 1's lane. */
static int g_coast = 0; /* --coast: every input is zero (the ships keep their initial velocities) */

/* Every committed column of every row (slots 0..rows-1) as JSON: the world.create fixture format when fixture is
 * nonzero (tests/space3d/test_space3d_serve.py builds the same world through `sutekh serve`), else
 * {"tick", "columns"}. */
static int dump_world(const char *path, const SuHostWorld *w, uint32_t rows, int fixture) {
    FILE *f = fopen(path, "wb");
    uint32_t i, r, n = su_host_field_count(w);
    uint32_t col[64];
    if (f == NULL || rows > 64u) {
        if (f != NULL) {
            fclose(f);
        }
        return 0;
    }
    if (fixture) {
        fprintf(f, "{\"fixture_format\":\"0.1\",\"schema_id\":\"arena\",\"schema_version\":1,\"tick\":0,\"entity_ids\":[");
        for (r = 0; r < rows; r++) {
            fprintf(f, "%s%u", r ? "," : "", (unsigned)(r + 1u));
        }
        fprintf(f, "],");
    } else {
        fprintf(f, "{\"tick\":%llu,", (unsigned long long)su_host_tick(w));
    }
    fprintf(f, "\"columns\":{");
    for (i = 0; i < n; i++) {
        SuHostFieldInfo fi;
        su_host_field_info(w, i, &fi);
        su_host_read(w, fi.id, 0, rows, col);
        fprintf(f, "%s\"%s\":[", i ? "," : "", fi.name);
        for (r = 0; r < rows; r++) {
            fprintf(f, "%s%u", r ? "," : "", (unsigned)col[r]);
        }
        fprintf(f, "]");
    }
    fprintf(f, "}}\n");
    return fclose(f) == 0;
}

static void inputs_for(uint64_t t, uint32_t ship, int32_t a[3]) {
    a[0] = a[1] = a[2] = 0;
    if (g_coast) {
        return;
    }
    if (ship <= 1u && t < 120u) {
        a[0] = 6 * 65536;
    } else if (ship == 2u) {
        a[0] = t < 60u ? 8 * 65536 : 0;
        a[1] = ((t / 90u) % 2u == 0u) ? 3 * 65536 : -3 * 65536;
    } else if (ship == 3u && t < 90u) {
        a[0] = -9 * 65536;
    }
}

static int push_inputs(SuHostWorld *w, const Ids *ids, const uint64_t *ship, uint64_t t) {
    SuHostInput in[SHIPS * 3u];
    SuHostError err;
    uint32_t s, n = 0, st;
    for (s = 0; s < SHIPS; s++) {
        int32_t a[3];
        inputs_for(t, s, a);
        in[n].entity = ship[s];
        in[n].field_id = ids->in_ax;
        in[n++].value = (uint32_t)a[0];
        in[n].entity = ship[s];
        in[n].field_id = ids->in_ay;
        in[n++].value = (uint32_t)a[1];
        in[n].entity = ship[s];
        in[n].field_id = ids->in_az;
        in[n++].value = (uint32_t)a[2];
    }
    st = su_host_input_push(w, t, in, n, &err);
    if (st != SU_OK) {
        fprintf(stderr, "input tick %llu: %s (%s)\n", (unsigned long long)t, su_host_status_name(st), err.message);
        return 0;
    }
    return 1;
}

typedef struct Hist {
    uint8_t *snap;
    size_t len;
    uint64_t tick;
    uint64_t fast;      /* fast digest of the state at tick */
    uint32_t nev;       /* events of tick */
    SuHostEvent ev[16];
} Hist;

static int record(const SuHostWorld *w, Hist *h) {
    size_t need = 0;
    uint32_t total = 0;
    if (su_host_snapshot_size(w, &need) != SU_OK) {
        return 0;
    }
    if (need > h->len) {
        uint8_t *p = realloc(h->snap, need);
        if (p == NULL) {
            return 0;
        }
        h->snap = p;
    }
    h->len = need;
    h->tick = su_host_tick(w);
    if (su_host_snapshot_write(w, h->snap, need, &need) != SU_OK || su_host_digest_fast(w, &h->fast) != SU_OK) {
        return 0;
    }
    return su_host_events_for_tick(w, h->tick, h->ev, 16u, &h->nev, &total, NULL) == SU_OK && total <= 16u;
}

static int64_t read_q(const SuHostWorld *w, uint32_t lo_id, uint64_t slot) {
    uint32_t lo = 0, hi = 0;
    su_host_read(w, lo_id, slot, 1, &lo);
    su_host_read(w, lo_id + 1u, slot, 1, &hi);
    return (int64_t)(((uint64_t)hi << 32) | lo);
}

/* Deepest penetration (m) in committed state: ships 0..3 (radius 1) against each other and the sphere (slot 7,
 * radius 3). Doubles here are the host's measurement only; they never feed back into the simulation. */
static double penetration(const SuHostWorld *w, const Ids *ids) {
    double p[8][3], worst = 0.0;
    static const uint64_t slots[5] = {0, 1, 2, 3, 7};
    static const double radius[5] = {1.0, 1.0, 1.0, 1.0, 3.0};
    uint32_t a, b, k;
    for (a = 0; a < 5u; a++) {
        for (k = 0; k < 3u; k++) {
            p[a][k] = (double)read_q(w, ids->pos[k], slots[a]) / 4294967296.0;
        }
    }
    for (a = 0; a < 4u; a++) {
        for (b = a + 1u; b < 5u; b++) {
            double dx = p[a][0] - p[b][0], dy = p[a][1] - p[b][1], dz = p[a][2] - p[b][2];
            double depth = radius[a] + radius[b] - sqrt(dx * dx + dy * dy + dz * dz);
            worst = depth > worst ? depth : worst;
        }
    }
    return worst;
}

int main(int argc, char **argv) {
    const char *root = ".";
    const char *expect = NULL;
    const char *dump_fixture = NULL, *dump_final = NULL;
    uint64_t ticks = 600, t, ship[SHIPS], other;
    int rollback = 0, native = 0, quiet = 0, i, nfiles = 5;
    static const char *const files[7] = {"arena_score.capsule.json", "arena_score.lir.json", "arena_move.capsule.json",
                                         "arena_move.lir.json", "arena_query.capsule.json",
                                         "arena_respond.capsule.json", "arena_respond.lir.json"};
    uint8_t *bytes[7];
    size_t lens[7];
    SuHostCapsule caps[4];
    uint32_t near_events = 0, worst_tick = 0;
    double worst = 0.0;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError err;
    Ids ids;
    Hist hist[HIST];
    uint32_t st, s, k, gate_events = 0, bump_events = 0, drills = 0, drill_bad = 0;
    uint8_t digest[32];
    char hex[65];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            ticks = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--expect-digest") == 0 && i + 1 < argc) {
            expect = argv[++i];
        } else if (strcmp(argv[i], "--rollback-check") == 0) {
            rollback = 1;
        } else if (strcmp(argv[i], "--native") == 0) {
            native = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--respond") == 0) {
            g_respond = 1;
        } else if (strcmp(argv[i], "--coast") == 0) {
            g_coast = 1;
        } else if (strcmp(argv[i], "--dump-fixture") == 0 && i + 1 < argc) {
            dump_fixture = argv[++i];
        } else if (strcmp(argv[i], "--dump-final") == 0 && i + 1 < argc) {
            dump_final = argv[++i];
        } else {
            fprintf(stderr, "usage: arena_host [--root <repo>] [--ticks N] [--rollback-check] [--native] [--respond] [--quiet] "
                            "[--expect-digest <hex>]\n");
            return 2;
        }
    }
    nfiles = g_respond ? 7 : 5;
    for (i = 0; i < nfiles; i++) {
        bytes[i] = slurp(root, g_respond && i == 4 ? "arena_query_prox.capsule.json" : files[i], &lens[i]);
        if (bytes[i] == NULL) {
            return 2;
        }
    }
    memset(caps, 0, sizeof caps);
    caps[0].capsule_json = bytes[0];
    caps[0].capsule_len = lens[0];
    caps[0].lir_json = bytes[1];
    caps[0].lir_len = lens[1];
    caps[1].capsule_json = bytes[2];
    caps[1].capsule_len = lens[2];
    caps[1].lir_json = bytes[3];
    caps[1].lir_len = lens[3];
    caps[2].capsule_json = bytes[4]; /* host operator record: no IR */
    caps[2].capsule_len = lens[4];
    if (g_respond) {
        caps[3].capsule_json = bytes[5];
        caps[3].capsule_len = lens[5];
        caps[3].lir_json = bytes[6];
        caps[3].lir_len = lens[6];
    }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = g_respond ? 4u : 3u;
    d.capsules = caps;
    d.max_entities = 16;
    d.tick_rate_hz = 120;
    d.event_log_max = 4096;
    st = su_host_world_create(&d, &w, &err);
    if (st != SU_OK) {
        fprintf(stderr, "world: %s (%s)\n", su_host_status_name(st), err.message);
        return 1;
    }
    if (!resolve(w, &ids)) {
        return 1;
    }
    for (k = 0; k < su_host_system_count(w); k++) {
        const char *sid = NULL;
        uint32_t tier = 0;
        su_host_system_info(w, k, &sid, NULL);
        if (native && k == 1u) {
            st = su_host_system_set_tier(w, k, SU_HOST_TIER_NATIVE, &err);
            if (st != SU_OK) {
                fprintf(stderr, "native %s: %s (%s)\n", sid, su_host_status_name(st), err.message);
                return 1;
            }
        }
        su_host_system_tier(w, k, &tier);
        if (!quiet) {
            printf("system %u %s tier %s\n", (unsigned)k, sid,
                   tier == SU_HOST_TIER_OPERATOR  ? "operator"
                   : tier == SU_HOST_TIER_NATIVE  ? "native"
                   : tier == SU_HOST_TIER_PLANNED ? "planned"
                                                  : "reference");
        }
    }
    /* Ships 0..3 (slots 0..3), gates at x = 40, 90, 140 m, a sphere in ship 2's path. */
    if (!spawn(w, &ids, 1u, Q32(0), Q32(0), 0, Q32(1), Q32(10), 0u, &ship[0]) ||
        !spawn(w, &ids, 1u, Q32(0), Q32(8), 0, Q32(1), Q32(10), 0u, &ship[1]) ||
        !spawn(w, &ids, 1u, Q32(0), Q32(-8), 0, Q32(1), Q32(12), 0u, &ship[2]) ||
        !spawn(w, &ids, 1u, Q32(130), Q32(8), 0, Q32(1), Q32(-6), 0u, &ship[3])) {
        return 1;
    }
    for (k = 0; k < GATES; k++) {
        if (!spawn(w, &ids, 6u, Q32(40 + 50 * (int64_t)k), 0, 0, Q32(20), 0, k, &other)) {
            return 1;
        }
    }
    if (!spawn(w, &ids, 2u, Q32(25), Q32(-6), 0, Q32(3), 0, 0u, &other)) {
        return 1;
    }
    if (dump_fixture != NULL && !dump_world(dump_fixture, w, SHIPS + GATES + 1u, 1)) {
        fprintf(stderr, "cannot write %s\n", dump_fixture);
        return 1;
    }
    memset(hist, 0, sizeof hist);
    for (t = su_host_tick(w); t < ticks; t++) {
        uint64_t done = 0;
        Hist *h = &hist[(t + 1u) % HIST];
        if (!push_inputs(w, &ids, ship, t)) {
            return 1;
        }
        st = su_host_step(w, 1, &done, &err);
        if (st != SU_OK) {
            fprintf(stderr, "step %llu: %s (%s)\n", (unsigned long long)t, su_host_status_name(st), err.message);
            return 1;
        }
        if (!record(w, h)) {
            fprintf(stderr, "record failed at tick %llu\n", (unsigned long long)(t + 1u));
            return 1;
        }
        for (k = 0; k < h->nev; k++) {
            const SuHostEvent *e = &h->ev[k];
            if (e->kind == 1u) {
                gate_events++;
            } else if (e->kind == 2u) {
                bump_events++;
            } else if (e->kind == 3u) {
                near_events++;
            }
            if (!quiet && (e->kind == 1u || e->kind == 2u)) {
                printf("tick %4llu  ship slot %u  %s %u (%s %u)\n", (unsigned long long)e->tick, (unsigned)e->slot,
                       e->kind == 1u ? "passed gate" : "bumped slot", (unsigned)e->a,
                       e->kind == 1u ? "score" : "contacts", (unsigned)e->b);
            }
        }
        {
            double pen = penetration(w, &ids);
            if (pen > worst) {
                worst = pen;
                worst_tick = (uint32_t)(t + 1u);
            }
        }
        /* Rollback drill: back DRILL_BACK ticks, re-push the inputs, re-simulate in one call, compare. */
        if (rollback && (t + 1u) % 30u == 0u && t + 1u >= DRILL_BACK + 1u) {
            const Hist *from = &hist[(t + 1u - DRILL_BACK) % HIST];
            uint64_t tt;
            int bad = 0;
            st = su_host_snapshot_restore_inplace(w, from->snap, from->len, &err);
            for (tt = from->tick; st == SU_OK && tt < t + 1u; tt++) {
                if (!push_inputs(w, &ids, ship, tt)) {
                    return 1;
                }
            }
            if (st == SU_OK) {
                st = su_host_step(w, DRILL_BACK, &done, &err);
            }
            if (st != SU_OK) {
                fprintf(stderr, "drill at %llu: %s (%s)\n", (unsigned long long)(t + 1u), su_host_status_name(st),
                        err.message);
                return 1;
            }
            for (tt = from->tick + 1u; tt <= t + 1u; tt++) {
                const Hist *r = &hist[tt % HIST];
                SuHostEvent ev[16];
                uint32_t n = 0;
                su_host_events_for_tick(w, tt, ev, 16u, &n, NULL, NULL);
                bad |= n != r->nev || memcmp(ev, r->ev, (size_t)n * sizeof ev[0]) != 0;
            }
            {
                uint64_t f = 0;
                su_host_digest_fast(w, &f);
                bad |= f != h->fast;
            }
            drills++;
            drill_bad += bad ? 1u : 0u;
        }
    }
    if (dump_final != NULL && !dump_world(dump_final, w, SHIPS + GATES + 1u, 0)) {
        fprintf(stderr, "cannot write %s\n", dump_final);
        return 1;
    }
    su_host_digest(w, digest);
    su_host_hex(digest, hex);
    {
        uint32_t sc[SHIPS], bp[SHIPS];
        su_host_read(w, ids.score, 0, SHIPS, sc);
        su_host_read(w, ids.bumps, 0, SHIPS, bp);
        printf("arena: %llu ticks, %u gate events, %u bump events, scores %u %u %u %u, bump ticks %u %u %u %u\n",
               (unsigned long long)su_host_tick(w), (unsigned)gate_events, (unsigned)bump_events, (unsigned)sc[0],
               (unsigned)sc[1], (unsigned)sc[2], (unsigned)sc[3], (unsigned)bp[0], (unsigned)bp[1], (unsigned)bp[2],
               (unsigned)bp[3]);
    }
    printf("arena: %s, deepest committed penetration %.9f m (tick %u), %u near-miss events\n",
           g_respond ? "same-tick response (arena.respond after space3d.query_prox)" : "next-tick response",
           worst, (unsigned)worst_tick, (unsigned)near_events);
    if (rollback) {
        printf("arena: %u rollback drills (restore in place %u ticks back, re-simulate in one call), %u differed\n",
               (unsigned)drills, (unsigned)DRILL_BACK, (unsigned)drill_bad);
    }
    printf("arena: final digest %s\n", hex);
    for (s = 0; s < HIST; s++) {
        free(hist[s].snap);
    }
    su_host_world_destroy(w);
    for (i = 0; i < nfiles; i++) {
        free(bytes[i]);
    }
    if (drill_bad != 0u) {
        return 1;
    }
    if (expect != NULL && strcmp(expect, hex) != 0) {
        fprintf(stderr, "digest differs from --expect-digest %s\n", expect);
        return 1;
    }
    if (g_respond && (worst > 1e-6 || near_events == 0u)) {
        fprintf(stderr, "same-tick response: penetration %.9f m (limit 1e-6), %u near-miss events\n", worst,
                (unsigned)near_events);
        return 1;
    }
    return (g_coast || gate_events > 0u) && bump_events > 0u ? 0 : 1;
}
