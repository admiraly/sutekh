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

/* ---------- IR 0.2 (M2-00c2): entity view, emitter, new ops ---------- */

/* Capsule: in_x (read), out_a and out_b (write). Caps 8/8. */
static const char V2_CAP[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"vm02\",\"source\":\"x\","
    "\"schema\":{\"id\":\"vm02.row\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\",\"fields\":["
    "{\"id\":1,\"name\":\"in_x\",\"type\":\"u32\",\"read\":true,\"write\":false},"
    "{\"id\":2,\"name\":\"out_a\",\"type\":\"u32\",\"read\":false,\"write\":true},"
    "{\"id\":3,\"name\":\"out_b\",\"type\":\"u32\",\"read\":false,\"write\":true}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\","
    "\"max_struct_per_row\":8,\"max_events_per_row\":8},"
    "\"ir_version\":\"0.2\",\"tests\":[\"t.json\"]}";

#define V2_HEAD "{\"ir_version\":\"0.2\",\"system_id\":\"vm02\",\"numeric_profile\":\"u32_mod_v1\"," \
                "\"query\":\"vm02.row\",\"instructions\":["
#define V2_LOADX "{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"in_x\"},"
#define V2_TAIL "]}"

/* out_a = slot + gen, out_b = tick. */
static const char V2_IR_KA[] =
    V2_HEAD V2_LOADX
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"gen_u32\",\"dst\":\"g\"},"
    "{\"op\":\"tick_u32\",\"dst\":\"t\"},"
    "{\"op\":\"add_u32\",\"dst\":\"sg\",\"a\":\"s\",\"b\":\"g\"},"
    "{\"op\":\"store_u32\",\"field\":\"out_a\",\"src\":\"sg\"},"
    "{\"op\":\"store_u32\",\"field\":\"out_b\",\"src\":\"t\"}" V2_TAIL;

/* Instructions: 0 load x, 1 const k=2, 2 c = x<k, 3 despawn_self c, 4 slot s, 5 emit c (kind 42, s, x),
 * 6 store out_a = x, 7 store out_b = s. */
static const char V2_IR_DESPAWN[] =
    V2_HEAD V2_LOADX
    "{\"op\":\"const_u32\",\"dst\":\"k\",\"value\":2},"
    "{\"op\":\"lt_u32\",\"dst\":\"c\",\"a\":\"x\",\"b\":\"k\"},"
    "{\"op\":\"despawn_self\",\"cond\":\"c\"},"
    "{\"op\":\"slot_u32\",\"dst\":\"s\"},"
    "{\"op\":\"emit_event\",\"cond\":\"c\",\"kind\":42,\"a\":\"s\",\"b\":\"x\"},"
    "{\"op\":\"store_u32\",\"field\":\"out_a\",\"src\":\"x\"},"
    "{\"op\":\"store_u32\",\"field\":\"out_b\",\"src\":\"s\"}" V2_TAIL;

/* Per row, instruction order: emit(e) [seq 0 if e], despawn(c), emit(c). */
static const char V2_IR_SEQ[] =
    V2_HEAD V2_LOADX
    "{\"op\":\"const_u32\",\"dst\":\"k\",\"value\":2},"
    "{\"op\":\"const_u32\",\"dst\":\"z\",\"value\":0},"
    "{\"op\":\"lt_u32\",\"dst\":\"c\",\"a\":\"x\",\"b\":\"k\"},"
    "{\"op\":\"eq_u32\",\"dst\":\"e\",\"a\":\"x\",\"b\":\"z\"},"
    "{\"op\":\"emit_event\",\"cond\":\"e\",\"kind\":5,\"a\":\"x\",\"b\":\"x\"},"
    "{\"op\":\"despawn_self\",\"cond\":\"c\"},"
    "{\"op\":\"emit_event\",\"cond\":\"c\",\"kind\":6,\"a\":\"x\",\"b\":\"x\"},"
    "{\"op\":\"store_u32\",\"field\":\"out_a\",\"src\":\"x\"},"
    "{\"op\":\"store_u32\",\"field\":\"out_b\",\"src\":\"x\"}" V2_TAIL;

#define V2_ROWS_MAX 8u
#define V2_LOG_MAX 64u

typedef struct V2Rec {
    char type;      /* 'D' despawn, 'E' event */
    uint32_t slot;
    uint32_t seq;
    uint32_t kind;  /* events only */
    uint32_t a;
    uint32_t b;
    uint64_t h;     /* despawn only */
} V2Rec;

typedef struct V2Log {
    V2Rec r[V2_LOG_MAX];
    uint32_t n;
    long fail_at;   /* index of the record whose callback reports overflow; -1 never */
} V2Log;

static int v2_em_despawn(void *u, uint32_t slot, uint32_t seq, uint64_t h) {
    V2Log *l = (V2Log *)u;
    if (l->fail_at >= 0 && (long)l->n == l->fail_at) {
        return 1;
    }
    if (l->n >= V2_LOG_MAX) {
        return 1;
    }
    l->r[l->n].type = 'D';
    l->r[l->n].slot = slot;
    l->r[l->n].seq = seq;
    l->r[l->n].h = h;
    l->n++;
    return 0;
}

static int v2_em_event(void *u, uint32_t slot, uint32_t seq, uint32_t kind, uint32_t a, uint32_t b) {
    V2Log *l = (V2Log *)u;
    if (l->fail_at >= 0 && (long)l->n == l->fail_at) {
        return 1;
    }
    if (l->n >= V2_LOG_MAX) {
        return 1;
    }
    l->r[l->n].type = 'E';
    l->r[l->n].slot = slot;
    l->r[l->n].seq = seq;
    l->r[l->n].kind = kind;
    l->r[l->n].a = a;
    l->r[l->n].b = b;
    l->r[l->n].h = 0;
    l->n++;
    return 0;
}

static void v2_log_init(V2Log *l, long fail_at) {
    memset(l, 0, sizeof *l);
    l->fail_at = fail_at;
}

static int v2_rec_eq(const V2Rec *x, const V2Rec *y) {
    if (x->type != y->type || x->slot != y->slot || x->seq != y->seq) {
        return 0;
    }
    if (x->type == 'E') {
        return x->kind == y->kind && x->a == y->a && x->b == y->b;
    }
    return x->h == y->h;
}

/* Compares the log with the expected records. */
static void v2_expect_log(const char *label, const V2Log *l, const V2Rec *want, uint32_t n) {
    uint32_t i;
    CHECK(l->n == n);
    if (l->n != n) {
        fprintf(stderr, "%s: %u records, want %u\n", label, (unsigned)l->n, (unsigned)n);
        return;
    }
    for (i = 0; i < n; i++) {
        if (!v2_rec_eq(&l->r[i], &want[i])) {
            g_failures++;
            fprintf(stderr, "%s: record %u differs\n", label, (unsigned)i);
        }
        g_checks++;
    }
}

static int v2_load(const char *ir, SuLirModule *m) {
    SuDiag d;
    uint32_t rc = su_lir_verify((const uint8_t *)V2_CAP, strlen(V2_CAP), (const uint8_t *)ir, strlen(ir), m, &d);
    if (rc != SU_OK) {
        fprintf(stderr, "v2 verify: %s (%s)\n", su_status_name(rc), d.message);
    }
    return rc == SU_OK;
}

/* One invocation over the V2 capsule. Returns the status and the failed instruction. */
static uint32_t v2_invoke(SuExecContext *ctx, uint64_t rows, const uint32_t *in, uint32_t *oa,
                          uint32_t *ob, uint32_t *failed) {
    SuColumnBinding ib[1];
    SuColumnBinding ob2[2];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t st;

    memset(ib, 0, sizeof ib);
    memset(ob2, 0, sizeof ob2);
    ib[0].struct_size = (uint32_t)sizeof(SuColumnBinding);
    ib[0].field_id = 1u;
    ib[0].elem_type = SU_TYPE_U32;
    ib[0].access = SU_ACCESS_READ;
    ib[0].layout_generation = 1u;
    ib[0].stride_bytes = 4u;
    ib[0].row_count = rows;
    ib[0].data = (void *)in;

    ob2[0] = ib[0];
    ob2[0].field_id = 2u;
    ob2[0].access = SU_ACCESS_WRITE;
    ob2[0].data = oa;
    ob2[1] = ob2[0];
    ob2[1].field_id = 3u;
    ob2[1].data = ob;

    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 1u;
    inv.row_count = rows;
    inv.bindings = ib;

    outs.struct_size = (uint32_t)sizeof(SuOutputs);
    outs.binding_count = 2u;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = ob2;

    st = su_vm_invoke_v1(ctx, &inv, &outs);
    *failed = outs.failed_insn;
    return st;
}

static void test_v2_known_answers(void) {
    SuLirModule m;
    SuExecContext *ctx = NULL;
    static const uint32_t in[4] = {10u, 20u, 30u, 40u};
    uint32_t oa[4], ob[4], failed;
    uint32_t gens[4] = {1u, 2u, 3u, 0xFFFFFFFEu};
    uint32_t i;

    CHECK(v2_load(V2_IR_KA, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx == NULL) {
        return;
    }

    /* Defaults without a view: slot = row index, gen 1, tick 0. */
    memset(oa, 0, sizeof oa);
    memset(ob, 0xAB, sizeof ob);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
    for (i = 0; i < 4u; i++) {
        CHECK(oa[i] == i + 1u);
        CHECK(ob[i] == 0u);
    }

    /* Generations wrap (0xFFFFFFFE + 3 mod 2^32 = 1), tick_low 0 = committed 0xFFFFFFFF + 1 wrapped. */
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, gens, 0u, 0u, NULL), SU_OK);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
    CHECK(oa[0] == 1u && oa[1] == 3u && oa[2] == 5u && oa[3] == 1u);
    CHECK(ob[0] == 0u && ob[1] == 0u && ob[2] == 0u && ob[3] == 0u);

    /* Chunk offset: slot = base + row; tick value passed through. */
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, NULL, 1024u, 7u, NULL), SU_OK);
    CHECK_ST(v2_invoke(ctx, 3u, in, oa, ob, &failed), SU_OK);
    CHECK(oa[0] == 1025u && oa[1] == 1026u && oa[2] == 1027u);
    CHECK(ob[0] == 7u && ob[1] == 7u && ob[2] == 7u);

    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

