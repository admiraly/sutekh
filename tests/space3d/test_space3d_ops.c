/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * space3d.query host operator (profiles/space3d/native/space3d_ops.c) on a real world:
 *   - descriptor: validation, typed-lane declaration module, constants hash literal, host record loading and
 *     refusal of altered records (host_decl.c compiled in);
 *   - hand-built scenes with known results (contacts, ordering, symmetry, gates, dead rows, zero rows);
 *   - seeded random scenes: broadphase output equals the brute-force (no broadphase) output on every output lane
 *     when no truncation flag is set, and two runs are bit-identical;
 *   - candidate truncation and the row limit.
 * Run from the repository root (reads profiles/space3d/space3d_query.capsule.json). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "ir/su_num.h"
#include "ir/su_sha256.h"
#include "runtime/rts/host_decl.h"
#include "space3d_ops.h"
#include "sutekh/status.h"
#include "world/su_world.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define ONE SU_Q32_ONE
#define Q(x) ((int64_t)(x) * ONE)
#define NONE 0xFFFFFFFFu

static const SuHostOpDesc *D = &g_space3d_query_desc;

/* ---------- scene: column arrays for every declared lane ---------- */

#define MAXR 4096u
static uint32_t g_col[80][MAXR]; /* indexed by field id */

typedef struct Scene {
    uint32_t rows;
    uint8_t dead[MAXR];
} Scene;

static void scene_init(Scene *s, uint32_t rows) {
    memset(g_col, 0, sizeof g_col);
    memset(s, 0, sizeof *s);
    s->rows = rows;
}

static void set32(uint32_t id, uint32_t r, uint32_t v) {
    g_col[id][r] = v;
}

static void setq(uint32_t id, uint32_t r, int64_t v) {
    g_col[id][r] = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    g_col[id + 1u][r] = (uint32_t)((uint64_t)v >> 32);
}

static void set3(uint32_t id, uint32_t r, int64_t x, int64_t y, int64_t z) {
    setq(id, r, x);
    setq(id + 2u, r, y);
    setq(id + 4u, r, z);
}

static void set_rot(uint32_t r, SuQuat q) {
    setq(SPACE3D_F_ROT_W, r, q.w);
    setq(SPACE3D_F_ROT_X, r, q.x);
    setq(SPACE3D_F_ROT_Y, r, q.y);
    setq(SPACE3D_F_ROT_Z, r, q.z);
}

static const SuQuat QID = {ONE, 0, 0, 0};

static void mover(uint32_t r, SuV3 prev, SuV3 pos, int64_t radius, uint32_t next_gate) {
    set32(SPACE3D_F_KIND, r, SPACE3D_KIND_MOVER);
    set3(SPACE3D_F_PREV_X, r, prev.x, prev.y, prev.z);
    set3(SPACE3D_F_POS_X, r, pos.x, pos.y, pos.z);
    setq(SPACE3D_F_RADIUS, r, radius);
    set32(SPACE3D_F_NEXT_GATE, r, next_gate);
    set_rot(r, QID);
}

static void sphere(uint32_t r, SuV3 c, int64_t radius) {
    set32(SPACE3D_F_KIND, r, SPACE3D_KIND_SPHERE);
    set3(SPACE3D_F_POS_X, r, c.x, c.y, c.z);
    setq(SPACE3D_F_RADIUS, r, radius);
    set_rot(r, QID);
}

static void box(uint32_t r, SuV3 c, SuQuat q, SuV3 half) {
    set32(SPACE3D_F_KIND, r, SPACE3D_KIND_BOX);
    set3(SPACE3D_F_POS_X, r, c.x, c.y, c.z);
    set3(SPACE3D_F_HALF_X, r, half.x, half.y, half.z);
    set_rot(r, q);
}

static void capsule(uint32_t r, SuV3 c, SuQuat q, int64_t half_len, int64_t radius) {
    set32(SPACE3D_F_KIND, r, SPACE3D_KIND_CAPSULE);
    set3(SPACE3D_F_POS_X, r, c.x, c.y, c.z);
    setq(SPACE3D_F_HALF_Z, r, half_len);
    setq(SPACE3D_F_RADIUS, r, radius);
    set_rot(r, q);
}

/* shape: 0 rectangle, 1 disc, 2 ellipse (semi-axes hx, hy). */
static void gate(uint32_t r, SuV3 c, SuQuat q, uint32_t index, int shape, int64_t hx, int64_t hy, int64_t radius) {
    set32(SPACE3D_F_KIND, r, shape == 1 ? SPACE3D_KIND_GATE_DISC
                             : shape == 2 ? SPACE3D_KIND_GATE_ELLIPSE : SPACE3D_KIND_GATE_RECT);
    set3(SPACE3D_F_POS_X, r, c.x, c.y, c.z);
    set_rot(r, q);
    set32(SPACE3D_F_GATE_INDEX, r, index);
    setq(SPACE3D_F_HALF_X, r, hx);
    setq(SPACE3D_F_HALF_Y, r, hy);
    setq(SPACE3D_F_RADIUS, r, radius);
}

