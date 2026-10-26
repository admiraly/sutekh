/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.4 (LIVE_IR_SPEC "IR 0.4"): verifier acceptance and rejection with exact codes and JSON pointers, semantic
 * hash coverage of the row filter, VM known answers for the u32/i64 bit operations, div/rem, gather_* and
 * find_eq_u32 on both VM tiers, the row filter (filtered-out rows keep their values), chunked execution through
 * su_jobs_run_system_chunked (cross-row reads see the whole table whatever the chunking), and the wrapper checks of
 * the whole-table view. The independent differential test is tests/ir03/ir03_diff.py --ir04. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "ir/su_num.h"
#include "ir/su_ops03.h"
#include "runtime/jobs/su_jobs.h"
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

#define P3 "fixed_q32_v1"

static char g_cap[65536];
static char g_lir[65536];

static void make_capsule(const char *ver, const char *fields) {
    snprintf(g_cap, sizeof g_cap,
             "{\"capsule_format\":\"0.1\",\"ir_version\":\"%s\",\"system_id\":\"t.sys\",\"source\":\"t.lir.json\","
             "\"schema\":{\"id\":\"t.tab\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"" P3 "\","
             "\"fields\":[%s],\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
             "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
             "\"tests\":[\"t.test.json\"]}",
             ver, fields);
}

/* extra: additional top-level members (for example ",\"row_filter\":{...}"), "" for none. */
static void make_lir(const char *ver, const char *extra, const char *insns) {
    snprintf(g_lir, sizeof g_lir,
             "{\"ir_version\":\"%s\",\"system_id\":\"t.sys\",\"numeric_profile\":\"" P3 "\",\"query\":\"t.tab\","
             "\"instructions\":[%s]%s}",
             ver, insns, extra);
}

static uint32_t verify(SuLirModule *m, SuDiag *d) {
    return su_lir_verify((const uint8_t *)g_cap, strlen(g_cap), (const uint8_t *)g_lir, strlen(g_lir), m, d);
}

static void expect(int line, uint32_t want, const char *ptr) {
    SuLirModule m;
    SuDiag d;
    uint32_t st = verify(&m, &d);
    g_checks++;
    if (st != want || (ptr != NULL && strcmp(d.json_pointer, ptr) != 0)) {
        g_failures++;
        fprintf(stderr, "line %d: got %s at '%s' (%s), want %s at '%s'\n", line, su_status_name(st), d.json_pointer,
                d.message, su_status_name(want), ptr != NULL ? ptr : "*");
    }
    if (st == SU_OK) {
        su_lir_module_free(&m);
    }
}

/* ---------- the test schema ---------- */

/* Inputs a, b, sel, tgt (u32, read) and q (q32, read); outputs o0..o9 (u32, read+write) and oq (q32, rw). */
#define FIELDS                                                                         \
    "{\"id\":1,\"name\":\"a\",\"type\":\"u32\",\"read\":true,\"write\":false},"        \
    "{\"id\":2,\"name\":\"b\",\"type\":\"u32\",\"read\":true,\"write\":false},"        \
    "{\"id\":3,\"name\":\"sel\",\"type\":\"u32\",\"read\":true,\"write\":false},"      \
    "{\"id\":4,\"name\":\"tgt\",\"type\":\"u32\",\"read\":true,\"write\":false},"      \
    "{\"id\":6,\"name\":\"q\",\"type\":\"q32\",\"read\":true,\"write\":false},"        \
    "{\"id\":10,\"name\":\"o0\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":11,\"name\":\"o1\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":12,\"name\":\"o2\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":13,\"name\":\"o3\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":14,\"name\":\"o4\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":15,\"name\":\"o5\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":16,\"name\":\"o6\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":17,\"name\":\"o7\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":18,\"name\":\"o8\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":19,\"name\":\"o9\",\"type\":\"u32\",\"read\":true,\"write\":true},"       \
    "{\"id\":20,\"name\":\"oq\",\"type\":\"q32\",\"read\":true,\"write\":true}"

/* o0..o7: a&b, a|b, a^b, ~a, a<<b, a>>b, a/b, a%b; o8 = a of row tgt; o9 = lowest slot whose b equals this row's a
 * (span 4096); oq = q of row tgt, plus the low word of (i64(a) xor i64(b)) | ~0 & ... checked via o-lanes below. */