static void test_v2_despawn_emit(void) {
    SuLirModule m;
    SuExecContext *ctx = NULL;
    static const uint32_t in[4] = {1u, 5u, 0u, 2u};
    static const uint32_t gens[4] = {1u, 2u, 3u, 4u};
    uint32_t oa[4], ob[4], failed;
    V2Log log;
    SuVmEmitter em;
    /* x<2 is true in rows 0 (x=1) and 2 (x=0). Row 3 has x=2, not below 2. */
    static const V2Rec want[4] = {
        {'D', 0u, 0u, 0u, 0u, 0u, (UINT64_C(1) << 32) | 0u},
        {'E', 0u, 1u, 42u, 0u, 1u, 0u},
        {'D', 2u, 0u, 0u, 0u, 0u, (UINT64_C(3) << 32) | 2u},
        {'E', 2u, 1u, 42u, 2u, 0u, 0u},
    };

    CHECK(v2_load(V2_IR_DESPAWN, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx == NULL) {
        return;
    }
    /* The module has despawn_self and emit_event: an emitter is required. */
    v2_log_init(&log, -1L);
    em.user = &log;
    em.emit_despawn = v2_em_despawn;
    em.emit_event = v2_em_event;
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, gens, 0u, 9u, &em), SU_OK);
    memset(oa, 0, sizeof oa);
    memset(ob, 0, sizeof ob);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
    v2_expect_log("despawn_emit", &log, want, 4u);
    CHECK(oa[0] == 1u && oa[1] == 5u && oa[2] == 0u && oa[3] == 2u);  /* out_a = x */
    CHECK(ob[0] == 0u && ob[1] == 1u && ob[2] == 2u && ob[3] == 3u);  /* out_b = slot */

    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