static SuV3 V(int64_t x, int64_t y, int64_t z) {
    SuV3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

/* Builds the world (descriptor lanes), runs the operator once, publishes. Returns the status; *wout keeps the
 * world for reading (NULL on creation failure). */
static uint32_t run_scene(const Scene *s, SuWorld **wout) {
    SuWorldFieldDesc fd[80];
    SuWorldDesc wd;
    uint32_t ids[MAXR], wid[80];
    SuWorld *w = NULL;
    SuInvocation wi, inv;
    SuOutputs wo, outs;
    SuColumnBinding bin[80], bout[80];
    uint32_t i, r, st, nin = 0, nout = 0;
    for (i = 0; i < D->field_count; i++) {
        fd[i].id = D->fields[i].id;
        fd[i].type = SU_TYPE_U32;
        fd[i].access = D->fields[i].access;
        fd[i].name = D->fields[i].name;
    }
    for (r = 0; r < s->rows; r++) {
        ids[r] = r + 1u;
    }
    memset(&wd, 0, sizeof wd);
    wd.schema_id = SPACE3D_SCHEMA_ID;
    wd.schema_version = 1;
    wd.layout_generation = 1;
    wd.row_count = s->rows;
    wd.entity_ids = ids;
    wd.fields = fd;
    wd.field_count = D->field_count;
    *wout = NULL;
    memset(&wi, 0, sizeof wi);
    memset(&wo, 0, sizeof wo);
    st = su_world_create(&wd, &w);
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < D->field_count; i++) {
        su_world_init_committed(w, D->fields[i].id, g_col[D->fields[i].id], s->rows);
    }
    for (r = 0; r < s->rows; r++) {
        if (s->dead[r]) {
            SuEntity e = SU_ENTITY_MAKE(r, 1u);
            CHECK(su_world_despawn(w, e) == SU_OK);
        }
    }
    *wout = w;
    st = su_world_begin_tick(w);
    if (st == SU_OK) st = su_world_build_invocation(w, &wi);
    if (st == SU_OK) st = su_world_build_outputs(w, &wo);
    if (st != SU_OK) {
        su_world_abort(w);
        return st;
    }
    for (i = 0; i < D->field_count; i++) {
        uint32_t j;
        if (D->fields[i].access & SU_ACCESS_READ) {
            for (j = 0; j < wi.binding_count; j++) {
                if (wi.bindings[j].field_id == D->fields[i].id) {
                    bin[nin++] = wi.bindings[j];
                }
            }
        }
        if (D->fields[i].access & SU_ACCESS_WRITE) {
            for (j = 0; j < wo.binding_count; j++) {
                if (wo.bindings[j].field_id == D->fields[i].id) {
                    wid[nout] = D->fields[i].id;
                    bout[nout++] = wo.bindings[j];
                }
            }
        }
    }
    inv = wi;
    inv.binding_count = nin;
    inv.bindings = bin;
    outs = wo;
    outs.binding_count = nout;
    outs.bindings = bout;
    st = D->run(w, &inv, &outs, D->host_ctx);
    if (st == SU_OK) st = su_world_commit_staging(w, wid, nout);
    if (st == SU_OK) st = su_world_publish(w);
    else su_world_abort(w);
    return st;
}

static uint32_t rd32(const SuWorld *w, uint32_t id, uint32_t r) {
    const uint32_t *c;
    uint64_t n;
    return su_world_read_committed(w, id, &c, &n) == SU_OK ? c[r] : 0xDEADBEEFu;
}

static int64_t rdq(const SuWorld *w, uint32_t id, uint32_t r) {
    return su_i64_from_bits(((uint64_t)rd32(w, id + 1u, r) << 32) | rd32(w, id, r));
}

/* Contact k of row r. */
static uint32_t cslot(const SuWorld *w, uint32_t r, uint32_t k) { return rd32(w, 42u + 10u * k, r); }
static int64_t ctoi(const SuWorld *w, uint32_t r, uint32_t k) { return rdq(w, 44u + 10u * k, r); }
static int64_t cn(const SuWorld *w, uint32_t r, uint32_t k, uint32_t axis) { return rdq(w, 46u + 10u * k + 2u * axis, r); }

/* ---------- descriptor and declaration ---------- */

static void test_descriptor(void) {
    SuLirModule m;
    SuSha256 c;
    uint8_t h[32];
    char hex[65], msg[256];
    uint32_t i, sv = 0, gen = 0;
    SuLirModule *decl = NULL;
    CHECK(su_host_desc_validate(D) == SU_OK);
    su_sha256_init(&c);
    su_sha256_update(&c, space3d_constants_text(), strlen(space3d_constants_text()));
    su_sha256_final(&c, h);
    su_sha256_hex(h, hex);
    CHECK(memcmp(h, D->constants_hash, 32) == 0);
    printf("space3d constants_hash %s\n", hex);
    CHECK(su_host_decl_build(D, &m) == SU_OK);
    CHECK(strcmp(m.numeric_profile, SU_NUMERIC_PROFILE_FIXED_Q32_V1) == 0);
    CHECK(m.field_count == D->field_count);
    for (i = 0; i < m.field_count; i++) {
        CHECK(m.fields[i].type == SU_TYPE_U32);
        CHECK(m.fields[i].value_type == D->fields[i].type);
        CHECK(m.fields[i].lane == (strstr(m.fields[i].name, ".hi") != NULL ? 1u : 0u));
    }
    printf("space3d semantic hash %s\n", m.semantic_hash_hex);
    su_lir_module_free(&m);
    /* The committed host record loads; altered copies are refused. */
    CHECK(su_host_load("profiles/space3d/space3d_query.capsule.json", NULL, D, &decl, &sv, &gen, msg, sizeof msg) ==
          SU_OK);
    if (decl == NULL) {
        fprintf(stderr, "host record: %s\n", msg);
    } else {
        CHECK(sv == 1u && gen == 1u && decl->field_count == D->field_count);
        su_host_decl_free(decl);
        decl = NULL;
    }
    {
        static char buf[65536];
        FILE *f = fopen("profiles/space3d/space3d_query.capsule.json", "rb");
        size_t n = f != NULL ? fread(buf, 1, sizeof buf - 1u, f) : 0u;
        const char *tmp = "space3d_record_tmp.json";
        static const char *const subs[][2] = {
            {"\"type\": \"q32\"", "\"type\": \"u32\""},          /* a typed lane declared as u32 */
            {"space3d_ops_v1", "rts_ops_v1"},                    /* wrong operator family */
            {"fixed_q32_v1", "u32_mod_v1"},                       /* wrong numeric profile */
            {"\"name\": \"pos_x.hi\"", "\"name\": \"pos_x_hi\""}, /* lane renamed */
        };
        static const uint32_t want[] = {SU_E_SCHEMA_MISMATCH, SU_E_SCHEMA_MISMATCH, SU_E_TYPE, SU_E_SCHEMA_MISMATCH};
        size_t k;
        if (f != NULL) {
            fclose(f);
        }
        buf[n] = '\0';
        CHECK(n > 0);
        for (k = 0; k < sizeof subs / sizeof subs[0]; k++) {
            char *p = strstr(buf, subs[k][0]);
            FILE *o;
            uint32_t st;
            CHECK(p != NULL);
            if (p == NULL) {
                continue;
            }
            o = fopen(tmp, "wb");
            fwrite(buf, 1, (size_t)(p - buf), o);
            fputs(subs[k][1], o);
            fputs(p + strlen(subs[k][0]), o);
            fclose(o);
            st = su_host_load(tmp, NULL, D, &decl, &sv, &gen, msg, sizeof msg);
            CHECK(st == want[k] && decl == NULL);
            if (st != want[k]) {
                fprintf(stderr, "altered record %zu: %s (%s)\n", k, su_status_name(st), msg);
            }
            if (decl != NULL) {
                su_host_decl_free(decl);
                decl = NULL;
            }
        }
        remove(tmp);
    }
    {
        /* Malformed typed descriptors are refused. */
        static const SuHostField bad_pair[] = {{10u, SU_TYPE_Q32, SU_ACCESS_READ, "a.lo"},
                                               {12u, SU_TYPE_Q32, SU_ACCESS_READ, "a.hi"}};
        static const SuHostField bad_acc[] = {{10u, SU_TYPE_Q32, SU_ACCESS_READ, "a.lo"},
                                              {11u, SU_TYPE_Q32, SU_ACCESS_WRITE, "a.hi"}};
        static const SuHostField bad_type[] = {{10u, SU_TYPE_V3, SU_ACCESS_READ, "a"}};
        SuHostOpDesc d = *D;
        d.field_count = 2;
        d.fields = bad_pair;
        CHECK(su_host_desc_validate(&d) == SU_E_INVALID_ARGUMENT);
        d.fields = bad_acc;
        CHECK(su_host_desc_validate(&d) == SU_E_INVALID_ARGUMENT);
        d.field_count = 1;
        d.fields = bad_type;
        CHECK(su_host_desc_validate(&d) == SU_E_INVALID_ARGUMENT);
    }
}

