/* M0-04 tests: checked scalar reference VM.
 *
 * Fixture-driven (examples/, tests/fixtures/t02, t03), generated row counts,
 * wrapper rejections (columns must stay untouched), and defensive checks on
 * hand-built modules that bypass the verifier.
 *
 * Run from the repository root (fixture paths are relative). Build with:
 *   gcc -std=c17 -Wall -Wextra -Werror -pedantic -Iinclude -Isrc
 *       src/ir/su_json.c src/ir/su_lir.c src/ir/su_sha256.c
 *       src/world/su_world.c src/vm/su_vm.c tests/vm/test_vm.c -o <scratch>/test_vm
 * then run <scratch>/test_vm from the repository root.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
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

#define CHECK_ST(expr, want)                                                        \
    do {                                                                            \
        uint32_t got_ = (expr);                                                     \
        g_checks++;                                                                 \
        if (got_ != (want)) {                                                       \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: %s -> %s, want %s\n", __FILE__, __LINE__, #expr, \
                    su_status_name(got_), su_status_name(want));                    \
        }                                                                           \
    } while (0)

/* ---------- file, path and JSON helpers ---------- */

typedef struct Buf {
    uint8_t *p;
    size_t n;
} Buf;

static int read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    long sz;
    size_t got;
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    b->p = malloc((size_t)sz + 1u);
    if (b->p == NULL) {
        fclose(f);
        return 0;
    }
    got = fread(b->p, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(b->p);
        b->p = NULL;
        return 0;
    }
    b->n = (size_t)sz;
    return 1;
}

/* Path of rel, resolved against the directory of base_file. */
static void join_rel(const char *base_file, const char *rel, char *out, size_t cap) {
    const char *slash = strrchr(base_file, '/');
    size_t dir_len = slash != NULL ? (size_t)(slash - base_file) + 1u : 0u;
    snprintf(out, cap, "%.*s%s", (int)dir_len, base_file, rel);
}

static const SuJsonLimits k_limits = {16u * 1024u * 1024u, 32u};

/* Reads and parses a JSON file. The byte buffer must outlive the doc use
 * pattern below: free the doc, then the buffer. */