static void test_v2_seq_order(void) {
    SuLirModule m;
    SuExecContext *ctx = NULL;
    static const uint32_t in[4] = {0u, 1u, 5u, 0u};
    uint32_t oa[4], ob[4], failed;
    V2Log log;
    SuVmEmitter em;
    /* Row 0 (x=0): e and c true -> E seq0, D seq1, E seq2. Row 1 (x=1): c only -> D seq0, E seq1.
     * Row 2: nothing. Row 3 repeats row 0. A false condition consumes no seq. */
    static const V2Rec want[8] = {
        {'E', 0u, 0u, 5u, 0u, 0u, 0u},
        {'D', 0u, 1u, 0u, 0u, 0u, (UINT64_C(1) << 32) | 0u},
        {'E', 0u, 2u, 6u, 0u, 0u, 0u},
        {'D', 1u, 0u, 0u, 0u, 0u, (UINT64_C(1) << 32) | 1u},
        {'E', 1u, 1u, 6u, 1u, 1u, 0u},
        {'E', 3u, 0u, 5u, 0u, 0u, 0u},
        {'D', 3u, 1u, 0u, 0u, 0u, (UINT64_C(1) << 32) | 3u},
        {'E', 3u, 2u, 6u, 0u, 0u, 0u},
    };

    CHECK(v2_load(V2_IR_SEQ, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx == NULL) {
        return;
    }
    v2_log_init(&log, -1L);
    em.user = &log;
    em.emit_despawn = v2_em_despawn;
    em.emit_event = v2_em_event;
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, NULL, 0u, 0u, &em), SU_OK);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
    v2_expect_log("seq_order", &log, want, 8u);

    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