/* ---------- hand-built scenes ---------- */

static void test_known(void) {
    Scene s;
    SuWorld *w = NULL;
    int64_t t08 = INT64_C(3435973837);
    uint32_t r;
    /* Row 0: mover hits a static sphere (row 1) head on at t = 0.8.
     * Row 2: mover moving +x at y = 20 hits a box (row 3) face at t = 0.8.
     * Rows 4, 5: two movers approaching each other along z; contact at t = 0.5 for both, normals opposite.
     * Row 6: mover crossing gate index 7 forward; rows 7 and 8 are gates with index 7 (row 7 is the lowest slot,
     *        a rectangle; row 8 a disc elsewhere) and row 9 a gate with index 3.
     * Row 10: static capsule; row 11: mover hitting the capsule side at t = 0.8.
     * Row 12: dead mover on a collision course with row 1 (ignored). Row 13: kind none. */
    scene_init(&s, 14);
    mover(0, V(Q(-10), 0, 0), V(0, 0, 0), ONE, 99);
    sphere(1, V(0, 0, 0), ONE);
    mover(2, V(Q(-10), Q(20), 0), V(0, Q(20), 0), ONE, 99);
    box(3, V(0, Q(20), 0), QID, V(ONE, ONE, ONE));
    mover(4, V(Q(100), 0, Q(-5)), V(Q(100), 0, Q(5)), ONE, 99);
    mover(5, V(Q(100), 0, Q(5)), V(Q(100), 0, Q(-5)), ONE, 99);
    mover(6, V(Q(300), Q(1), Q(-1)), V(Q(300), Q(1), Q(1)), ONE, 7);
    gate(7, V(Q(300), 0, 0), QID, 7, 0, Q(5), Q(5), 0);
    gate(8, V(Q(300), Q(1000), 0), QID, 7, 1, 0, 0, Q(5));
    gate(9, V(Q(300), 0, 0), QID, 3, 0, Q(50), Q(50), 0);
    capsule(10, V(Q(500), 0, 0), QID, Q(5), ONE); /* along z, from z = -5 to 5 */
    mover(11, V(Q(490), 0, 0), V(Q(500), 0, 0), ONE, 99);
    mover(12, V(Q(-10), 0, Q(1) / 2), V(0, 0, Q(1) / 2), ONE, 99);
    s.dead[12] = 1;
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w == NULL) {
        return;
    }
    CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 1u && cslot(w, 0, 0) == 1u && ctoi(w, 0, 0) == t08);
    CHECK(cn(w, 0, 0, 0) == -ONE && cn(w, 0, 0, 1) == 0 && cn(w, 0, 0, 2) == 0);
    CHECK(rd32(w, 43u, 0) == 1u); /* generation of row 1 */
    CHECK(cslot(w, 0, 1) == NONE && ctoi(w, 0, 1) == 0);
    CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 2) == 1u && cslot(w, 2, 0) == 3u && ctoi(w, 2, 0) == t08 && cn(w, 2, 0, 0) == -ONE);
    CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 4) == 1u && cslot(w, 4, 0) == 5u && ctoi(w, 4, 0) == su_q32_div(Q(8), Q(20)));
    CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 5) == 1u && cslot(w, 5, 0) == 4u && ctoi(w, 5, 0) == ctoi(w, 4, 0));
    CHECK(cn(w, 4, 0, 2) == -ONE && cn(w, 5, 0, 2) == ONE);
    CHECK(rd32(w, SPACE3D_F_GATE_DIR, 6) == SPACE3D_GATE_FORWARD && rd32(w, SPACE3D_F_GATE_SLOT, 6) == 7u);
    CHECK(rdq(w, SPACE3D_F_GATE_TOI, 6) == ONE / 2);
    CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 11) == 1u && cslot(w, 11, 0) == 10u && ctoi(w, 11, 0) == t08);
    for (r = 0; r < 14; r++) {
        if (r == 6 || r == 12) {
            continue;
        }
        CHECK(rd32(w, SPACE3D_F_GATE_DIR, r) == SPACE3D_GATE_NONE && rd32(w, SPACE3D_F_GATE_SLOT, r) == NONE);
    }
    /* Dead and non-mover rows store zeros (and NONE slots). */
    for (r = 7; r < 14; r++) {
        if (r == 11 || r == 12) {
            continue;
        }
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, r) == 0u && rd32(w, SPACE3D_F_HIT_FLAGS, r) == 0u && cslot(w, r, 0) == NONE);
    }
    CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 1) == 0u && rd32(w, SPACE3D_F_HIT_COUNT, 3) == 0u);
    /* A dead row is canonically zero after publish (the world zeroes dead rows), whatever the operator stored. */
    for (r = 0; r < D->field_count; r++) {
        CHECK(rd32(w, D->fields[r].id, 12) == 0u);
    }
    CHECK(space3d_query_ctx()->last.movers == 6u && space3d_query_ctx()->last.gates == 3u);
    su_world_destroy(w);

    /* Ordering and the contact cap: a mover sweeping through three spheres at t 0.2, 0.5, 0.5 (slots 3 and 1). */
    scene_init(&s, 4);
    mover(0, V(Q(-10), 0, 0), V(Q(10), 0, 0), ONE, 0);
    sphere(1, V(Q(2), Q(1), 0), ONE);
    sphere(2, V(Q(-4), 0, 0), ONE);
    sphere(3, V(Q(2), Q(-1), 0), ONE);
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w != NULL) {
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 2u);
        CHECK(rd32(w, SPACE3D_F_HIT_FLAGS, 0) == SPACE3D_FLAG_CONTACT_TRUNCATED);
        CHECK(cslot(w, 0, 0) == 2u && cslot(w, 0, 1) == 1u); /* slot 1 before slot 3 at equal toi */
        CHECK(ctoi(w, 0, 1) > ctoi(w, 0, 0));
        su_world_destroy(w);
    }
    /* Gate backward crossing, disc gate, a missed gate (outside the rectangle), and an elliptical gate (semi-axes
     * 20 x 5): (12, 3) is inside (0.36 + 0.36), (12, 4.5) outside (0.36 + 0.81). */
    scene_init(&s, 8);
    gate(5, V(0, 0, Q(100)), QID, 9, 2, Q(20), Q(5), 0);
    mover(6, V(Q(12), Q(3), Q(99)), V(Q(12), Q(3), Q(101)), ONE, 9);
    mover(7, V(Q(12), Q(9) / 2, Q(99)), V(Q(12), Q(9) / 2, Q(101)), ONE, 9);
    mover(0, V(0, 0, ONE), V(0, 0, -ONE), ONE, 1);
    gate(1, V(0, 0, 0), QID, 1, 1, 0, 0, Q(5));
    mover(2, V(Q(10), 0, -ONE), V(Q(10), 0, ONE), ONE, 2);
    gate(3, V(0, 0, 0), QID, 2, 0, Q(5), Q(5), 0);
    mover(4, V(Q(3), Q(3), -ONE), V(Q(3), Q(3), ONE), ONE, 1);
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w != NULL) {
        CHECK(rd32(w, SPACE3D_F_GATE_DIR, 0) == SPACE3D_GATE_BACKWARD && rd32(w, SPACE3D_F_GATE_SLOT, 0) == 1u);
        CHECK(rd32(w, SPACE3D_F_GATE_DIR, 2) == SPACE3D_GATE_NONE);
        CHECK(rd32(w, SPACE3D_F_GATE_DIR, 4) == SPACE3D_GATE_FORWARD); /* (3,3) inside the radius-5 disc */
        CHECK(rd32(w, SPACE3D_F_GATE_DIR, 6) == SPACE3D_GATE_FORWARD && rd32(w, SPACE3D_F_GATE_SLOT, 6) == 5u);
        CHECK(rdq(w, SPACE3D_F_GATE_TOI, 6) == ONE / 2);
        CHECK(rd32(w, SPACE3D_F_GATE_DIR, 7) == SPACE3D_GATE_NONE);
        su_world_destroy(w);
    }
    /* Zero rows. */
    scene_init(&s, 0);
    CHECK(run_scene(&s, &w) == SU_OK);
    su_world_destroy(w);
}