#define BODY                                                                                                       \
    "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},{\"op\":\"load_u32\",\"dst\":\"b\",\"field\":\"b\"},"     \
    "{\"op\":\"load_u32\",\"dst\":\"t\",\"field\":\"tgt\"},"                                                       \
    "{\"op\":\"and_u32\",\"dst\":\"r0\",\"a\":\"a\",\"b\":\"b\"},{\"op\":\"or_u32\",\"dst\":\"r1\",\"a\":\"a\",\"b\":\"b\"}," \
    "{\"op\":\"xor_u32\",\"dst\":\"r2\",\"a\":\"a\",\"b\":\"b\"},{\"op\":\"not_u32\",\"dst\":\"r3\",\"a\":\"a\"},"  \
    "{\"op\":\"shl_u32\",\"dst\":\"r4\",\"a\":\"a\",\"n\":\"b\"},{\"op\":\"shr_u32\",\"dst\":\"r5\",\"a\":\"a\",\"n\":\"b\"}," \
    "{\"op\":\"div_u32\",\"dst\":\"r6\",\"a\":\"a\",\"b\":\"b\"},{\"op\":\"rem_u32\",\"dst\":\"r7\",\"a\":\"a\",\"b\":\"b\"}," \
    "{\"op\":\"gather_u32\",\"dst\":\"r8\",\"field\":\"a\",\"slot\":\"t\"},"                                       \
    "{\"op\":\"find_eq_u32\",\"dst\":\"r9\",\"field\":\"b\",\"key\":\"a\",\"span\":4096},"                          \
    "{\"op\":\"gather_q32\",\"dst\":\"gq\",\"field\":\"q\",\"slot\":\"t\"},"                                       \
    "{\"op\":\"q32_raw\",\"dst\":\"gr\",\"a\":\"gq\"},{\"op\":\"const_i64\",\"dst\":\"m\",\"value\":-256},"          \
    "{\"op\":\"and_i64\",\"dst\":\"x1\",\"a\":\"gr\",\"b\":\"m\"},{\"op\":\"not_i64\",\"dst\":\"x2\",\"a\":\"m\"},"   \
    "{\"op\":\"or_i64\",\"dst\":\"x3\",\"a\":\"x1\",\"b\":\"x2\"},{\"op\":\"xor_i64\",\"dst\":\"x4\",\"a\":\"x3\",\"b\":\"m\"}," \
    "{\"op\":\"q32_from_raw\",\"dst\":\"oq\",\"a\":\"x4\"},"                                                       \
    "{\"op\":\"store_u32\",\"field\":\"o0\",\"src\":\"r0\"},{\"op\":\"store_u32\",\"field\":\"o1\",\"src\":\"r1\"}," \
    "{\"op\":\"store_u32\",\"field\":\"o2\",\"src\":\"r2\"},{\"op\":\"store_u32\",\"field\":\"o3\",\"src\":\"r3\"}," \
    "{\"op\":\"store_u32\",\"field\":\"o4\",\"src\":\"r4\"},{\"op\":\"store_u32\",\"field\":\"o5\",\"src\":\"r5\"}," \
    "{\"op\":\"store_u32\",\"field\":\"o6\",\"src\":\"r6\"},{\"op\":\"store_u32\",\"field\":\"o7\",\"src\":\"r7\"}," \
    "{\"op\":\"store_u32\",\"field\":\"o8\",\"src\":\"r8\"},{\"op\":\"store_u32\",\"field\":\"o9\",\"src\":\"r9\"}," \
    "{\"op\":\"store_q32\",\"field\":\"oq\",\"src\":\"oq\"}"

/* ---------- verifier ---------- */

