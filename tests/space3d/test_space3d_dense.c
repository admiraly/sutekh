/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Dense scenes above the space3d_ops_v1 sweep budget (issue 163), through the public host API (records, capacity,
 * su_host_step, su_host_operator_counters):
 *   - converge: 15,000 small movers converging on four large targets (three static spheres and one large mover).
 *     Every small mover certainly hits its target this tick (it moves 3 m straight at the target's centre from at
 *     most 2.51 m off its surface), so the target must be among its contacts unless the mover is flagged.
 *     space3d.query (v1) runs out of its 2^22 sweep pairs, flags every mover and loses contacts; space3d.query2 (v2)
 *     must lose none and flag none of the small movers.
 *   - pileup: the same plus 4,000 movers fired from one shared point. Under v2 the pair budget cuts only the piled
 *     movers (flag 4) and every small mover on a shell still finds its target.
 *   - counter semantics (last run and totals against the per-row flags), no allocation in the tick (glibc builds,
 *     not under AddressSanitizer), determinism (pinned su_host_digest per scene and family: equal on every compiler,
 *     build type and tier, since the operator is compiled C).
 * Usage: test_space3d_dense [--bench TICKS] (run from the repository root). --bench times TICKS ticks per scene
 * and family (median, minimum) for the performance comparison. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ir/su_num.h"
#include "space3d_ops.h"
#include "sutekh/host.h"
#include "sutekh/status.h"

/* ---------- allocation counting (glibc, as tests/host/test_host_rollback.c) ---------- */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SU_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define SU_TEST_ASAN 1
#endif
#if defined(__linux__) && defined(__GLIBC__) && !defined(SU_TEST_ASAN)
#define SU_TEST_COUNT_ALLOC 1
extern void *__libc_malloc(size_t n);
extern void *__libc_calloc(size_t n, size_t m);
extern void *__libc_realloc(void *p, size_t n);
static volatile int g_count_alloc;
static unsigned long g_allocs;
void *malloc(size_t n) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __libc_malloc(n);
}
void *calloc(size_t n, size_t m) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __libc_calloc(n, m);
}
void *realloc(void *p, size_t n) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __libc_realloc(p, n);
}
#endif

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
#define NONE 0xFFFFFFFFu
#define TARGETS 4u
#define SHELL_MOVERS 15000u /* converge */
#define PILE_SHELL 1000u    /* pileup: shell movers */
#define PILE_MOVERS 3000u   /* pileup: movers fired from one point */

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
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b->p = malloc((size_t)n + 1u);
    b->n = b->p != NULL ? fread(b->p, 1, (size_t)n, f) : 0u;
    fclose(f);
    return b->p != NULL && b->n == (size_t)n;
}