/* ---------- random scenes: broadphase == brute force ---------- */

static uint64_t g_rng = 1;
static uint64_t rng(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static int64_t rq(int64_t span) {
    return (int64_t)(rng() % (uint64_t)(2 * span * ONE + 1)) - span * ONE;
}

static void random_scene(Scene *s, uint32_t rows, int64_t span) {
    uint32_t r;
    scene_init(s, rows);
    for (r = 0; r < rows; r++) {
        uint32_t k = (uint32_t)(rng() % 10u);
        SuV3 p = V(rq(span), rq(span), rq(span));
        SuQuat q0, q;
        q0.w = rq(1);
        q0.x = rq(1);
        q0.y = rq(1);
        q0.z = rq(1);
        q = su_quat_normalize(q0);
        if (k < 4) {
            SuV3 d = V(rq(12), rq(12), rq(12));
            mover(r, p, su_v3_add(p, d), ONE / 2 + (int64_t)(rng() % (uint64_t)(2 * ONE)), (uint32_t)(rng() % 4u));
        } else if (k < 6) {
            sphere(r, p, ONE / 2 + (int64_t)(rng() % (uint64_t)(4 * ONE)));
        } else if (k < 8) {
            box(r, p, q, V(ONE / 2 + (int64_t)(rng() % (uint64_t)(6 * ONE)), ONE / 2 + (int64_t)(rng() % (uint64_t)(6 * ONE)),
                           ONE / 2 + (int64_t)(rng() % (uint64_t)(6 * ONE))));
        } else if (k < 9) {
            capsule(r, p, q, (int64_t)(rng() % (uint64_t)(8 * ONE)), ONE / 2 + (int64_t)(rng() % (uint64_t)(2 * ONE)));
        } else {
            gate(r, p, q, (uint32_t)(rng() % 4u), (int)(rng() % 3u), Q(8), Q(6), Q(7));
        }
        if (rng() % 50u == 0u) {
            s->dead[r] = 1;
        }
    }
}

/* Copies every write lane of w into dst (rows x write lanes). */
static void snapshot_out(const SuWorld *w, uint32_t rows, uint32_t *dst) {
    uint32_t i, r, n = 0;
    for (i = 0; i < D->field_count; i++) {
        if ((D->fields[i].access & SU_ACCESS_WRITE) == 0u) {
            continue;
        }
        for (r = 0; r < rows; r++) {
            dst[(size_t)n * rows + r] = rd32(w, D->fields[i].id, r);
        }
        n++;
    }
}

static void test_random(void) {
    static uint32_t a[32 * MAXR], b[32 * MAXR], c[32 * MAXR];
    uint32_t seed, compared = 0, flagged = 0, contacts = 0, gates = 0;
    for (seed = 1; seed <= 40; seed++) {
        Scene s;
        SuWorld *w1 = NULL, *w2 = NULL, *w3 = NULL;
        uint32_t rows = 64u + (seed * 97u) % 900u, r, k;
        int64_t span = 20 + (int64_t)(seed % 5u) * 40;
        int flags_any = 0;
        g_rng = seed * UINT64_C(7919);
        random_scene(&s, rows, span);
        space3d_query_ctx()->brute_force = 0;
        CHECK(run_scene(&s, &w1) == SU_OK);
        CHECK(run_scene(&s, &w2) == SU_OK);
        space3d_query_ctx()->brute_force = 1;
        CHECK(run_scene(&s, &w3) == SU_OK);
        space3d_query_ctx()->brute_force = 0;
        if (w1 == NULL || w2 == NULL || w3 == NULL) {
            su_world_destroy(w1);
            su_world_destroy(w2);
            su_world_destroy(w3);
            continue;
        }
        snapshot_out(w1, rows, a);
        snapshot_out(w2, rows, b);
        snapshot_out(w3, rows, c);
        CHECK(memcmp(a, b, sizeof(uint32_t) * 26u * rows) == 0); /* repeat is bit-identical */
        for (r = 0; r < rows; r++) {
            if (rd32(w1, SPACE3D_F_HIT_FLAGS, r) & (SPACE3D_FLAG_CAND_TRUNCATED | SPACE3D_FLAG_SWEEP_BUDGET)) {
                flags_any = 1;
            }
            contacts += rd32(w1, SPACE3D_F_HIT_COUNT, r);
            gates += rd32(w1, SPACE3D_F_GATE_DIR, r) != 0u;
        }
        if (flags_any) {
            flagged++;
        } else {
            compared++;
            CHECK(memcmp(a, c, sizeof(uint32_t) * 26u * rows) == 0); /* broadphase loses nothing */
            if (memcmp(a, c, sizeof(uint32_t) * 26u * rows) != 0) {
                for (k = 0; k < 26u * rows; k++) {
                    if (a[k] != c[k]) {
                        fprintf(stderr, "seed %u: lane %u row %u: sweep %u brute %u\n", seed, k / rows, k % rows, a[k],
                                c[k]);
                        break;
                    }
                }
            }
        }
        su_world_destroy(w1);
        su_world_destroy(w2);
        su_world_destroy(w3);
    }
    printf("space3d random: %u scenes compared bit-exact with brute force, %u with truncation (repeat-only), "
           "%u contacts, %u gate crossings\n", compared, flagged, contacts, gates);
    CHECK(compared >= 30u);
    CHECK(contacts > 100u && gates > 0u);
}

/* ---------- truncation and limits ---------- */

static void test_limits(void) {
    Scene s;
    SuWorld *w = NULL;
    uint32_t r;
    /* One mover inside a cluster of 40 overlapping spheres: 32 candidates kept, flag set, deterministic. */
    scene_init(&s, 41);
    mover(0, V(0, 0, 0), V(ONE, 0, 0), ONE, 0);
    for (r = 1; r <= 40u; r++) {
        sphere(r, V(Q((int64_t)(r % 5u)) - Q(2), Q((int64_t)(r / 5u % 4u)) - Q(2), 0), ONE);
    }
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w != NULL) {
        CHECK((rd32(w, SPACE3D_F_HIT_FLAGS, 0) & SPACE3D_FLAG_CAND_TRUNCATED) != 0u);
        CHECK(space3d_query_ctx()->last.candidates == SPACE3D_CAND_MAX);
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 2u);
        su_world_destroy(w);
    }
    /* The row limit: a world above SPACE3D_MAX_ROWS is refused before any store. */
    {
        SuInvocation in;
        SuOutputs out;
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.row_count = (uint64_t)SPACE3D_MAX_ROWS + 1u;
        CHECK(D->run(NULL, &in, &out, D->host_ctx) == SU_E_RESOURCE_LIMIT);
    }
}