static void test_verifier(void) {
    make_capsule("0.4", FIELDS);
    make_lir("0.4", "", BODY);
    expect(__LINE__, SU_OK, NULL);
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"mask\":1,\"equals\":1}", BODY);
    expect(__LINE__, SU_OK, NULL);
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"equals\":7}", BODY);
    expect(__LINE__, SU_OK, NULL);
    /* 0.4 operations and the row filter are unknown below 0.4. */
    make_capsule("0.3", FIELDS);
    make_lir("0.3", "", BODY);
    expect(__LINE__, SU_E_UNKNOWN_OPCODE, "/instructions/3/op");
    make_lir("0.3", ",\"row_filter\":{\"field\":\"sel\",\"equals\":1}",
             "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"}");
    expect(__LINE__, SU_E_TYPE, "/row_filter");
    make_capsule("0.4", FIELDS);
    make_lir("0.3", "", BODY);
    expect(__LINE__, SU_E_SCHEMA_MISMATCH, "/ir_version");
    /* Row filter shape and semantics. */
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"mask\":1,\"equals\":2}", BODY);
    expect(__LINE__, SU_E_TYPE, "/row_filter/equals");
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\"}", BODY);
    expect(__LINE__, SU_E_TYPE, "/row_filter/equals");
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"equals\":1,\"bits\":1}", BODY);
    expect(__LINE__, SU_E_TYPE, "/row_filter/bits");
    make_lir("0.4", ",\"row_filter\":{\"field\":\"nope\",\"equals\":1}", BODY);
    expect(__LINE__, SU_E_UNDECLARED_READ, "/row_filter/field");
    make_lir("0.4", ",\"row_filter\":{\"field\":\"q\",\"equals\":1}", BODY);
    expect(__LINE__, SU_E_TYPE, "/row_filter/field");
    make_lir("0.4", ",\"row_filter\":5", BODY);
    expect(__LINE__, SU_E_TYPE, "/row_filter");
    make_capsule("0.4", "{\"id\":1,\"name\":\"a\",\"type\":\"u32\",\"read\":true,\"write\":false},"
                        "{\"id\":2,\"name\":\"w\",\"type\":\"u32\",\"read\":false,\"write\":true}");
    make_lir("0.4", ",\"row_filter\":{\"field\":\"a\",\"equals\":1}",
             "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},{\"op\":\"store_u32\",\"field\":\"w\",\"src\":\"a\"}");
    expect(__LINE__, SU_E_UNDECLARED_READ, "/row_filter");
    make_lir("0.4", ",\"row_filter\":{\"field\":\"w\",\"equals\":1}",
             "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},{\"op\":\"store_u32\",\"field\":\"w\",\"src\":\"a\"}");
    expect(__LINE__, SU_E_UNDECLARED_READ, "/row_filter/field");
    make_lir("0.4", "", "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},"
                        "{\"op\":\"gather_u32\",\"dst\":\"g\",\"field\":\"w\",\"slot\":\"a\"},"
                        "{\"op\":\"store_u32\",\"field\":\"w\",\"src\":\"g\"}");
    expect(__LINE__, SU_E_UNDECLARED_READ, "/instructions/1/field");
    /* Operand types and constants of the new operations. */
    make_capsule("0.4", FIELDS);
    make_lir("0.4", "", "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},"
                        "{\"op\":\"gather_i32\",\"dst\":\"g\",\"field\":\"a\",\"slot\":\"a\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/field");
    make_lir("0.4", "", "{\"op\":\"const_i64\",\"dst\":\"a\",\"value\":1},"
                        "{\"op\":\"gather_u32\",\"dst\":\"g\",\"field\":\"a\",\"slot\":\"a\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/slot");
    make_lir("0.4", "", "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},"
                        "{\"op\":\"find_eq_u32\",\"dst\":\"g\",\"field\":\"b\",\"key\":\"a\",\"span\":0}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/span");
    make_lir("0.4", "", "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},"
                        "{\"op\":\"find_eq_u32\",\"dst\":\"g\",\"field\":\"b\",\"key\":\"a\",\"span\":4097}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/span");
    make_lir("0.4", "", "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},"
                        "{\"op\":\"find_eq_u32\",\"dst\":\"g\",\"field\":\"q\",\"key\":\"a\",\"span\":4}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/field");
    make_lir("0.4", "", "{\"op\":\"const_i64\",\"dst\":\"a\",\"value\":1},{\"op\":\"and_u32\",\"dst\":\"g\",\"a\":\"a\",\"b\":\"a\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/a");
    make_lir("0.4", "", "{\"op\":\"load_u32\",\"dst\":\"a\",\"field\":\"a\"},{\"op\":\"xor_i64\",\"dst\":\"g\",\"a\":\"a\",\"b\":\"a\"}");
    expect(__LINE__, SU_E_TYPE, "/instructions/1/a");
}

static int hash_of(char out[65]) {
    SuLirModule m;
    SuDiag d;
    if (verify(&m, &d) != SU_OK) {
        fprintf(stderr, "hash_of: %s %s\n", d.json_pointer, d.message);
        return 0;
    }
    memcpy(out, m.semantic_hash_hex, 65);
    CHECK(m.artifact_format == SU_ARTIFACT_FORMAT_VERSION_0_4);
    su_lir_module_free(&m);
    return 1;
}