static uint64_t g_rng;
static uint64_t rng(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Uniform raw q32 in [0, m) metres. */
static int64_t rq(uint32_t m) {
    return (int64_t)(rng() % ((uint64_t)m << 32));
}

/* A random unit direction (integer construction and su_v3_normalize: the same bits on every build). */
static SuV3 rdir(void) {
    for (;;) {
        SuV3 d;
        d.x = rq(2) - ONE;
        d.y = rq(2) - ONE;
        d.z = rq(2) - ONE;
        if (su_v3_len_sq(d) > ONE / 16 && su_v3_len_sq(d) <= ONE) {
            return su_v3_normalize(d);
        }
    }
}

static uint32_t fid(const SuHostWorld *w, const char *name) {
    uint32_t id = 0;
    CHECK(su_host_field_id(w, name, &id) == SU_OK);
    return id;
}

static void put64(const SuHostWorld *w, SuHostValue *v, uint32_t *n, const char *name, int64_t x) {
    char lo[64];
    uint64_t u = (uint64_t)x;
    uint32_t id;
    snprintf(lo, sizeof lo, "%s.lo", name);
    id = fid(w, lo);
    v[*n].field_id = id;
    v[*n].value = (uint32_t)u;
    v[*n + 1u].field_id = id + 1u;
    v[*n + 1u].value = (uint32_t)(u >> 32);
    *n += 2u;
}

/* Spawns one row: kind, prev, pos, radius (rot identity). Returns its slot. */
static uint32_t spawn(SuHostWorld *w, uint32_t kind, SuV3 prev, SuV3 pos, int64_t radius) {
    SuHostValue v[32];
    SuHostError e;
    uint32_t n = 0;
    uint64_t ent = 0;
    v[n].field_id = fid(w, "kind");
    v[n++].value = kind;
    put64(w, v, &n, "prev_x", prev.x);
    put64(w, v, &n, "prev_y", prev.y);
    put64(w, v, &n, "prev_z", prev.z);
    put64(w, v, &n, "pos_x", pos.x);
    put64(w, v, &n, "pos_y", pos.y);
    put64(w, v, &n, "pos_z", pos.z);
    put64(w, v, &n, "rot_w", ONE);
    put64(w, v, &n, "radius", radius);
    CHECK(su_host_spawn(w, v, n, 0, &ent, &e) == SU_OK);
    return (uint32_t)(ent & 0xFFFFFFFFu);
}

static SuV3 V(int64_t x, int64_t y, int64_t z) {
    SuV3 v;
    v.x = x * ONE;
    v.y = y * ONE;
    v.z = z * ONE;
    return v;
}

typedef struct Scene {
    uint32_t rows;
    uint32_t target_of[SHELL_MOVERS + TARGETS]; /* per slot: its target's slot, else NONE */
    int64_t toi[SHELL_MOVERS + TARGETS];        /* per shell mover: the time of impact on its target (gap / speed) */
    uint32_t pile[SHELL_MOVERS + TARGETS];      /* per slot: 1 for a piled mover */
} Scene;

/* Builds the scene into w: shell movers first (slots 0..), then the pile, then the targets (highest slots, so slot
 * order favours nothing). Movers: radius 0.25 m, 3 m per tick. Targets: radius 20 m, 50 m apart in a square. */
/* sparse: SHELL_MOVERS movers spread through a 400 m cube at up to 3 m per tick, with 400 static spheres; no target
 * bookkeeping. Both families must agree on every lane (no truncation expected). */
static void build_sparse(SuHostWorld *w, Scene *sc) {
    uint32_t i;
    g_rng = UINT64_C(1631);
    memset(sc, 0, sizeof *sc);
    for (i = 0; i < SHELL_MOVERS + 400u; i++) {
        SuV3 p0;
        p0.x = rq(400) - 200 * ONE;
        p0.y = rq(400) - 200 * ONE;
        p0.z = rq(400) - 200 * ONE;
        if (i < SHELL_MOVERS) {
            spawn(w, 1u, p0, su_v3_add(p0, su_v3_scale(rdir(), rq(3))), ONE / 4 + rq(1) / 2);
        } else {
            spawn(w, 2u, p0, p0, ONE + rq(6));
        }
    }
    sc->rows = 0; /* no target bookkeeping */
}

static void build(SuHostWorld *w, Scene *sc, uint32_t nshell, uint32_t npile) {
    static const int64_t cx[TARGETS][3] = {{-10, -10, 0}, {10, -10, 0}, {-10, 10, 0}, {10, 10, 0}};
    const int64_t r_t = 8 * ONE, r_m = ONE / 4, speed = 3 * ONE;
    uint32_t i, tslot[TARGETS];
    g_rng = UINT64_C(163);
    memset(sc, 0, sizeof *sc);
    for (i = 0; i < nshell; i++) {
        uint32_t t = i % TARGETS;
        SuV3 c = V(cx[t][0], cx[t][1], cx[t][2]), d = rdir();
        int64_t gap = ONE / 100 + rq(2) + rq(1) / 2; /* [0.01, 2.51) m off the surface */
        SuV3 p0 = su_v3_add(c, su_v3_scale(d, r_t + r_m + gap));
        SuV3 p1 = su_v3_sub(p0, su_v3_scale(d, speed));
        uint32_t s = spawn(w, 1u, p0, p1, r_m);
        sc->target_of[s] = t; /* resolved to a slot below */
        sc->toi[s] = su_q32_div(gap, speed);
    }
    for (i = 0; i < npile; i++) {
        SuV3 p0 = V(0, 0, 40), d = rdir();
        uint32_t s = spawn(w, 1u, p0, su_v3_add(p0, su_v3_scale(d, speed)), r_m);
        sc->target_of[s] = NONE;
        sc->pile[s] = 1;
    }
    for (i = 0; i < TARGETS; i++) {
        SuV3 c = V(cx[i][0], cx[i][1], cx[i][2]);
        tslot[i] = spawn(w, i == 3u ? 1u : 2u, c, c, r_t); /* target 3 is a (stationary) mover */
        sc->target_of[tslot[i]] = NONE;
    }
    sc->rows = nshell + npile + TARGETS;
    for (i = 0; i < nshell; i++) {
        sc->target_of[i] = tslot[sc->target_of[i]];
    }
}

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Shell movers: found (the target is contact 0 or 1); displaced (two contacts no later than the target's, flag 2:
 * what the brute force reports too); lost with a flag (1 or 4: broadphase truncation or budget); lost with no flag
 * (a silent loss: must never happen). */
typedef struct Result {
    uint32_t found, lost, displaced, lost_flagged, shell_bp_flagged, pile_flag4, shell_flag4;
    SuHostOperatorCounters k;
    char digest[65];
    uint64_t lanes; /* FNV-1a of the result lanes (comparable across families) */
    double ms_median, ms_min;
} Result;

/* One scene on one record: create, build, step `ticks` ticks (the first warms up; allocations are counted on the
 * rest), check counters against the lanes. */
static void run(const Bytes *record, uint32_t nshell, uint32_t npile, uint32_t ticks, Result *res) {
    static const char *const lanes[] = {"hit_count", "hit_flags", "c0_slot", "c0_gen", "c0_toi.lo", "c0_toi.hi",
                                        "c0_nx.lo",  "c0_nx.hi",  "c1_slot", "c1_gen", "c1_toi.lo", "c1_toi.hi",
                                        "c1_nz.lo",  "c1_nz.hi",  "gate_dir"};
    static Scene sc;
    static double ms[1024];
    SuHostCapsule cap;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint64_t done = 0, rows = 0;
    const uint32_t *hc, *fl, *c0, *c1, *t1lo, *t1hi;
    uint32_t s, t, nfl[3] = {0, 0, 0}, nany = 0;
    uint8_t dg[32];
    memset(res, 0, sizeof *res);
    cap.capsule_json = record->p;
    cap.capsule_len = record->n;
    cap.lir_json = NULL;
    cap.lir_len = 0;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.max_entities = nshell == 0u ? SHELL_MOVERS + 400u : nshell + npile + TARGETS;
    d.tick_rate_hz = 60;
    d.max_struct_per_tick = 1;
    d.event_log_max = 64;
    CHECK(su_host_world_create(&d, &w, &e) == SU_OK);
    if (w == NULL) {
        fprintf(stderr, "world: %s\n", e.message);
        return;
    }
    if (nshell == 0u) {
        build_sparse(w, &sc);
    } else {
        build(w, &sc, nshell, npile);
    }
    CHECK(su_host_step(w, 1, &done, &e) == SU_OK);
    for (t = 1; t < ticks && t < 1024u; t++) {
        double t0;
#ifdef SU_TEST_COUNT_ALLOC
        g_allocs = 0;
        g_count_alloc = 1;
#endif
        t0 = now_ms();
        CHECK(su_host_step(w, 1, &done, &e) == SU_OK);
        ms[t - 1u] = now_ms() - t0;
#ifdef SU_TEST_COUNT_ALLOC
        g_count_alloc = 0;
        CHECK(g_allocs == 0u);
#endif
    }
    if (ticks > 1u) {
        qsort(ms, ticks - 1u, sizeof ms[0], cmp_d);
        res->ms_median = ms[(ticks - 1u) / 2u];
        res->ms_min = ms[0];
    }
    CHECK(su_host_column(w, fid(w, "hit_count"), &hc, &rows) == SU_OK);
    CHECK(su_host_column(w, fid(w, "hit_flags"), &fl, &rows) == SU_OK);
    CHECK(su_host_column(w, fid(w, "c0_slot"), &c0, &rows) == SU_OK);
    CHECK(su_host_column(w, fid(w, "c1_slot"), &c1, &rows) == SU_OK);
    CHECK(su_host_column(w, fid(w, "c1_toi.lo"), &t1lo, &rows) == SU_OK);
    CHECK(su_host_column(w, fid(w, "c1_toi.hi"), &t1hi, &rows) == SU_OK);
    for (s = 0; s < rows; s++) {
        uint32_t f = fl[s];
        nfl[0] += (f & 1u) != 0u;
        nfl[1] += (f & 2u) != 0u;
        nfl[2] += (f & 4u) != 0u;
        nany += f != 0u;
        if (s >= sc.rows) {
            continue;
        }
        if (sc.pile[s]) {
            res->pile_flag4 += (f & 4u) != 0u;
        }
        if (sc.target_of[s] == NONE) {
            continue;
        }
        res->shell_bp_flagged += (f & 5u) != 0u;
        res->shell_flag4 += (f & 4u) != 0u;
        if ((hc[s] > 0u && c0[s] == sc.target_of[s]) || (hc[s] > 1u && c1[s] == sc.target_of[s])) {
            res->found++;
        } else if (hc[s] == 2u && (f & 2u) != 0u &&
                   (int64_t)(((uint64_t)t1hi[s] << 32) | t1lo[s]) <= sc.toi[s] + (ONE >> 20)) {
            res->displaced++;
        } else if ((f & 5u) != 0u) {
            res->lost_flagged++;
        } else {
            res->lost++;
        }
    }
    /* Counters: the last run equals the lanes; totals sum the runs. */
    {
        SuHostOperatorCounters k, bad;
        memset(&k, 0, sizeof k);
        k.struct_size = sizeof k;
        CHECK(su_host_operator_counters(w, 0, &k) == SU_OK);
        CHECK(k.runs == ticks);
        CHECK(k.rows_cand_truncated == nfl[0] && k.rows_contact_truncated == nfl[1] && k.rows_budget == nfl[2]);
        CHECK(k.rows_truncated == nany);
        CHECK(k.runs_truncated == (nany > 0u ? ticks : 0u)); /* the scene does not change between ticks */
        CHECK(k.total_rows_truncated == (uint64_t)nany * ticks && k.total_rows_budget == (uint64_t)nfl[2] * ticks);
        CHECK(k.runs_budget == (k.budget_hit ? ticks : 0u));
        CHECK(k.total_pairs_skipped == k.pairs_skipped * ticks);
        res->k = k;
        bad = k;
        bad.struct_size = 8;
        CHECK(su_host_operator_counters(w, 0, &bad) == SU_E_INVALID_ARGUMENT);
        bad.struct_size = sizeof bad;
        bad.reserved = 1;
        CHECK(su_host_operator_counters(w, 0, &bad) == SU_E_INVALID_ARGUMENT);
        bad.reserved = 0;
        CHECK(su_host_operator_counters(w, 1, &bad) == SU_E_INVALID_ARGUMENT);
        CHECK(su_host_operator_counters(NULL, 0, &bad) == SU_E_INVALID_ARGUMENT);
    }
    {
        uint32_t li;
        uint64_t hsh = UINT64_C(0xcbf29ce484222325);
        for (li = 0; li < sizeof lanes / sizeof lanes[0]; li++) {
            const uint32_t *col;
            uint64_t r;
            CHECK(su_host_column(w, fid(w, lanes[li]), &col, &rows) == SU_OK);
            for (r = 0; r < rows; r++) {
                hsh = (hsh ^ col[r]) * UINT64_C(0x100000001b3);
            }
        }
        res->lanes = hsh;
    }
    CHECK(su_host_digest(w, dg) == SU_OK);
    su_host_hex(dg, res->digest);
    su_host_world_destroy(w);
}

static void report(const char *name, const Result *r) {
    printf("%-22s found %5u displaced %4u lost: flagged %5u, unflagged %u | shell flag 1/4 %5u (4: %5u) | "
           "pile flag 4 %4u | "
           "rows flag 1/2/4 %llu/%llu/%llu | pairs %llu of budget %llu, skipped %llu, budget_hit %llu | "
           "%.2f ms/tick median, %.2f min | digest %.16s\n",
           name, r->found, r->displaced, r->lost_flagged, r->lost, r->shell_bp_flagged, r->shell_flag4, r->pile_flag4,
           (unsigned long long)r->k.rows_cand_truncated, (unsigned long long)r->k.rows_contact_truncated,
           (unsigned long long)r->k.rows_budget, (unsigned long long)r->k.pairs_examined,
           (unsigned long long)r->k.pair_budget, (unsigned long long)r->k.pairs_skipped,
           (unsigned long long)r->k.budget_hit, r->ms_median, r->ms_min, r->digest);
}

int main(int argc, char **argv) {
    Bytes v1, v2;
    Result a1, a2, b1, b2, c1, c2, c3;
    uint32_t ticks = 3;
    if (argc == 3 && strcmp(argv[1], "--bench") == 0) {
        ticks = (uint32_t)strtoul(argv[2], NULL, 10);
        ticks = ticks < 2u ? 2u : ticks > 1024u ? 1024u : ticks;
    }
    if (!slurp("profiles/space3d/space3d_query.capsule.json", &v1) ||
        !slurp("profiles/space3d/space3d_query2.capsule.json", &v2)) {
        fprintf(stderr, "run from the repository root\n");
        return 1;
    }
    run(&v1, SHELL_MOVERS, 0, ticks, &a1);
    run(&v2, SHELL_MOVERS, 0, ticks, &a2);
    run(&v1, PILE_SHELL, PILE_MOVERS, ticks, &b1);
    run(&v2, PILE_SHELL, PILE_MOVERS, ticks, &b2);
    run(&v1, 0, 0, ticks, &c1);
    run(&v2, 0, 0, ticks, &c2);
    space3d_test_force_grid(1); /* the same scene through the grid instead of the sweep */
    run(&v2, 0, 0, ticks, &c3);
    space3d_test_force_grid(0);
    report("converge v1 (query)", &a1);
    report("converge v2 (query2)", &a2);
    report("pileup v1 (query)", &b1);
    report("pileup v2 (query2)", &b2);
    report("sparse v1 (query)", &c1);
    report("sparse v2 (query2)", &c2);
    report("sparse v2, grid forced", &c3);

    /* No scene and family ever loses a certain contact without a flag. */
    CHECK(a1.lost == 0u && a2.lost == 0u && b1.lost == 0u && b2.lost == 0u);
    /* converge is well above the old budget: v1 stops its sweep, flags every mover and loses most contacts. */
    CHECK(a1.k.budget_hit == 1u && a1.k.pairs_examined == (1u << 22) && a1.k.rows_budget == SHELL_MOVERS + 1u);
    CHECK(a1.lost_flagged > SHELL_MOVERS / 2u);
    /* v2, converge: within its budget; every certain contact found or displaced by two earlier ones (as for the
     * brute force). Shell movers may have flag 1 (more than 32 overlapping neighbours: the target, a coarser grid
     * level, is always kept); none has flag 4. */
    CHECK(a2.k.budget_hit == 0u && a2.k.rows_budget == 0u && a2.k.pairs_skipped == 0u);
    CHECK(a2.k.pair_budget == 1024u * (SHELL_MOVERS + TARGETS) && a2.k.pairs_examined < a2.k.pair_budget);
    CHECK(a2.lost_flagged == 0u && a2.shell_flag4 == 0u);
    CHECK(a2.found + a2.displaced == SHELL_MOVERS && a2.found > SHELL_MOVERS / 2u);
    /* pileup: 3,000 movers fired from one point next to 1,000 converging ones. v1 runs out and flags everything; v2
     * cuts only the piled movers (flag 4) and the converging movers find every contact. */
    CHECK(b1.k.budget_hit == 1u && b1.k.rows_budget == PILE_SHELL + PILE_MOVERS + 1u);
    CHECK(b2.k.budget_hit == 1u && b2.k.pairs_skipped > 0u && b2.k.pairs_examined <= b2.k.pair_budget);
    CHECK(b2.k.pair_budget == (1u << 22));
    CHECK(b2.pile_flag4 > PILE_MOVERS / 2u && b2.shell_flag4 == 0u && b2.k.rows_budget == b2.pile_flag4);
    CHECK(b2.lost_flagged == 0u && b2.shell_bp_flagged == 0u && b2.found + b2.displaced == PILE_SHELL);
    /* sparse: no truncation in either family, so both equal the brute force and each other on every lane. */
    CHECK(c1.k.rows_truncated == 0u && c2.k.rows_truncated == 0u && c1.lanes == c2.lanes);
    CHECK(c3.k.rows_truncated == 0u && c3.lanes == c2.lanes && strcmp(c3.digest, c2.digest) == 0);
    /* Determinism: the same state on every build (compiled C operator; tiers do not apply to it). */
    if (ticks == 3u) {
        static const char *const want[6] = {
            "986564b2559a98d1c4b752fe5a92e3eaa16e99886e527d9ae7546c18f7f6a9e7",
            "1452a33e8e7cc147abf7e19e0b5e4bf243b44266fe9a4e073efa259f03e3d2f4",
            "06ef56f94dc8d1399317ebce8cb135282f9b6ed69deaa933e3346143c2b04625",
            "ea0030f25e31dfea8d2a126b8b7ed3d70eaa1747cae576bc6d7eaea00da5d51c",
            "19a9a6fbb57f8c904285611c76caa011f3c662438c24994bc956d6682613a0b1",
            "e68340103d7ccf449a856aadfe8d754f3d859216b1b88787e6066d56e7b43b7b",
        };
        const Result *r[6] = {&a1, &a2, &b1, &b2, &c1, &c2};
        uint32_t i;
        for (i = 0; i < 6u; i++) {
            if (want[i][0] != '\0') {
                CHECK(strcmp(r[i]->digest, want[i]) == 0);
            }
            printf("digest %u %s\n", i, r[i]->digest);
        }
    }
    free(v1.p);
    free(v2.p);
    printf("test_space3d_dense: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