/* ---------- space3d.query_prox (2026-10-18) ---------- */

static void prox_mover(uint32_t r, SuV3 prev, SuV3 pos, int64_t radius, int64_t margin) {
    mover(r, prev, pos, radius, 0);
    setq(SPACE3D_F_PROX_MARGIN, r, margin);
}

static uint32_t write_lanes(void) {
    uint32_t i, n = 0;
    for (i = 0; i < D->field_count; i++) {
        n += (D->fields[i].access & SU_ACCESS_WRITE) != 0u;
    }
    return n;
}

static void test_prox(void) {
    static uint32_t a[32 * MAXR], b[32 * MAXR], c[32 * MAXR];
    const SuHostOpDesc *base = &g_space3d_query_desc;
    SuSha256 sh;
    uint8_t h[32];
    char msg[256];
    uint32_t i, sv = 0, gen = 0, seed, compared = 0, flagged = 0, found = 0, nw;
    SuLirModule *decl = NULL;
    Scene s;
    SuWorld *w = NULL;

    D = &g_space3d_query_prox_desc;
    /* Descriptor: the base table followed by the proximity lanes; own constants hash; record loads only here. */
    CHECK(su_host_desc_validate(D) == SU_OK);
    CHECK(D->field_count == base->field_count + 6u);
    for (i = 0; i < base->field_count; i++) {
        CHECK(D->fields[i].id == base->fields[i].id && D->fields[i].access == base->fields[i].access &&
              strcmp(D->fields[i].name, base->fields[i].name) == 0);
    }
    su_sha256_init(&sh);
    su_sha256_update(&sh, space3d_prox_constants_text(), strlen(space3d_prox_constants_text()));
    su_sha256_final(&sh, h);
    CHECK(memcmp(h, D->constants_hash, 32) == 0);
    CHECK(memcmp(h, base->constants_hash, 32) != 0);
    CHECK(su_host_load("profiles/space3d/space3d_query_prox.capsule.json", NULL, D, &decl, &sv, &gen, msg,
                       sizeof msg) == SU_OK);
    su_host_decl_free(decl);
    decl = NULL;
    CHECK(su_host_load("profiles/space3d/space3d_query_prox.capsule.json", NULL, base, &decl, &sv, &gen, msg,
                       sizeof msg) != SU_OK && decl == NULL);
    CHECK(su_host_load("profiles/space3d/space3d_query.capsule.json", NULL, D, &decl, &sv, &gen, msg, sizeof msg) !=
              SU_OK &&
          decl == NULL);

    /* Known answers. Mover 0 at rest at the origin, radius 1. Spheres (radius 1) at +5 x (slot 1) and -5 y
     * (slot 2): surface distance 3 each, the tie goes to slot 1. A capsule (slot 3) along z from -5 to -3, radius
     * 1/2: distance 3 - 1/2 - 1 = 3/2. A box (slot 4) centred at +10 z, half extents 1: distance 8. */
    {
        static const int64_t margins[5] = {Q(5), 0, Q(1), Q(10), -Q(4)};
        static const uint32_t want_slot[5] = {3u, NONE, NONE, 3u, NONE};
        static const int64_t want_dist[5] = {Q(3) / 2, 0, 0, Q(3) / 2, 0};
        uint32_t k;
        for (k = 0; k < 5u; k++) {
            scene_init(&s, 5);
            prox_mover(0, V(0, 0, 0), V(0, 0, 0), ONE, margins[k]);
            sphere(1, V(Q(5), 0, 0), ONE);
            sphere(2, V(0, -Q(5), 0), ONE);
            capsule(3, V(0, 0, -Q(4)), QID, ONE, ONE / 2);
            box(4, V(0, 0, Q(10)), QID, V(ONE, ONE, ONE));
            CHECK(run_scene(&s, &w) == SU_OK);
            if (w != NULL) {
                CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 0) == want_slot[k]);
                CHECK(rdq(w, SPACE3D_F_PROX_DIST, 0) == want_dist[k]);
                CHECK(rd32(w, SPACE3D_F_PROX_GEN, 0) == (want_slot[k] == NONE ? 0u : 1u));
                CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 1) == NONE); /* non-mover rows: none */
                CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 0u);
                su_world_destroy(w);
                w = NULL;
            }
        }
        /* Without the capsule: the two spheres tie at distance 3, the lower slot wins. The box alone: 8. */
        scene_init(&s, 5);
        prox_mover(0, V(0, 0, 0), V(0, 0, 0), ONE, Q(5));
        sphere(1, V(Q(5), 0, 0), ONE);
        sphere(2, V(0, -Q(5), 0), ONE);
        box(4, V(0, 0, Q(10)), QID, V(ONE, ONE, ONE));
        CHECK(run_scene(&s, &w) == SU_OK);
        if (w != NULL) {
            CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 0) == 1u && rdq(w, SPACE3D_F_PROX_DIST, 0) == Q(3));
            su_world_destroy(w);
            w = NULL;
        }
        scene_init(&s, 5);
        prox_mover(0, V(0, 0, 0), V(0, 0, 0), ONE, Q(9));
        box(4, V(0, 0, Q(10)), QID, V(ONE, ONE, ONE));
        CHECK(run_scene(&s, &w) == SU_OK);
        if (w != NULL) {
            CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 0) == 4u && rdq(w, SPACE3D_F_PROX_DIST, 0) == Q(8));
            su_world_destroy(w);
            w = NULL;
        }
        /* A touching sphere: distance 0 (and a contact). */
        scene_init(&s, 2);
        prox_mover(0, V(0, 0, 0), V(0, 0, 0), ONE, ONE);
        sphere(1, V(Q(2), 0, 0), ONE);
        CHECK(run_scene(&s, &w) == SU_OK);
        if (w != NULL) {
            CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 0) == 1u && rdq(w, SPACE3D_F_PROX_DIST, 0) == 0);
            su_world_destroy(w);
            w = NULL;
        }
    }

    /* Random scenes: (1) margin 0 everywhere gives the base operator's lanes bit for bit; (2) random margins: the
     * broadphase equals brute force on every lane when nothing is truncated; repeat is bit-identical. */
    nw = write_lanes();
    for (seed = 1; seed <= 30; seed++) {
        uint32_t rows = 64u + (seed * 131u) % 700u, r;
        int64_t span = 20 + (int64_t)(seed % 4u) * 40;
        SuWorld *w1 = NULL, *w2 = NULL, *w3 = NULL;
        int flags_any = 0;
        g_rng = seed * UINT64_C(104729);
        random_scene(&s, rows, span);
        D = base;
        CHECK(run_scene(&s, &w1) == SU_OK);
        if (w1 != NULL) {
            snapshot_out(w1, rows, a);
            su_world_destroy(w1);
            w1 = NULL;
        }
        D = &g_space3d_query_prox_desc;
        CHECK(run_scene(&s, &w1) == SU_OK);
        if (w1 != NULL) {
            snapshot_out(w1, rows, b);
            CHECK(memcmp(a, b, sizeof(uint32_t) * 26u * rows) == 0); /* margin 0: base lanes unchanged */
            for (r = 0; r < rows; r++) {
                CHECK(s.dead[r] || rd32(w1, SPACE3D_F_PROX_SLOT, r) == NONE); /* dead rows are zeroed */
            }
            su_world_destroy(w1);
            w1 = NULL;
        }
        for (r = 0; r < rows; r++) {
            if (g_col[SPACE3D_F_KIND][r] == SPACE3D_KIND_MOVER) {
                setq(SPACE3D_F_PROX_MARGIN, r, (int64_t)(rng() % (uint64_t)(8 * ONE)) - ONE);
            }
        }
        CHECK(run_scene(&s, &w1) == SU_OK);
        CHECK(run_scene(&s, &w2) == SU_OK);
        space3d_query_ctx()->brute_force = 1;
        CHECK(run_scene(&s, &w3) == SU_OK);
        space3d_query_ctx()->brute_force = 0;
        if (w1 != NULL && w2 != NULL && w3 != NULL) {
            snapshot_out(w1, rows, a);
            snapshot_out(w2, rows, b);
            snapshot_out(w3, rows, c);
            CHECK(memcmp(a, b, sizeof(uint32_t) * nw * rows) == 0);
            for (r = 0; r < rows; r++) {
                flags_any |= (rd32(w1, SPACE3D_F_HIT_FLAGS, r) &
                              (SPACE3D_FLAG_CAND_TRUNCATED | SPACE3D_FLAG_SWEEP_BUDGET)) != 0u;
                if (rd32(w1, SPACE3D_F_PROX_SLOT, r) != NONE) {
                    found++;
                    CHECK(rdq(w1, SPACE3D_F_PROX_DIST, r) <= rdq(w1, SPACE3D_F_PROX_MARGIN, r));
                }
            }
            if (flags_any) {
                flagged++;
            } else {
                compared++;
                CHECK(memcmp(a, c, sizeof(uint32_t) * nw * rows) == 0);
            }
        }
        su_world_destroy(w1);
        su_world_destroy(w2);
        su_world_destroy(w3);
    }
    printf("space3d prox: %u scenes equal to brute force, %u truncated (repeat-only), %u proximity hits%c", compared,
           flagged, found, 10);
    CHECK(compared >= 20u && found > 50u);
    D = base;
}