static void test_hash(void) {
    char h0[65], h1[65], h2[65], h3[65], h4[65];
    make_capsule("0.4", FIELDS);
    make_lir("0.4", "", BODY);
    CHECK(hash_of(h0));
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"equals\":1}", BODY);
    CHECK(hash_of(h1));
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"mask\":4294967295,\"equals\":1}", BODY);
    CHECK(hash_of(h2)); /* an explicit full mask is the default */
    make_lir("0.4", ",\"row_filter\":{\"field\":\"sel\",\"mask\":3,\"equals\":1}", BODY);
    CHECK(hash_of(h3));
    make_lir("0.4", ",\"row_filter\":{\"field\":\"tgt\",\"mask\":3,\"equals\":1}", BODY);
    CHECK(hash_of(h4));
    CHECK(strcmp(h0, h1) != 0);
    CHECK(strcmp(h1, h2) == 0);
    CHECK(strcmp(h1, h3) != 0);
    CHECK(strcmp(h3, h4) != 0);
    printf("ir04 semantic hash (body, no filter): %s\n", h0);
}

/* ---------- execution ---------- */

#define NROWS 11u
#define NF_MAX 32u

typedef struct World {
    SuWorld *w;
    SuLirModule m;
} World;

/* Row inputs: a, b, sel, tgt, q (raw). */
static const uint32_t IN_A[NROWS] = {0xF0F0F0F0u, 7u, 100u, 0u, 0xFFFFFFFFu, 5u, 1u, 32u, 9u, 9u, 3u};
static const uint32_t IN_B[NROWS] = {0x0FF00FF0u, 0u, 7u, 31u, 32u, 33u, 5u, 1u, 9u, 3u, 100u};
static const uint32_t IN_SEL[NROWS] = {1u, 0u, 3u, 1u, 2u, 5u, 1u, 0u, 1u, 7u, 1u};
static const uint32_t IN_TGT[NROWS] = {10u, 0u, 11u, 4u, 0xFFFFFFFFu, 9u, 3u, 2u, 5u, 1u, 6u};
static const int64_t IN_Q[NROWS] = {SU_Q32_ONE, -SU_Q32_ONE, 3, -3, INT64_MAX, INT64_MIN, 0, INT64_C(0x123456789ABCDEF),
                                    -INT64_C(0x123456789ABCDEF), SU_Q32_HALF, 77};

static int build(World *x, const char *filter) {
    SuDiag d;
    SuWorldFieldDesc fd[NF_MAX];
    SuWorldDesc wd;
    uint32_t ids[NROWS], col[NROWS];
    uint32_t i, r;
    make_capsule("0.4", FIELDS);
    make_lir("0.4", filter, BODY);
    memset(x, 0, sizeof *x);
    if (verify(&x->m, &d) != SU_OK) {
        fprintf(stderr, "build: %s %s\n", d.json_pointer, d.message);
        return 0;
    }
    for (i = 0; i < x->m.field_count; i++) {
        fd[i].id = x->m.fields[i].id;
        fd[i].type = SU_TYPE_U32;
        fd[i].access = x->m.fields[i].access;
        fd[i].name = x->m.fields[i].name;
    }
    for (r = 0; r < NROWS; r++) {
        ids[r] = r + 1u;
    }
    memset(&wd, 0, sizeof wd);
    wd.schema_id = "t.tab";
    wd.schema_version = 1;
    wd.layout_generation = 1;
    wd.row_count = NROWS;
    wd.entity_ids = ids;
    wd.fields = fd;
    wd.field_count = x->m.field_count;
    if (su_world_create(&wd, &x->w) != SU_OK) {
        return 0;
    }
    for (i = 0; i < x->m.field_count; i++) {
        const SuLirField *f = &x->m.fields[i];
        for (r = 0; r < NROWS; r++) {
            uint64_t q = (uint64_t)IN_Q[r];
            switch (f->id) {
            case 1: col[r] = IN_A[r]; break;
            case 2: col[r] = IN_B[r]; break;
            case 3: col[r] = IN_SEL[r]; break;
            case 4: col[r] = IN_TGT[r]; break;
            case 6: col[r] = (uint32_t)(q & 0xFFFFFFFFu); break;
            case 7: col[r] = (uint32_t)(q >> 32); break;
            default: col[r] = 0xA0000000u + f->id * 16u + r; break; /* outputs: recognisable entry values */
            }
        }
        if (su_world_init_committed(x->w, f->id, col, NROWS) != SU_OK) {
            return 0;
        }
    }
    return 1;
}