static void test_v2_dead_rows(void) {
    SuLirModule m;
    SuExecContext *ctx = NULL;
    /* Dead rows 1 (x=0) and 3 (x=1) MATCH the condition, so only the skip keeps them silent. */
    static const uint32_t in[4] = {1u, 0u, 0u, 1u};
    static const uint8_t alive[4] = {1u, 0u, 1u, 0u};
    uint32_t oa[4], ob[4], failed;
    V2Log log;
    SuVmEmitter em;
    static const V2Rec want[4] = {
        {'D', 0u, 0u, 0u, 0u, 0u, (UINT64_C(1) << 32) | 0u},
        {'E', 0u, 1u, 42u, 0u, 1u, 0u},
        {'D', 2u, 0u, 0u, 0u, 0u, (UINT64_C(1) << 32) | 2u},
        {'E', 2u, 1u, 42u, 2u, 0u, 0u},
    };

    CHECK(v2_load(V2_IR_DESPAWN, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx == NULL) {
        return;
    }
    v2_log_init(&log, -1L);
    em.user = &log;
    em.emit_despawn = v2_em_despawn;
    em.emit_event = v2_em_event;
    CHECK_ST(su_vm_context_set_entity_view(ctx, alive, NULL, 0u, 0u, &em), SU_OK);
    memset(oa, 0xEE, sizeof oa);
    memset(ob, 0xEE, sizeof ob);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
    v2_expect_log("dead_rows", &log, want, 4u);
    CHECK(oa[1] == 0xEEEEEEEEu && oa[3] == 0xEEEEEEEEu);  /* dead rows: staging untouched */
    CHECK(ob[1] == 0xEEEEEEEEu && ob[3] == 0xEEEEEEEEu);
    CHECK(oa[0] == 1u && oa[2] == 0u && ob[0] == 0u && ob[2] == 2u);

    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

static void test_v2_overflow(void) {
    SuLirModule m;
    SuExecContext *ctx = NULL;
    static const uint32_t in[4] = {1u, 5u, 0u, 2u};
    uint32_t oa[4], ob[4], failed;
    V2Log log;
    SuVmEmitter em;

    CHECK(v2_load(V2_IR_DESPAWN, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx == NULL) {
        return;
    }
    /* Third record (row 2 despawn, instruction 3) reports overflow: the call fails with
     * SU_E_RESOURCE_LIMIT and failed_insn names the emitting instruction. */
    v2_log_init(&log, 2L);
    em.user = &log;
    em.emit_despawn = v2_em_despawn;
    em.emit_event = v2_em_event;
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, NULL, 0u, 0u, &em), SU_OK);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_E_RESOURCE_LIMIT);
    CHECK(failed == 3u);
    CHECK(su_vm_context_last_failed_insn(ctx) == 3u);
    CHECK(log.n == 2u);

    /* Same call with room succeeds. */
    v2_log_init(&log, -1L);
    em.user = &log;
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, NULL, 0u, 0u, &em), SU_OK);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
    CHECK(log.n == 4u);

    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

static void test_v2_emitter_rules(void) {
    SuLirModule m;
    SuExecContext *ctx = NULL;
    static const uint32_t in[4] = {1u, 5u, 0u, 2u};
    uint32_t oa[4], ob[4], failed;
    SuVmEmitter bad;
    V2Log log;
    SuVmEmitter em;

    CHECK(v2_load(V2_IR_DESPAWN, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx == NULL) {
        return;
    }
    /* No emitter: the module needs one. Columns stay untouched. */
    memset(oa, 0x5A, sizeof oa);
    memset(ob, 0x5A, sizeof ob);
    CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_E_INVALID_ARGUMENT);
    CHECK(failed == UINT32_MAX);
    CHECK(oa[0] == 0x5A5A5A5Au && ob[3] == 0x5A5A5A5Au);

    /* An incomplete emitter is refused by the setter. */
    memset(&bad, 0, sizeof bad);
    bad.emit_despawn = v2_em_despawn;
    CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, NULL, 0u, 0u, &bad), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_vm_context_set_entity_view(NULL, NULL, NULL, 0u, 0u, NULL), SU_E_INVALID_ARGUMENT);

    /* A 0.2 module without emit/despawn needs no emitter. */
    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
    CHECK(v2_load(V2_IR_KA, &m));
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    if (ctx != NULL) {
        v2_log_init(&log, -1L);
        em.user = &log;
        em.emit_despawn = v2_em_despawn;
        em.emit_event = v2_em_event;
        CHECK_ST(su_vm_context_set_entity_view(ctx, NULL, NULL, 0u, 0u, &em), SU_OK);
        CHECK_ST(v2_invoke(ctx, 4u, in, oa, ob, &failed), SU_OK);
        CHECK(log.n == 0u);
        su_vm_context_destroy(ctx);
    }
    su_lir_module_free(&m);
}