/* ---------- space3d.query_shape: capsule movers (2026-10-18) ---------- */

static void cap_mover(uint32_t r, SuV3 prev, SuV3 pos, int64_t radius, SuQuat q, int64_t z0, int64_t z1) {
    mover(r, prev, pos, radius, 0);
    set_rot(r, q);
    setq(SPACE3D_F_Z0, r, z0);
    setq(SPACE3D_F_Z1, r, z1);
}

static int near64(int64_t a, int64_t b, int64_t tol) {
    return (a > b ? a - b : b - a) <= tol;
}

static void test_shape(void) {
    static uint32_t a[40 * MAXR], b[40 * MAXR], c[40 * MAXR];
    const SuHostOpDesc *prox = &g_space3d_query_prox_desc;
    SuSha256 sh;
    uint8_t h[32];
    char msg[256];
    uint32_t i, sv = 0, gen = 0, seed, compared = 0, flagged = 0, contacts = 0, nw, np;
    SuLirModule *decl = NULL;
    Scene s;
    SuWorld *w = NULL;
    /* 90 degrees about y: local +z -> world +x. */
    SuQuat qy;
    qy.w = su_q32_sqrt(ONE / 2);
    qy.x = 0;
    qy.y = qy.w;
    qy.z = 0;

    D = &g_space3d_query_shape_desc;
    CHECK(su_host_desc_validate(D) == SU_OK);
    CHECK(D->field_count == prox->field_count + 4u);
    for (i = 0; i < prox->field_count; i++) {
        CHECK(D->fields[i].id == prox->fields[i].id && D->fields[i].access == prox->fields[i].access &&
              strcmp(D->fields[i].name, prox->fields[i].name) == 0);
    }
    su_sha256_init(&sh);
    su_sha256_update(&sh, space3d_shape_constants_text(), strlen(space3d_shape_constants_text()));
    su_sha256_final(&sh, h);
    CHECK(memcmp(h, D->constants_hash, 32) == 0);
    CHECK(su_host_load("profiles/space3d/space3d_query_shape.capsule.json", NULL, D, &decl, &sv, &gen, msg,
                       sizeof msg) == SU_OK);
    su_host_decl_free(decl);
    decl = NULL;
    CHECK(su_host_load("profiles/space3d/space3d_query_shape.capsule.json", NULL, prox, &decl, &sv, &gen, msg,
                       sizeof msg) != SU_OK && decl == NULL);
    CHECK(su_host_load("profiles/space3d/space3d_query_prox.capsule.json", NULL, D, &decl, &sv, &gen, msg,
                       sizeof msg) != SU_OK && decl == NULL);

    /* Known: a long mover (z in [-8, 8], r 2) moving +x by 20 from the origin past a sphere (r 1) at (10, 0, 7): the
     * segment passes 3 from the centre once x = 7: toi 0.35, normal -x. A sphere mover (row 2, lanes zero) on a
     * parallel path 50 m away misses its own sphere (row 3) at the same offset. Margins: the capsule mover's
     * proximity to the sphere at its end position (x = 20) is 10 - 3 = 7. */
    scene_init(&s, 4);
    cap_mover(0, V(0, 0, 0), V(Q(20), 0, 0), Q(2), QID, -Q(8), Q(8));
    setq(SPACE3D_F_PROX_MARGIN, 0, Q(20));
    sphere(1, V(Q(10), 0, Q(7)), ONE);
    mover(2, V(0, Q(50), 0), V(Q(20), Q(50), 0), Q(2), 0);
    sphere(3, V(Q(10), Q(50), Q(7)), ONE);
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w != NULL) {
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 1u && rd32(w, 42u, 0) == 1u);
        CHECK(near64(rdq(w, 44u, 0), su_q32_div(Q(7), Q(20)), 2));
        CHECK(rdq(w, 46u, 0) == -ONE && rdq(w, 48u, 0) == 0 && rdq(w, 50u, 0) == 0);
        CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 0) == 1u && near64(rdq(w, SPACE3D_F_PROX_DIST, 0), Q(7), 4));
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 2) == 0u);
        su_world_destroy(w);
        w = NULL;
    }
    /* Orientation: the same capsule rotated so its axis is world x, moving +x by 40 towards a sphere at (30, 0, 1):
     * its front end x + 8 is 3 from the centre when 22 - x = sqrt(8): toi (22 - sqrt 8) / 40 = 0.47929. */
    scene_init(&s, 2);
    cap_mover(0, V(0, 0, 0), V(Q(40), 0, 0), Q(2), qy, -Q(8), Q(8));
    sphere(1, V(Q(30), 0, ONE), ONE);
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w != NULL) {
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 1u);
        CHECK(near64(rdq(w, 44u, 0), (int64_t)((22.0 - 2.8284271247461903) / 40.0 * 4294967296.0), 64));
        su_world_destroy(w);
        w = NULL;
    }
    /* Two capsule movers (axes along z, r 1 each) passing head-on along x, 1.5 apart in y and overlapping in z: R = 2,
     * contact at an x gap of sqrt(4 - 2.25); both report each other at the same toi with opposite normals. */
    scene_init(&s, 2);
    cap_mover(0, V(-Q(10), 0, 0), V(Q(10), 0, 0), ONE, QID, -Q(4), Q(4));
    cap_mover(1, V(Q(10), Q(3) / 2, Q(2)), V(-Q(10), Q(3) / 2, Q(2)), ONE, QID, -Q(4), Q(4));
    CHECK(run_scene(&s, &w) == SU_OK);
    if (w != NULL) {
        int64_t t0 = rdq(w, 44u, 0), t1 = rdq(w, 44u, 1);
        double want = (20.0 - 1.3228756555322954) / 40.0; /* sqrt(1.75) */
        CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 1u && rd32(w, SPACE3D_F_HIT_COUNT, 1) == 1u);
        CHECK(rd32(w, 42u, 0) == 1u && rd32(w, 42u, 1) == 0u);
        CHECK(near64(t0, (int64_t)(want * 4294967296.0), 64) && near64(t1, t0, 64));
        CHECK(near64(rdq(w, 46u, 0), -rdq(w, 46u, 1), 64) && near64(rdq(w, 48u, 0), -rdq(w, 48u, 1), 64));
        su_world_destroy(w);
        w = NULL;
    }
    /* Tie-break: two spheres touched at the same toi by the two ends of the segment (z = +/-8): contacts ordered by
     * slot at equal toi; the proximity tie goes to the lower slot; swapping the slots swaps the outputs. */
    for (i = 0; i < 2u; i++) {
        uint32_t lo = 1u + i, hi = 2u - i;
        scene_init(&s, 3);
        cap_mover(0, V(0, 0, 0), V(Q(20), 0, 0), Q(2), QID, -Q(8), Q(8));
        setq(SPACE3D_F_PROX_MARGIN, 0, Q(30));
        sphere(lo, V(Q(10), 0, Q(9)), ONE);
        sphere(hi, V(Q(10), 0, -Q(9)), ONE);
        CHECK(run_scene(&s, &w) == SU_OK);
        if (w != NULL) {
            CHECK(rd32(w, SPACE3D_F_HIT_COUNT, 0) == 2u && rdq(w, 44u, 0) == rdq(w, 54u, 0));
            CHECK(rd32(w, 42u, 0) == 1u && rd32(w, 52u, 0) == 2u);
            CHECK(rdq(w, 50u, 0) == -rdq(w, 60u, 0));
            CHECK((rdq(w, 50u, 0) < 0) == (i == 0)); /* slot 1 is the upper sphere for i = 0 */
            CHECK(rd32(w, SPACE3D_F_PROX_SLOT, 0) == 1u);
            su_world_destroy(w);
            w = NULL;
        }
    }

    /* Random scenes: (1) z lanes zero: every lane equals space3d.query_prox (random margins); (2) random capsule
     * movers: broadphase equals brute force on every lane when nothing is truncated; repeat is bit-identical. */
    nw = write_lanes();
    for (seed = 1; seed <= 30; seed++) {
        uint32_t rows = 64u + (seed * 151u) % 700u, r;
        int64_t span = 20 + (int64_t)(seed % 4u) * 40;
        SuWorld *w1 = NULL, *w2 = NULL, *w3 = NULL;
        int flags_any = 0;
        g_rng = seed * UINT64_C(15485863);
        random_scene(&s, rows, span);
        for (r = 0; r < rows; r++) {
            if (g_col[SPACE3D_F_KIND][r] == SPACE3D_KIND_MOVER) {
                setq(SPACE3D_F_PROX_MARGIN, r, (int64_t)(rng() % (uint64_t)(8 * ONE)) - ONE);
            }
        }
        D = prox;
        np = write_lanes();
        CHECK(run_scene(&s, &w1) == SU_OK);
        if (w1 != NULL) {
            snapshot_out(w1, rows, a);
            su_world_destroy(w1);
            w1 = NULL;
        }
        D = &g_space3d_query_shape_desc;
        CHECK(run_scene(&s, &w1) == SU_OK);
        if (w1 != NULL) {
            snapshot_out(w1, rows, b);
            CHECK(np == nw && memcmp(a, b, sizeof(uint32_t) * np * rows) == 0); /* lanes zero: query_prox exactly */
            su_world_destroy(w1);
            w1 = NULL;
        }
        for (r = 0; r < rows; r++) {
            if (g_col[SPACE3D_F_KIND][r] == SPACE3D_KIND_MOVER && rng() % 4u != 0u) {
                SuQuat q0;
                q0.w = rq(1);
                q0.x = rq(1);
                q0.y = rq(1);
                q0.z = rq(1);
                set_rot(r, su_quat_normalize(q0));
                setq(SPACE3D_F_Z0, r, -(int64_t)(rng() % (uint64_t)(10 * ONE)));
                setq(SPACE3D_F_Z1, r, (int64_t)(rng() % (uint64_t)(10 * ONE)));
            }
        }
        CHECK(run_scene(&s, &w1) == SU_OK);
        CHECK(run_scene(&s, &w2) == SU_OK);
        space3d_query_ctx()->brute_force = 1;
        CHECK(run_scene(&s, &w3) == SU_OK);
        space3d_query_ctx()->brute_force = 0;
        if (w1 != NULL && w2 != NULL && w3 != NULL) {
            snapshot_out(w1, rows, a);
            snapshot_out(w2, rows, b);
            snapshot_out(w3, rows, c);
            CHECK(memcmp(a, b, sizeof(uint32_t) * nw * rows) == 0);
            for (r = 0; r < rows; r++) {
                flags_any |= (rd32(w1, SPACE3D_F_HIT_FLAGS, r) &
                              (SPACE3D_FLAG_CAND_TRUNCATED | SPACE3D_FLAG_SWEEP_BUDGET)) != 0u;
                contacts += rd32(w1, SPACE3D_F_HIT_COUNT, r);
            }
            if (flags_any) {
                flagged++;
            } else {
                compared++;
                CHECK(memcmp(a, c, sizeof(uint32_t) * nw * rows) == 0); /* capsule bounds lose nothing */
            }
        }
        su_world_destroy(w1);
        su_world_destroy(w2);
        su_world_destroy(w3);
    }
    printf("space3d shape: %u capsule scenes equal to brute force, %u truncated (repeat-only), %u contacts%c",
           compared, flagged, contacts, 10);
    CHECK(compared >= 20u && contacts > 100u);
    D = &g_space3d_query_desc;
}

int main(void) {
    test_descriptor();
    test_known();
    test_random();
    test_limits();
    test_prox();
    test_shape();
    printf("test_space3d_ops: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