static int parse_file(const char *path, Buf *b, SuJsonDoc *doc) {
    SuJsonError err;
    uint32_t st;
    if (!read_file(path, b)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    st = su_json_parse(b->p, b->n, &k_limits, doc, &err);
    if (st != SU_OK) {
        fprintf(stderr, "%s: parse %s at %zu\n", path, su_status_name(st), err.offset);
        free(b->p);
        b->p = NULL;
        return 0;
    }
    return 1;
}

static const char *json_str(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_STRING ? v->u.string.data : NULL;
}

static const SuJsonValue *json_obj(const SuJsonValue *obj, const char *key) {
    const SuJsonValue *v = obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
    return v != NULL && v->type == SU_JSON_OBJECT ? v : NULL;
}

/* Allocates and fills a u32 array from a JSON array of integers. */
static int json_u32_array(const SuJsonValue *arr, uint32_t **out, size_t *n) {
    size_t i;
    uint32_t *vals;
    if (arr == NULL || arr->type != SU_JSON_ARRAY) {
        return 0;
    }
    vals = calloc(arr->u.array.count + 1u, sizeof *vals);
    if (vals == NULL) {
        return 0;
    }
    for (i = 0; i < arr->u.array.count; i++) {
        uint64_t v = 0;
        if (!su_json_get_uint(arr->u.array.items[i], &v) || v > UINT32_MAX) {
            free(vals);
            return 0;
        }
        vals[i] = (uint32_t)v;
    }
    *out = vals;
    *n = arr->u.array.count;
    return 1;
}

/* ---------- module and world loading ---------- */

/* Verifies the capsule at cap_path with the IR it names in "source" (or with
 * lir_override when non-NULL). Returns the capsule's layout generation. */
static int load_module(const char *cap_path, const char *lir_override, SuLirModule *mod,
                       uint32_t *layout_gen) {
    Buf cb = {0};
    Buf lb = {0};
    SuJsonDoc cdoc;
    SuDiag diag;
    char source[256];
    char lir_path[512];
    uint64_t gen = 0;
    const SuJsonValue *schema;
    const char *src;
    uint32_t st;
    int ok = 0;

    if (!parse_file(cap_path, &cb, &cdoc)) {
        return 0;
    }
    schema = json_obj(cdoc.root, "schema");
    if (schema == NULL || !su_json_get_uint(su_json_member(schema, "layout_generation"), &gen) ||
        gen > UINT32_MAX) {
        goto done;
    }
    src = json_str(cdoc.root, "source");
    if (src == NULL || strlen(src) >= sizeof source) {
        goto done;
    }
    strcpy(source, src);
    if (lir_override != NULL) {
        snprintf(lir_path, sizeof lir_path, "%s", lir_override);
    } else {
        join_rel(cap_path, source, lir_path, sizeof lir_path);
    }
    if (!read_file(lir_path, &lb)) {
        fprintf(stderr, "cannot read %s\n", lir_path);
        goto done;
    }
    st = su_lir_verify(cb.p, cb.n, lb.p, lb.n, mod, &diag);
    if (st != SU_OK) {
        fprintf(stderr, "verify %s: %s (%s)\n", lir_path, su_status_name(st), diag.message);
        goto done;
    }
    *layout_gen = (uint32_t)gen;
    ok = 1;
done:
    su_json_doc_free(&cdoc);
    free(cb.p);
    free(lb.p);
    return ok;
}

/* World with the module's fields. Returns 1 on success. */
static int make_world(const SuLirModule *m, uint32_t gen, uint64_t rows, const uint32_t *ids,
                      SuWorld **w) {
    SuWorldFieldDesc fd[SU_LIMIT_FIELDS];
    SuWorldDesc d;
    uint32_t i;
    for (i = 0; i < m->field_count; i++) {
        fd[i].id = m->fields[i].id;
        fd[i].type = m->fields[i].type;
        fd[i].access = m->fields[i].access;
        fd[i].name = m->fields[i].name;
    }
    d.schema_id = m->query;
    d.schema_version = 1u;
    d.layout_generation = gen;
    d.row_count = rows;
    d.entity_ids = ids;
    d.fields = fd;
    d.field_count = m->field_count;
    return su_world_create(&d, w) == SU_OK;
}

/* Fills committed columns of w from a JSON "columns" object keyed by name. */
static int init_columns(SuWorld *w, const SuLirModule *m, const SuJsonValue *cols, uint64_t rows) {
    uint32_t i;
    for (i = 0; i < m->field_count; i++) {
        const SuJsonValue *arr = cols != NULL ? su_json_member(cols, m->fields[i].name) : NULL;
        uint32_t *vals = NULL;
        size_t n = 0;
        uint32_t st;
        if (!json_u32_array(arr, &vals, &n) || n != rows) {
            free(vals);
            return 0;
        }
        st = su_world_init_committed(w, m->fields[i].id, vals, rows);
        free(vals);
        if (st != SU_OK) {
            return 0;
        }
    }
    return 1;
}

/* Opens a world from a fixture world file. */
static int open_world(const char *world_path, const SuLirModule *m, uint32_t gen, SuWorld **w,
                      uint64_t *rows) {
    Buf b = {0};
    SuJsonDoc doc;
    uint32_t *ids = NULL;
    size_t n = 0;
    int ok = 0;
    if (!parse_file(world_path, &b, &doc)) {
        return 0;
    }
    if (!json_u32_array(su_json_member(doc.root, "entity_ids"), &ids, &n)) {
        goto done;
    }
    if (!make_world(m, gen, (uint64_t)n, ids, w)) {
        goto done;
    }
    if (!init_columns(*w, m, json_obj(doc.root, "columns"), (uint64_t)n)) {
        su_world_destroy(*w);
        *w = NULL;
        goto done;
    }
    *rows = (uint64_t)n;
    ok = 1;
done:
    free(ids);
    su_json_doc_free(&doc);
    free(b.p);
    return ok;
}

/* Runs `ticks` whole ticks of one system. Returns SU_OK when every tick
 * committed and published. */
static uint32_t run_ticks(SuWorld *w, SuExecContext *ctx, uint64_t ticks) {
    uint64_t t;
    for (t = 0; t < ticks; t++) {
        uint32_t st = su_world_begin_tick(w);
        if (st != SU_OK) {
            return st;
        }
        st = su_vm_run_system(w, ctx);
        if (st != SU_OK) {
            su_world_abort(w);
            return st;
        }
        st = su_world_publish(w);
        if (st != SU_OK) {
            return st;
        }
    }
    return SU_OK;
}

/* Compares every listed column of the committed root against expected. */
static void check_expected_columns(SuWorld *w, const SuJsonValue *expected, const char *label) {
    size_t i;
    if (expected == NULL || expected->type != SU_JSON_OBJECT) {
        CHECK(0);
        return;
    }
    for (i = 0; i < expected->u.object.count; i++) {
        const SuJsonMember *mem = &expected->u.object.members[i];
        uint32_t *want = NULL;
        size_t n = 0;
        const uint32_t *got = NULL;
        uint64_t rc = 0;
        size_t k;
        uint32_t st = su_world_read_committed_by_name(w, mem->key, &got, &rc);
        if (!json_u32_array(mem->value, &want, &n)) {
            CHECK(0);
            continue;
        }
        CHECK_ST(st, SU_OK);
        CHECK(rc == (uint64_t)n);
        if (st == SU_OK && rc == (uint64_t)n) {
            for (k = 0; k < n; k++) {
                if (got[k] != want[k]) {
                    g_failures++;
                    fprintf(stderr, "%s: column %s row %zu got %u want %u\n", label, mem->key,
                            k, got[k], want[k]);
                    break;
                }
            }
            g_checks++;
        }
        free(want);
    }
}

/* ---------- fixture runner (T01, T02, T03) ---------- */

static void run_case(const char *cap_path, const char *world_path, const SuJsonValue *c,
                     const char *label) {
    SuLirModule mod;
    SuWorld *w = NULL;
    SuExecContext *ctx = NULL;
    uint32_t gen = 0;
    uint64_t rows = 0;
    uint64_t ticks = 0;
    uint32_t st;

    memset(&mod, 0, sizeof mod);
    CHECK(load_module(cap_path, NULL, &mod, &gen));
    CHECK(su_json_get_uint(su_json_member(c, "ticks"), &ticks));
    if (!open_world(world_path, &mod, gen, &w, &rows)) {
        CHECK(0);
        su_lir_module_free(&mod);
        return;
    }
    st = su_vm_context_create(&mod, gen, &ctx);
    CHECK_ST(st, SU_OK);
    if (st == SU_OK) {
        CHECK_ST(run_ticks(w, ctx, ticks), SU_OK);
        CHECK(su_world_tick(w) == ticks);
        check_expected_columns(w, json_obj(c, "expected_columns"), label);
    }
    su_vm_context_destroy(ctx);
    su_world_destroy(w);
    su_lir_module_free(&mod);
}

static void run_fixture(const char *test_path) {
    Buf tb = {0};
    SuJsonDoc tdoc;
    char cap_path[512];
    char world_path[512];
    const char *capsule;
    const char *world;
    const SuJsonValue *cases;
    size_t i;

    if (!parse_file(test_path, &tb, &tdoc)) {
        CHECK(0);
        return;
    }
    capsule = json_str(tdoc.root, "capsule");
    world = json_str(tdoc.root, "world");
    CHECK(capsule != NULL && world != NULL);
    if (capsule != NULL && world != NULL) {
        join_rel(test_path, capsule, cap_path, sizeof cap_path);
        join_rel(test_path, world, world_path, sizeof world_path);
        cases = su_json_member(tdoc.root, "cases");
        CHECK(cases != NULL && cases->type == SU_JSON_ARRAY);
        if (cases != NULL && cases->type == SU_JSON_ARRAY) {
            for (i = 0; i < cases->u.array.count; i++) {
                const SuJsonValue *c = cases->u.array.items[i];
                const char *name = json_str(c, "name");
                run_case(cap_path, world_path, c, name != NULL ? name : test_path);
            }
        }
    }
    su_json_doc_free(&tdoc);
    free(tb.p);
}

/* ---------- movement_v2 behavior swap ---------- */

static void test_movement_v2_swap(void) {
    SuLirModule v1;
    SuLirModule v2;
    SuWorld *w = NULL;
    SuExecContext *c1 = NULL;
    SuExecContext *c2 = NULL;
    uint32_t gen = 0;
    uint64_t rows = 0;
    const uint32_t from_v1[4] = {1u, 3u, 0u, 7u};
    const uint32_t from_orig_v2[4] = {2u, 5u, 1u, 7u};
    const uint32_t swap_v2_after_v1[4] = {3u, 7u, 2u, 7u};
    const uint32_t vel_orig[4] = {1u, 2u, 1u, 0u};
    const uint32_t *got = NULL;
    uint64_t rc = 0;

    memset(&v1, 0, sizeof v1);
    memset(&v2, 0, sizeof v2);
    CHECK(load_module("examples/movement.capsule.json", NULL, &v1, &gen));
    CHECK(load_module("examples/movement.capsule.json", "examples/movement_v2.lir.json", &v2, &gen));
    CHECK(memcmp(v1.semantic_hash, v2.semantic_hash, 32) != 0);
    CHECK(open_world("examples/world.json", &v1, gen, &w, &rows));
    CHECK_ST(su_vm_context_create(&v1, gen, &c1), SU_OK);
    CHECK_ST(su_vm_context_create(&v2, gen, &c2), SU_OK);
    if (w != NULL && c1 != NULL && c2 != NULL) {
        /* v2 alone from the original state. */
        CHECK_ST(run_ticks(w, c2, 1), SU_OK);
        CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
        CHECK(rc == 4u && memcmp(got, from_orig_v2, sizeof from_orig_v2) == 0);
        CHECK_ST(su_world_read_committed_by_name(w, "velocity_x", &got, &rc), SU_OK);
        CHECK(memcmp(got, vel_orig, sizeof vel_orig) == 0);

        /* Reset by reopening, then v1 followed by a swap to v2 at a tick boundary. */
        su_vm_context_destroy(c2);
        c2 = NULL;
        su_world_destroy(w);
        w = NULL;
        CHECK(open_world("examples/world.json", &v1, gen, &w, &rows));
        CHECK_ST(run_ticks(w, c1, 1), SU_OK);
        CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
        CHECK(memcmp(got, from_v1, sizeof from_v1) == 0);
        CHECK_ST(su_vm_context_create(&v2, gen, &c2), SU_OK);
        CHECK_ST(run_ticks(w, c2, 1), SU_OK);
        CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
        CHECK(memcmp(got, swap_v2_after_v1, sizeof swap_v2_after_v1) == 0);
        CHECK(su_world_tick(w) == 2u);
    }
    su_vm_context_destroy(c1);
    su_vm_context_destroy(c2);
    su_world_destroy(w);
    su_lir_module_free(&v1);
    su_lir_module_free(&v2);
}

/* ---------- generated row counts against a C reference ---------- */

static void test_generated_rows(void) {
    static const uint64_t k_counts[] = {0u, 1u, 7u, 8u, 9u, 1024u};
    size_t c;
    for (c = 0; c < sizeof k_counts / sizeof k_counts[0]; c++) {
        uint64_t n = k_counts[c];
        uint32_t *ids = calloc((size_t)n + 1u, sizeof *ids);
        uint32_t *pos = calloc((size_t)n + 1u, sizeof *pos);
        uint32_t *vel = calloc((size_t)n + 1u, sizeof *vel);
        uint32_t *ref = calloc((size_t)n + 1u, sizeof *ref);
        uint32_t seed = 0x9E3779B9u;
        uint64_t i;
        uint64_t t;
        SuLirModule mod;
        SuWorld *w = NULL;
        SuExecContext *ctx = NULL;
        uint32_t gen = 0;
        const uint32_t *got = NULL;
        uint64_t rc = 0;

        memset(&mod, 0, sizeof mod);
        for (i = 0; i < n; i++) {
            seed = seed * 1664525u + 1013904223u;
            ids[i] = (uint32_t)(i + 1u);
            pos[i] = seed;
            seed = seed * 1664525u + 1013904223u;
            vel[i] = (i % 5u == 0u) ? 0xFFFFFFFFu : seed;
            ref[i] = pos[i];
        }
        CHECK(load_module("examples/movement.capsule.json", NULL, &mod, &gen));
        CHECK(make_world(&mod, gen, n, ids, &w));
        if (w != NULL) {
            CHECK_ST(su_world_init_committed(w, 1u, pos, n), SU_OK);
            CHECK_ST(su_world_init_committed(w, 2u, vel, n), SU_OK);
        }
        CHECK_ST(su_vm_context_create(&mod, gen, &ctx), SU_OK);
        if (w != NULL && ctx != NULL) {
            for (t = 0; t < 3u; t++) {
                for (i = 0; i < n; i++) {
                    ref[i] = ref[i] + vel[i];
                }
            }
            CHECK_ST(run_ticks(w, ctx, 3u), SU_OK);
            CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
            CHECK(rc == n);
            CHECK(n == 0u || memcmp(got, ref, (size_t)n * sizeof *ref) == 0);
            CHECK_ST(su_world_read_committed_by_name(w, "velocity_x", &got, &rc), SU_OK);
            CHECK(n == 0u || memcmp(got, vel, (size_t)n * sizeof *vel) == 0);
        }
        su_vm_context_destroy(ctx);
        su_world_destroy(w);
        su_lir_module_free(&mod);
        free(ids);
        free(pos);
        free(vel);
        free(ref);
    }
}

/* ---------- wrapper rejections ---------- */

#define NROWS 4u

typedef struct Views {
    SuWorld *w;
    SuInvocation inv;
    SuOutputs outs;
    SuColumnBinding in_orig[2];  /* position_x (1), velocity_x (2) */
    SuColumnBinding out_orig[1]; /* position_x staging */
} Views;

typedef struct Snap {
    uint32_t work_pos[NROWS];
    uint32_t work_vel[NROWS];
    uint32_t stage[NROWS];
    uint32_t commit_pos[NROWS];
    uint32_t commit_vel[NROWS];
} Snap;

static void snap_take(const Views *v, Snap *s) {
    const uint32_t *cp = NULL;
    const uint32_t *cv = NULL;
    uint64_t rc = 0;
    memcpy(s->work_pos, v->in_orig[0].data, sizeof s->work_pos);
    memcpy(s->work_vel, v->in_orig[1].data, sizeof s->work_vel);
    memcpy(s->stage, v->out_orig[0].data, sizeof s->stage);
    (void)su_world_read_committed(v->w, 1u, &cp, &rc);
    memcpy(s->commit_pos, cp, sizeof s->commit_pos);
    (void)su_world_read_committed(v->w, 2u, &cv, &rc);
    memcpy(s->commit_vel, cv, sizeof s->commit_vel);
}

static void prep(const Views *v, SuInvocation *in, SuColumnBinding *ib, SuOutputs *o,
                 SuColumnBinding *ob) {
    memcpy(ib, v->in_orig, sizeof v->in_orig);
    memcpy(ob, v->out_orig, sizeof v->out_orig);
    *in = v->inv;
    in->bindings = ib;
    *o = v->outs;
    o->bindings = ob;
}

/* Expects want, no column written, and the failure recorded on the outputs. */
static void expect_reject(const char *label, uint32_t want, SuExecContext *ctx, const SuInvocation *in,
                          SuOutputs *out, const Snap *before, const Views *v) {
    Snap now;
    uint32_t st = su_vm_invoke_v1(ctx, in, out);
    if (st != want) {
        g_failures++;
        fprintf(stderr, "%s: got %s, want %s\n", label, su_status_name(st), su_status_name(want));
    }
    g_checks++;
    snap_take(v, &now);
    CHECK(memcmp(&now, before, sizeof now) == 0);
    CHECK(out->failed_insn == UINT32_MAX);
}

#define CASE(label, want, mutate)                                                    \
    do {                                                                             \
        SuInvocation in_;                                                            \
        SuOutputs o_;                                                                \
        SuColumnBinding ib_[2];                                                      \
        SuColumnBinding ob_[1];                                                      \
        prep(&vw, &in_, ib_, &o_, ob_);                                              \
        mutate;                                                                      \
        expect_reject(label, want, ctx, &in_, &o_, &before, &vw);                    \
    } while (0)

static void test_wrapper_rejections(void) {
    SuLirModule mod;
    SuExecContext *ctx = NULL;
    Views vw;
    Snap before;
    uint32_t gen = 0;
    uint64_t rows = 0;
    const uint32_t want_stage[NROWS] = {1u, 3u, 0u, 7u};
    uint32_t i;

    memset(&mod, 0, sizeof mod);
    memset(&vw, 0, sizeof vw);
    CHECK(load_module("examples/movement.capsule.json", NULL, &mod, &gen));
    CHECK(open_world("examples/world.json", &mod, gen, &vw.w, &rows));
    CHECK_ST(su_vm_context_create(&mod, gen, &ctx), SU_OK);
    if (vw.w == NULL || ctx == NULL) {
        goto done;
    }
    CHECK_ST(su_world_begin_tick(vw.w), SU_OK);
    CHECK_ST(su_world_build_invocation(vw.w, &vw.inv), SU_OK);
    CHECK_ST(su_world_build_outputs(vw.w, &vw.outs), SU_OK);
    memcpy(vw.in_orig, vw.inv.bindings, sizeof vw.in_orig);
    memcpy(vw.out_orig, vw.outs.bindings, sizeof vw.out_orig);
    CHECK(vw.inv.binding_count == 2u && vw.outs.binding_count == 1u);
    /* Sentinel staging so any write is visible. */
    for (i = 0; i < NROWS; i++) {
        ((uint32_t *)vw.out_orig[0].data)[i] = 0xDEADBEEFu;
    }
    snap_take(&vw, &before);

    /* Null pointers and envelope. */
    CHECK_ST(su_vm_invoke_v1(NULL, &vw.inv, &vw.outs), SU_E_INVALID_ARGUMENT);
    {
        Snap now;
        snap_take(&vw, &now);
        CHECK(memcmp(&now, &before, sizeof now) == 0);
    }
    {
        SuInvocation in_ = vw.inv;
        SuColumnBinding ib_[2];
        memcpy(ib_, vw.in_orig, sizeof ib_);
        in_.bindings = ib_;
        CHECK_ST(su_vm_invoke_v1(ctx, &in_, NULL), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_vm_invoke_v1(ctx, NULL, &vw.outs), SU_E_INVALID_ARGUMENT);
        {
            Snap now;
            snap_take(&vw, &now);
            CHECK(memcmp(&now, &before, sizeof now) == 0);
        }
    }
    CASE("input struct_size", SU_E_INVALID_ARGUMENT, in_.struct_size = 0u);
    CASE("input abi_version", SU_E_INVALID_ARGUMENT, in_.abi_version = 2u);
    CASE("output struct_size", SU_E_INVALID_ARGUMENT, o_.struct_size = 0u);
    CASE("output reserved", SU_E_INVALID_ARGUMENT, o_.reserved = 1u);
    CASE("input count short", SU_E_INVALID_ARGUMENT, in_.binding_count = 1u);
    CASE("output count zero", SU_E_INVALID_ARGUMENT, o_.binding_count = 0u);
    CASE("input bindings NULL", SU_E_INVALID_ARGUMENT, in_.bindings = NULL);
    CASE("output bindings NULL", SU_E_INVALID_ARGUMENT, o_.bindings = NULL);
    CASE("binding struct_size", SU_E_INVALID_ARGUMENT, ob_[0].struct_size = 0u);
    CASE("binding stride 8", SU_E_INVALID_ARGUMENT, ib_[1].stride_bytes = 8u);
    CASE("binding elem bool", SU_E_INVALID_ARGUMENT, ob_[0].elem_type = SU_TYPE_BOOL);
    CASE("output access read", SU_E_INVALID_ARGUMENT, ob_[0].access = SU_ACCESS_READ);
    CASE("input access write", SU_E_INVALID_ARGUMENT, ib_[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE);
    CASE("input data NULL", SU_E_INVALID_ARGUMENT, ib_[1].data = NULL);
    CASE("output data misaligned", SU_E_INVALID_ARGUMENT, ob_[0].data = (uint8_t *)ob_[0].data + 1);

    /* Generation, field identity, row counts. */
    CASE("invocation generation", SU_E_SCHEMA_MISMATCH, in_.layout_generation = 2u);
    CASE("input binding generation", SU_E_SCHEMA_MISMATCH, ib_[0].layout_generation = 2u);
    CASE("output binding generation", SU_E_SCHEMA_MISMATCH, ob_[0].layout_generation = 2u);
    CASE("input field id unknown", SU_E_SCHEMA_MISMATCH, ib_[1].field_id = 99u);
    CASE("input field id duplicated", SU_E_SCHEMA_MISMATCH, ib_[1].field_id = 1u);
    CASE("output field is read-only", SU_E_SCHEMA_MISMATCH, ob_[0].field_id = 2u);
    CASE("input binding wrong length", SU_E_SCHEMA_MISMATCH, ib_[1].row_count = NROWS - 1u);
    CASE("output binding wrong length", SU_E_SCHEMA_MISMATCH, ob_[0].row_count = NROWS + 1u);
    CASE("invocation row_count mismatch", SU_E_SCHEMA_MISMATCH, in_.row_count = NROWS - 1u);

    /* Aliasing (staging overlaps any input or other staging range). */
    CASE("staging == velocity input", SU_E_INVALID_ARGUMENT, ob_[0].data = ib_[1].data);
    CASE("staging == position input (in place)", SU_E_INVALID_ARGUMENT, ob_[0].data = ib_[0].data);
    CASE("staging partially overlaps velocity", SU_E_INVALID_ARGUMENT,
         ob_[0].data = (uint32_t *)ib_[1].data + 2);

    /* Row limit (runtime-configurable). */
    CHECK_ST(su_vm_context_set_row_limit(ctx, 2u), SU_OK);
    CASE("row limit", SU_E_RESOURCE_LIMIT, (void)0);
    CHECK_ST(su_vm_context_set_row_limit(ctx, SU_LIMIT_ROWS), SU_OK);

    /* Staging untouched and the failure index is UINT32_MAX (not attributable). */
    CHECK(su_vm_context_last_failed_insn(ctx) == UINT32_MAX);

    /* A well-formed call succeeds and fills staging with the expected values. */
    CHECK_ST(su_vm_invoke_v1(ctx, &vw.inv, &vw.outs), SU_OK);
    CHECK(memcmp(vw.out_orig[0].data, want_stage, sizeof want_stage) == 0);
    CHECK(vw.outs.failed_insn == UINT32_MAX);
    CHECK_ST(su_world_abort(vw.w), SU_OK);

done:
    su_vm_context_destroy(ctx);
    su_world_destroy(vw.w);
    su_lir_module_free(&mod);
}

/* ---------- run_system preconditions ---------- */

static void test_run_system_phase(void) {
    SuLirModule mod;
    SuExecContext *ctx = NULL;
    SuWorld *w = NULL;
    uint32_t gen = 0;
    uint64_t rows = 0;
    const uint32_t *got = NULL;
    uint64_t rc = 0;
    const uint32_t orig_pos[NROWS] = {0u, 1u, 0xFFFFFFFFu, 7u};

    memset(&mod, 0, sizeof mod);
    CHECK(load_module("examples/movement.capsule.json", NULL, &mod, &gen));
    CHECK(open_world("examples/world.json", &mod, gen, &w, &rows));
    CHECK_ST(su_vm_context_create(&mod, gen, &ctx), SU_OK);
    if (w != NULL && ctx != NULL) {
        /* Outside a tick: no views, no invocation, nothing committed. */
        CHECK_ST(su_vm_run_system(w, ctx), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_vm_run_system(NULL, ctx), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_vm_run_system(w, NULL), SU_E_INVALID_ARGUMENT);
        CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
        CHECK(memcmp(got, orig_pos, sizeof orig_pos) == 0);
        CHECK(su_world_tick(w) == 0u);
    }
    su_vm_context_destroy(ctx);
    su_world_destroy(w);
    su_lir_module_free(&mod);
}

/* ---------- defensive checks on hand-built modules ---------- */

static char k_pos_name[] = "position_x";
static char k_vel_name[] = "velocity_x";

static void hand_module(SuLirModule *m, const SuInsn *insns, uint32_t count, uint32_t regs) {
    static SuLirField fields[2];
    memset(m, 0, sizeof *m);
    fields[0].id = 1u;
    fields[0].type = SU_TYPE_U32;
    fields[0].access = SU_ACCESS_READ | SU_ACCESS_WRITE;
    fields[0].name = k_pos_name;
    fields[1].id = 2u;
    fields[1].type = SU_TYPE_U32;
    fields[1].access = SU_ACCESS_READ;
    fields[1].name = k_vel_name;
    m->field_count = 2u;
    m->fields = fields;
    m->register_count = regs;
    m->insn_count = count;
    m->insns = (SuInsn *)insns;
    m->query = "demo.actor";
    m->system_id = "demo.movement";
    m->numeric_profile = "u32_mod_v1";
}

static void test_defensive(void) {
    static const SuInsn dup_store[] = {
        {SU_OP_LOAD_U32, 0u, 0u, 0u, 0u},
        {SU_OP_LOAD_U32, 1u, 1u, 0u, 0u},
        {SU_OP_ADD_U32, 2u, 0u, 1u, 0u},
        {SU_OP_STORE_U32, SU_OPERAND_NONE, 0u, 2u, 0u},
        {SU_OP_STORE_U32, SU_OPERAND_NONE, 0u, 2u, 0u},
    };
    static const SuInsn no_store[] = {
        {SU_OP_LOAD_U32, 0u, 0u, 0u, 0u},
        {SU_OP_LOAD_U32, 1u, 1u, 0u, 0u},
        {SU_OP_ADD_U32, 2u, 0u, 1u, 0u},
    };
    static const SuInsn bool_as_u32[] = {
        {SU_OP_LOAD_U32, 0u, 0u, 0u, 0u},
        {SU_OP_LOAD_U32, 1u, 1u, 0u, 0u},
        {SU_OP_EQ_U32, 2u, 0u, 1u, 0u},
        {SU_OP_ADD_U32, 3u, 2u, 0u, 0u},
        {SU_OP_STORE_U32, SU_OPERAND_NONE, 0u, 3u, 0u},
    };
    static const SuInsn bad_op[] = {
        {99u, 0u, 0u, 0u, 0u},
    };
    static const SuInsn bad_reg[] = {
        {SU_OP_CONST_U32, 9u, 1u, SU_OPERAND_NONE, SU_OPERAND_NONE},
    };
    static const SuInsn store_read_only[] = {
        {SU_OP_CONST_U32, 0u, 5u, SU_OPERAND_NONE, SU_OPERAND_NONE},
        {SU_OP_STORE_U32, SU_OPERAND_NONE, 1u, 0u, 0u},
    };
    static const SuInsn all_ops[] = {
        {SU_OP_LOAD_U32, 0u, 0u, 0u, 0u},          /* r0 = pos */
        {SU_OP_LOAD_U32, 1u, 1u, 0u, 0u},          /* r1 = vel */
        {SU_OP_CONST_U32, 2u, 2u, SU_OPERAND_NONE, SU_OPERAND_NONE}, /* r2 = 2 */
        {SU_OP_MUL_U32, 3u, 1u, 2u, 0u},           /* r3 = 2*vel */
        {SU_OP_SUB_U32, 4u, 3u, 1u, 0u},           /* r4 = r3 - vel = vel */
        {SU_OP_LT_U32, 5u, 0u, 4u, 0u},            /* r5 = pos < r4 (bool) */
        {SU_OP_SELECT_U32, 6u, 5u, 0u, 4u},        /* r6 = r5 ? pos : r4 */
        {SU_OP_ADD_U32, 7u, 6u, 0u, 0u},           /* r7 = r6 + pos */
        {SU_OP_STORE_U32, SU_OPERAND_NONE, 0u, 7u, 0u},
    };
    SuLirModule m;
    SuExecContext *ctx = NULL;
    SuWorld *w = NULL;
    SuLirModule ex;
    uint32_t gen = 0;
    uint64_t rows = 0;
    const uint32_t *got = NULL;
    uint64_t rc = 0;
    const uint32_t orig_pos[NROWS] = {0u, 1u, 0xFFFFFFFFu, 7u};

    memset(&ex, 0, sizeof ex);
    CHECK(load_module("examples/movement.capsule.json", NULL, &ex, &gen));
    CHECK(open_world("examples/world.json", &ex, gen, &w, &rows));

    /* Creation rejects malformed modules. */
    CHECK_ST(su_vm_context_create(NULL, gen, &ctx), SU_E_INVALID_ARGUMENT);
    hand_module(&m, bad_op, 1u, 4u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_E_INVALID_ARGUMENT);
    hand_module(&m, bad_reg, 1u, 4u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_E_INVALID_ARGUMENT);
    hand_module(&m, store_read_only, 2u, 4u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_E_INVALID_ARGUMENT);
    CHECK(ctx == NULL);

    /* Second store to one writable field in one row: SU_E_INTERNAL at insn 4. */
    hand_module(&m, dup_store, 5u, 3u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_OK);
    if (ctx != NULL && w != NULL) {
        CHECK_ST(su_world_begin_tick(w), SU_OK);
        CHECK_ST(su_vm_run_system(w, ctx), SU_E_INTERNAL);
        CHECK(su_vm_context_last_failed_insn(ctx) == 4u);
        CHECK_ST(su_world_abort(w), SU_OK);
        CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
        CHECK(memcmp(got, orig_pos, sizeof orig_pos) == 0);
    }
    su_vm_context_destroy(ctx);
    ctx = NULL;

    /* Writable field never stored: SU_E_INTERNAL, no attributable insn. */
    hand_module(&m, no_store, 3u, 3u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_OK);
    if (ctx != NULL && w != NULL) {
        CHECK_ST(su_world_begin_tick(w), SU_OK);
        CHECK_ST(su_vm_run_system(w, ctx), SU_E_INTERNAL);
        CHECK(su_vm_context_last_failed_insn(ctx) == UINT32_MAX);
        CHECK_ST(su_world_abort(w), SU_OK);
    }
    su_vm_context_destroy(ctx);
    ctx = NULL;

    /* Bool operand used as u32: SU_E_INTERNAL at insn 3. */
    hand_module(&m, bool_as_u32, 5u, 4u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_OK);
    if (ctx != NULL && w != NULL) {
        CHECK_ST(su_world_begin_tick(w), SU_OK);
        CHECK_ST(su_vm_run_system(w, ctx), SU_E_INTERNAL);
        CHECK(su_vm_context_last_failed_insn(ctx) == 3u);
        CHECK_ST(su_world_abort(w), SU_OK);
    }
    su_vm_context_destroy(ctx);
    ctx = NULL;

    /* Every opcode in one program, checked against a direct C evaluation of
     * the same expression for each row of the example world. */
    hand_module(&m, all_ops, 9u, 8u);
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_OK);
    if (ctx != NULL && w != NULL) {
        uint32_t want[NROWS];
        uint32_t pos[NROWS] = {0u, 1u, 0xFFFFFFFFu, 7u};
        uint32_t vel[NROWS] = {1u, 2u, 1u, 0u};
        uint32_t r;
        for (r = 0; r < NROWS; r++) {
            uint32_t r3 = vel[r] * 2u;
            uint32_t r4 = r3 - vel[r];
            uint32_t r5 = pos[r] < r4;
            uint32_t r6 = r5 ? pos[r] : r4;
            want[r] = r6 + pos[r];
        }
        CHECK_ST(su_world_begin_tick(w), SU_OK);
        CHECK_ST(su_vm_run_system(w, ctx), SU_OK);
        CHECK_ST(su_world_publish(w), SU_OK);
        CHECK_ST(su_world_read_committed_by_name(w, "position_x", &got, &rc), SU_OK);
        CHECK(memcmp(got, want, sizeof want) == 0);
    }
    su_vm_context_destroy(ctx);
    su_world_destroy(w);
    su_lir_module_free(&ex);
}

/* ---------- main ---------- */

int main(void) {
    static const char *const k_fixtures[] = {
        "examples/movement.test.json",
        "tests/fixtures/t02/movement_0.test.json",
        "tests/fixtures/t02/movement_1.test.json",
        "tests/fixtures/t02/movement_7.test.json",
        "tests/fixtures/t02/movement_8.test.json",
        "tests/fixtures/t02/movement_9.test.json",
        "tests/fixtures/t02/movement_1024.test.json",
        "tests/fixtures/t03/eq_edges/test.json",
        "tests/fixtures/t03/lt_edges/test.json",
        "tests/fixtures/t03/mul_overflow/test.json",
        "tests/fixtures/t03/select_min/test.json",
        "tests/fixtures/t03/sub_underflow/test.json",
        "tests/fixtures/t03/wrap_add/test.json",
    };
    size_t i;

    /* The VM entry point must match the frozen ABI typedef exactly. */
    {
        SuInvokeV1Fn fn = su_vm_invoke_v1;
        CHECK(fn != NULL);
    }

    for (i = 0; i < sizeof k_fixtures / sizeof k_fixtures[0]; i++) {
        run_fixture(k_fixtures[i]);
    }
    test_movement_v2_swap();
    test_generated_rows();
    test_wrapper_rejections();
    test_run_system_phase();
    test_defensive();

    printf("test_vm: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