/* A 0.1 module (the movement example) skips dead rows too, with no emitter. */
static void test_v2_dead_rows_01(void) {
    SuLirModule m;
    uint32_t gen = 0;
    SuExecContext *ctx = NULL;
    static const uint32_t pos[4] = {10u, 20u, 30u, 40u};
    static const uint32_t vel[4] = {1u, 2u, 3u, 4u};
    static const uint8_t alive[4] = {1u, 0u, 1u, 1u};
    uint32_t stage[4];
    SuColumnBinding ib[2], ob[1];
    SuInvocation inv;
    SuOutputs outs;
    uint32_t failed;
    uint32_t st;

    CHECK(load_module("examples/movement.capsule.json", NULL, &m, &gen));
    if (m.field_count != 2u) {
        su_lir_module_free(&m);
        return;
    }
    CHECK_ST(su_vm_context_create(&m, gen, &ctx), SU_OK);
    if (ctx == NULL) {
        su_lir_module_free(&m);
        return;
    }
    memset(ib, 0, sizeof ib);
    memset(ob, 0, sizeof ob);
    ib[0].struct_size = (uint32_t)sizeof(SuColumnBinding);
    ib[0].field_id = 1u;
    ib[0].elem_type = SU_TYPE_U32;
    ib[0].access = SU_ACCESS_READ;
    ib[0].layout_generation = 1u;
    ib[0].stride_bytes = 4u;
    ib[0].row_count = 4u;
    ib[0].data = (void *)pos;
    ib[1] = ib[0];
    ib[1].field_id = 2u;
    ib[1].data = (void *)vel;
    ob[0] = ib[0];
    ob[0].access = SU_ACCESS_WRITE;
    ob[0].data = stage;
    memset(stage, 0xCD, sizeof stage);
    inv.struct_size = (uint32_t)sizeof(SuInvocation);
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = 1u;
    inv.binding_count = 2u;
    inv.row_count = 4u;
    inv.bindings = ib;
    outs.struct_size = (uint32_t)sizeof(SuOutputs);
    outs.binding_count = 1u;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = ob;
    CHECK_ST(su_vm_context_set_entity_view(ctx, alive, NULL, 0u, 0u, NULL), SU_OK);
    st = su_vm_invoke_v1(ctx, &inv, &outs);
    failed = outs.failed_insn;
    CHECK_ST(st, SU_OK);
    CHECK(failed == UINT32_MAX);
    CHECK(stage[0] == 11u && stage[2] == 33u && stage[3] == 44u);
    CHECK(stage[1] == 0xCDCDCDCDu);

    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

static void test_v2_hand_modules(void) {
    SuInsn insn;
    SuLirModule m;
    SuExecContext *ctx = NULL;

    /* A 0.2 opcode in a module that is not 0.2 is refused by the context builder. */
    memset(&insn, 0, sizeof insn);
    insn.op = SU_OP_SLOT_U32;
    insn.dst = 0u;
    insn.a = insn.b = insn.c = SU_OPERAND_NONE;
    hand_module(&m, &insn, 1u, 1u);
    m.ir_version_minor = 1u;
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_E_INVALID_ARGUMENT);
    CHECK(ctx == NULL);
    m.ir_version_minor = 2u;
    CHECK_ST(su_vm_context_create(&m, 1u, &ctx), SU_OK);
    su_vm_context_destroy(ctx);
}

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
    test_v2_known_answers();
    test_v2_despawn_emit();
    test_v2_seq_order();
    test_v2_dead_rows();
    test_v2_overflow();
    test_v2_emitter_rules();
    test_v2_dead_rows_01();
    test_v2_hand_modules();

    printf("test_vm: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