static void destroy(World *x) {
    su_world_destroy(x->w);
    su_lir_module_free(&x->m);
}

/* tier 0: jobs chunked (chunk_rows 3, two workers); else su_vm_run_system on that tier. */
static uint32_t run(World *x, uint32_t tier) {
    uint32_t st = su_world_begin_tick(x->w);
    if (st != SU_OK) {
        return st;
    }
    if (tier == 0u) {
        SuJobsOptions o;
        SuJobsReport rep;
        memset(&o, 0, sizeof o);
        o.workers = 2u;
        o.chunk_rows = 3u;
        st = su_jobs_run_system_chunked(x->w, &x->m, 1u, &o, &rep);
        su_jobs_report_free(&rep);
    } else {
        SuExecContext *c = NULL;
        st = su_vm_context_create(&x->m, 1u, &c);
        if (st == SU_OK) st = su_vm_context_set_tier(c, tier);
        if (st == SU_OK) st = su_vm_run_system(x->w, c);
        su_vm_context_destroy(c);
    }
    if (st == SU_OK) {
        return su_world_publish(x->w);
    }
    su_world_abort(x->w);
    return st;
}

static uint32_t out_at(const World *x, uint32_t id, uint32_t r) {
    const uint32_t *c = NULL;
    uint64_t n = 0;
    if (su_world_read_committed(x->w, id, &c, &n) != SU_OK || r >= n) {
        return 0xDEADBEEFu;
    }
    return c[r];
}

/* Expected value of output field id at row r (filtered: row r did not pass the filter). */
static uint32_t want_at(uint32_t id, uint32_t r, int filtered) {
    uint32_t a = IN_A[r], b = IN_B[r], t = IN_TGT[r], s;
    int64_t g, m = -256, x4;
    if (filtered) {
        return 0xA0000000u + id * 16u + r;
    }
    switch (id) {
    case 10: return a & b;
    case 11: return a | b;
    case 12: return a ^ b;
    case 13: return ~a;
    case 14: return b >= 32u ? 0u : a << b;
    case 15: return b >= 32u ? 0u : a >> b;
    case 16: return b == 0u ? 0u : a / b;
    case 17: return b == 0u ? a : a % b;
    case 18: return t < NROWS ? IN_A[t] : 0u;
    case 19:
        for (s = 0; s < NROWS; s++) {
            if (IN_B[s] == a) {
                return s;
            }
        }
        return 0xFFFFFFFFu;
    default:
        break;
    }
    g = t < NROWS ? IN_Q[t] : 0;
    x4 = su_i64_from_bits((((uint64_t)g & (uint64_t)m) | ~(uint64_t)m) ^ (uint64_t)m);
    return id == 20u ? (uint32_t)((uint64_t)x4 & 0xFFFFFFFFu) : (uint32_t)((uint64_t)x4 >> 32);
}

static void check_outputs(const World *x, int line, int has, uint32_t mask, uint32_t eq) {
    uint32_t r, id;
    for (r = 0; r < NROWS; r++) {
        int filtered = has && (IN_SEL[r] & mask) != eq;
        for (id = 10; id <= 21; id++) {
            uint32_t got = out_at(x, id, r), want = want_at(id, r, filtered);
            g_checks++;
            if (got != want) {
                g_failures++;
                fprintf(stderr, "line %d: row %u field %u: got 0x%08X want 0x%08X\n", line, (unsigned)r,
                        (unsigned)id, (unsigned)got, (unsigned)want);
            }
        }
    }
}

static void test_exec(void) {
    static const char *filters[3] = {"", ",\"row_filter\":{\"field\":\"sel\",\"mask\":1,\"equals\":1}",
                                     ",\"row_filter\":{\"field\":\"sel\",\"equals\":3}"};
    static const uint32_t fmask[3] = {0u, 1u, 0xFFFFFFFFu};
    static const uint32_t feq[3] = {0u, 1u, 3u};
    uint32_t f, tier;
    for (f = 0; f < 3u; f++) {
        for (tier = 0; tier <= 2u; tier++) {
            World x;
            if (!build(&x, filters[f])) {
                CHECK(0);
                continue;
            }
            CHECK(run(&x, tier) == SU_OK);
            check_outputs(&x, (int)(1000u * f + tier), f != 0u, fmask[f], feq[f]);
            destroy(&x);
        }
    }
}

/* Wrapper checks of the whole-table view (direct su_vm_invoke_v1). */
static void test_table_view(void) {
    World x;
    SuInvocation wi, inv, tab;
    SuOutputs wo, outs;
    SuColumnBinding in[NF_MAX], out[NF_MAX];
    SuExecContext *c = NULL;
    uint32_t i, j, nin = 0, nout = 0;
    if (!build(&x, "")) {
        CHECK(0);
        return;
    }
    CHECK(x.m.uses_gather == 1u);
    CHECK(su_world_begin_tick(x.w) == SU_OK);
    CHECK(su_world_build_invocation(x.w, &wi) == SU_OK);
    CHECK(su_world_build_outputs(x.w, &wo) == SU_OK);
    for (i = 0; i < x.m.field_count; i++) {
        for (j = 0; j < wi.binding_count && (x.m.fields[i].access & SU_ACCESS_READ) != 0u; j++) {
            if (wi.bindings[j].field_id == x.m.fields[i].id) {
                in[nin++] = wi.bindings[j];
            }
        }
        for (j = 0; j < wo.binding_count && (x.m.fields[i].access & SU_ACCESS_WRITE) != 0u; j++) {
            if (wo.bindings[j].field_id == x.m.fields[i].id) {
                out[nout++] = wo.bindings[j];
            }
        }
    }
    inv = wi;
    inv.bindings = in;
    inv.binding_count = nin;
    outs = wo;
    outs.bindings = out;
    outs.binding_count = nout;
    CHECK(su_vm_context_create(&x.m, 1u, &c) == SU_OK);
    /* A chunk at slot 4 without a table view: refused (it cannot know the table). */
    CHECK(su_vm_context_set_entity_view(c, NULL, NULL, 4u, 0u, NULL) == SU_OK);
    CHECK(su_vm_invoke_v1(c, &inv, &outs) == SU_E_INVALID_ARGUMENT);
    /* With the view: accepted. */
    tab = inv;
    CHECK(su_vm_context_set_table_view(c, &tab) == SU_OK);
    CHECK(su_vm_invoke_v1(c, &inv, &outs) == SU_OK);
    /* Wrong generation, wrong binding count, overlap with staging. */
    tab.layout_generation = 2u;
    CHECK(su_vm_context_set_table_view(c, &tab) == SU_OK);
    CHECK(su_vm_invoke_v1(c, &inv, &outs) == SU_E_SCHEMA_MISMATCH);
    tab = inv;
    tab.binding_count = nin - 1u;
    CHECK(su_vm_context_set_table_view(c, &tab) == SU_OK);
    CHECK(su_vm_invoke_v1(c, &inv, &outs) == SU_E_INVALID_ARGUMENT);
    {
        SuColumnBinding alias[NF_MAX];
        memcpy(alias, in, sizeof alias);
        alias[0].data = out[0].data;
        tab = inv;
        tab.bindings = alias;
        CHECK(su_vm_context_set_table_view(c, &tab) == SU_OK);
        CHECK(su_vm_invoke_v1(c, &inv, &outs) == SU_E_INVALID_ARGUMENT);
    }
    CHECK(su_vm_context_set_table_view(c, NULL) == SU_OK);
    CHECK(su_vm_context_set_table_view(NULL, NULL) == SU_E_INVALID_ARGUMENT);
    su_vm_context_destroy(c);
    su_world_abort(x.w);
    destroy(&x);
}

/* The op table: 0.4 rows are known only from 0.4 on, codes unique. */
static void test_table(void) {
    size_t i, n04 = 0;
    for (i = 0; i < su_op03_count(); i++) {
        const SuOp03 *o = su_op03_at(i);
        if (su_op_min_minor(o) == 4u) {
            n04++;
            CHECK(o->code >= SU_OP_AND_U32);
        }
    }
    CHECK(n04 == 18u);
    CHECK(su_op_min_minor(su_op03_by_name("add_i32", 7)) == 3u);
    CHECK(su_op_min_minor(su_op03_by_name("xor_u32", 7)) == 4u);
}

int main(void) {
    test_table();
    test_verifier();
    test_hash();
    test_exec();
    test_table_view();
    printf("test_ir04: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
