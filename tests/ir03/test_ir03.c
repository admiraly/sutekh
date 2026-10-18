/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.3 (numeric profile fixed_q32_v1): verifier acceptance and rejection with exact codes and JSON pointers,
 * lane layout of the verified field table, semantic-hash invariants, VM known answers on a real world, and
 * VM defensive checks on hand-built modules that bypass the verifier.
 *
 * Run from the repository root (the fixture under tests/fixtures/ir03 is read by relative path). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_lir.h"
#include "ir/su_num.h"
#include "ir/su_ops03.h"
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

#define ONE SU_Q32_ONE
#define HALF SU_Q32_HALF

/* ---------- document builders ---------- */

static char g_cap[16384];
static char g_lir[65536];

/* Capsule with the given fields JSON array body (without brackets). */
static void make_capsule(const char *ver, const char *profile, const char *fields) {
    snprintf(g_cap, sizeof g_cap,
             "{\"capsule_format\":\"0.1\",\"ir_version\":\"%s\",\"system_id\":\"t.sys\",\"source\":\"t.lir.json\","
             "\"schema\":{\"id\":\"t.ship\",\"version\":1,\"layout_generation\":1},\"numeric_profile\":\"%s\","
             "\"fields\":[%s],\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
             "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
             "\"tests\":[\"t.test.json\"]}",
             ver, profile, fields);
}

static void make_lir(const char *ver, const char *profile, const char *insns) {
    snprintf(g_lir, sizeof g_lir,
             "{\"ir_version\":\"%s\",\"system_id\":\"t.sys\",\"numeric_profile\":\"%s\",\"query\":\"t.ship\","
             "\"instructions\":[%s]}",
             ver, profile, insns);
}

#define P3 "fixed_q32_v1"

/* Standard field set: pos (q32 rw, id 10), vel (q32 r, id 20), dt (q32 r, id 30), n (i32 rw, id 40),
 * big (i64 rw, id 50), cnt (u32 r, id 60). */
#define FIELDS_STD                                                                    \
    "{\"id\":10,\"name\":\"pos\",\"type\":\"q32\",\"read\":true,\"write\":true},"    \
    "{\"id\":20,\"name\":\"vel\",\"type\":\"q32\",\"read\":true,\"write\":false},"   \
    "{\"id\":30,\"name\":\"dt\",\"type\":\"q32\",\"read\":true,\"write\":false},"    \
    "{\"id\":40,\"name\":\"n\",\"type\":\"i32\",\"read\":true,\"write\":true},"      \
    "{\"id\":50,\"name\":\"big\",\"type\":\"i64\",\"read\":true,\"write\":true},"    \
    "{\"id\":60,\"name\":\"cnt\",\"type\":\"u32\",\"read\":true,\"write\":false}"

/* Stores every writable field of FIELDS_STD (append after the instructions under test). */
#define STORES_STD                                                                    \
    "{\"op\":\"load_q32\",\"dst\":\"zz_p\",\"field\":\"pos\"},"                       \
    "{\"op\":\"store_q32\",\"field\":\"pos\",\"src\":\"zz_p\"},"                      \
    "{\"op\":\"load_i32\",\"dst\":\"zz_n\",\"field\":\"n\"},"                         \
    "{\"op\":\"store_i32\",\"field\":\"n\",\"src\":\"zz_n\"},"                        \
    "{\"op\":\"load_i64\",\"dst\":\"zz_b\",\"field\":\"big\"},"                       \
    "{\"op\":\"store_i64\",\"field\":\"big\",\"src\":\"zz_b\"}"

static uint32_t verify(SuLirModule *m, SuDiag *d) {
    return su_lir_verify((const uint8_t *)g_cap, strlen(g_cap), (const uint8_t *)g_lir, strlen(g_lir), m, d);
}

/* Verifies the current documents and expects code at pointer (pointer NULL: not checked). */
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

/* One body (instructions JSON) on FIELDS_STD, 0.3. */
static void body(const char *insns) {
    make_capsule("0.3", P3, FIELDS_STD);
    make_lir("0.3", P3, insns);
}

/* ---------- verifier: acceptance and lane layout ---------- */

static void test_accept_and_lanes(void) {
    SuLirModule m;
    SuDiag d;
    uint32_t st;
    body(STORES_STD);
    st = verify(&m, &d);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "accept: %s %s %s\n", su_status_name(st), d.json_pointer, d.message);
        return;
    }
    CHECK(m.ir_version_minor == 3u);
    CHECK(m.artifact_format == SU_ARTIFACT_FORMAT_VERSION_0_3);
    CHECK(strcmp(m.numeric_profile, P3) == 0);
    CHECK(m.field_count == 10u); /* lanes: pos 2, vel 2, dt 2, n 1, big 2, cnt 1 */
    su_lir_module_free(&m);
}

static void test_lane_table(void) {
    SuLirModule m;
    SuDiag d;
    static const struct {
        uint32_t id;
        const char *name;
        uint32_t vt;
        uint32_t lane;
        uint32_t access;
    } want[] = {
        {10, "pos.lo", SU_TYPE_Q32, 0, 3}, {11, "pos.hi", SU_TYPE_Q32, 1, 3}, {20, "vel.lo", SU_TYPE_Q32, 0, 1},
        {21, "vel.hi", SU_TYPE_Q32, 1, 1}, {30, "dt.lo", SU_TYPE_Q32, 0, 1},  {31, "dt.hi", SU_TYPE_Q32, 1, 1},
        {40, "n", SU_TYPE_I32, 0, 3},      {50, "big.lo", SU_TYPE_I64, 0, 3}, {51, "big.hi", SU_TYPE_I64, 1, 3},
        {60, "cnt", SU_TYPE_U32, 0, 1},
    };
    uint32_t i;
    body(STORES_STD);
    if (verify(&m, &d) != SU_OK) {
        CHECK(0);
        return;
    }
    CHECK(m.field_count == sizeof want / sizeof want[0]);
    for (i = 0; i < m.field_count && i < sizeof want / sizeof want[0]; i++) {
        CHECK(m.fields[i].id == want[i].id);
        CHECK(strcmp(m.fields[i].name, want[i].name) == 0);
        CHECK(m.fields[i].type == SU_TYPE_U32);
        CHECK(m.fields[i].value_type == want[i].vt);
        CHECK(m.fields[i].lane == want[i].lane);
        CHECK(m.fields[i].access == want[i].access);
    }
    CHECK(m.uses_entity_ops == 0u);
    su_lir_module_free(&m);
}

/* ---------- verifier: rejections ---------- */

static void test_reject(void) {
    /* Profile and version pairing. */
    make_capsule("0.3", "u32_mod_v1", FIELDS_STD);
    make_lir("0.3", P3, STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/numeric_profile");
    make_capsule("0.3", P3, FIELDS_STD);
    make_lir("0.3", "u32_mod_v1", STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/numeric_profile");
    make_capsule("0.2", P3, "{\"id\":1,\"name\":\"a\",\"type\":\"u32\",\"read\":true,\"write\":false}");
    make_lir("0.2", P3, "");
    expect(__LINE__, SU_E_TYPE, "/numeric_profile");
    make_capsule("0.4", P3, FIELDS_STD);
    make_lir("0.3", P3, STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/ir_version");
    make_capsule("0.3", P3, FIELDS_STD);
    make_lir("0.2", "u32_mod_v1", STORES_STD);
    expect(__LINE__, SU_E_SCHEMA_MISMATCH, "/numeric_profile"); /* both documents valid alone; they disagree */
    /* 0.3 field types outside 0.3, unknown types. */
    make_capsule("0.2", "u32_mod_v1", "{\"id\":1,\"name\":\"a\",\"type\":\"q32\",\"read\":true,\"write\":false}");
    make_lir("0.2", "u32_mod_v1", "");
    expect(__LINE__, SU_E_TYPE, "/fields/0");
    make_capsule("0.3", P3, "{\"id\":1,\"name\":\"a\",\"type\":\"f32\",\"read\":true,\"write\":false}");
    make_lir("0.3", P3, "");
    expect(__LINE__, SU_E_TYPE, "/fields/0");
    /* 0.3 ops are unknown in 0.2 modules. */
    make_capsule("0.2", "u32_mod_v1", "{\"id\":1,\"name\":\"a\",\"type\":\"u32\",\"read\":true,\"write\":false}");
    make_lir("0.2", "u32_mod_v1", "{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":1}");
    expect(__LINE__, SU_E_UNKNOWN_OPCODE, "/instructions/0/op");
    /* Lane collisions and limits (stage 5). */
    make_capsule("0.3", P3,
                 "{\"id\":10,\"name\":\"a\",\"type\":\"q32\",\"read\":true,\"write\":false},"
                 "{\"id\":11,\"name\":\"b\",\"type\":\"u32\",\"read\":true,\"write\":false}");
    make_lir("0.3", P3, "");
    expect(__LINE__, SU_E_SCHEMA_MISMATCH, "/fields/0/id");
    make_capsule("0.3", P3,
                 "{\"id\":10,\"name\":\"a\",\"type\":\"q32\",\"read\":true,\"write\":false},"
                 "{\"id\":11,\"name\":\"b\",\"type\":\"i64\",\"read\":true,\"write\":false}");
    make_lir("0.3", P3, "");
    expect(__LINE__, SU_E_SCHEMA_MISMATCH, "/fields/0/id");
    make_capsule("0.3", P3, "{\"id\":4294967295,\"name\":\"a\",\"type\":\"q32\",\"read\":true,\"write\":false}");
    make_lir("0.3", P3, "");
    expect(__LINE__, SU_E_SCHEMA_MISMATCH, "/fields/0/id");
    {
        /* 129 q32 fields = 258 lanes > 256. */
        static char fields[16384];
        size_t n = 0;
        int i;
        for (i = 0; i < 129; i++) {
            n += (size_t)snprintf(fields + n, sizeof fields - n, "%s{\"id\":%d,\"name\":\"f%d\",\"type\":\"q32\","
                                  "\"read\":true,\"write\":false}", i ? "," : "", 2 * i + 1, i);
        }
        make_capsule("0.3", P3, fields);
        make_lir("0.3", P3, "");
        expect(__LINE__, SU_E_RESOURCE_LIMIT, "/fields");
    }
    /* Field value types. */
    body("{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"pos\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/field");
    body("{\"op\":\"load_q32\",\"dst\":\"x\",\"field\":\"n\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/field");
    body("{\"op\":\"load_i64\",\"dst\":\"x\",\"field\":\"pos\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/field");
    body("{\"op\":\"load_i32\",\"dst\":\"x\",\"field\":\"cnt\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/field");
    body("{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"cnt\"}," STORES_STD);
    expect(__LINE__, SU_OK, NULL);
    /* Lane names are not field names. */
    body("{\"op\":\"load_u32\",\"dst\":\"x\",\"field\":\"pos.lo\"}," STORES_STD);
    expect(__LINE__, SU_E_UNDECLARED_READ, "/instructions/0/field");
    /* Permissions. */
    body("{\"op\":\"load_q32\",\"dst\":\"x\",\"field\":\"nope\"}," STORES_STD);
    expect(__LINE__, SU_E_UNDECLARED_READ, "/instructions/0/field");
    body("{\"op\":\"load_q32\",\"dst\":\"x\",\"field\":\"vel\"},{\"op\":\"store_q32\",\"field\":\"vel\",\"src\":\"x\"},"
         STORES_STD);
    expect(__LINE__, SU_E_UNDECLARED_WRITE, "/instructions/1/field");
    body("{\"op\":\"load_q32\",\"dst\":\"x\",\"field\":\"pos\"},{\"op\":\"store_q32\",\"field\":\"pos\",\"src\":\"x\"},"
         STORES_STD);
    expect(__LINE__, SU_E_UNDECLARED_WRITE, "/instructions/3/field"); /* stored twice */
    /* Missing store of a 64-bit field (both lanes unstored). */
    body("{\"op\":\"load_i32\",\"dst\":\"a\",\"field\":\"n\"},{\"op\":\"store_i32\",\"field\":\"n\",\"src\":\"a\"},"
         "{\"op\":\"load_i64\",\"dst\":\"b\",\"field\":\"big\"},{\"op\":\"store_i64\",\"field\":\"big\",\"src\":\"b\"}");
    expect(__LINE__, SU_E_MISSING_STORE, "/instructions");
    /* store_v3 needs three distinct writable q32 fields. */
    make_capsule("0.3", P3,
                 "{\"id\":1,\"name\":\"x\",\"type\":\"q32\",\"read\":true,\"write\":true},"
                 "{\"id\":3,\"name\":\"y\",\"type\":\"q32\",\"read\":true,\"write\":true},"
                 "{\"id\":5,\"name\":\"z\",\"type\":\"q32\",\"read\":true,\"write\":true}");
    make_lir("0.3", P3, "{\"op\":\"load_v3\",\"dst\":\"v\",\"x\":\"x\",\"y\":\"y\",\"z\":\"z\"},"
                        "{\"op\":\"store_v3\",\"x\":\"x\",\"y\":\"x\",\"z\":\"z\",\"src\":\"v\"}");
    expect(__LINE__, SU_E_UNDECLARED_WRITE, "/instructions/1/y");
    make_lir("0.3", P3, "{\"op\":\"load_v3\",\"dst\":\"v\",\"x\":\"x\",\"y\":\"y\",\"z\":\"z\"},"
                        "{\"op\":\"store_v3\",\"x\":\"x\",\"y\":\"y\",\"z\":\"z\",\"src\":\"v\"}");
    expect(__LINE__, SU_OK, NULL);
    /* Registers: undefined, duplicate, wrong type. */
    body("{\"op\":\"add_q32\",\"dst\":\"x\",\"a\":\"p\",\"b\":\"p\"}," STORES_STD);
    expect(__LINE__, SU_E_UNDEFINED_REGISTER, "/instructions/0/a");
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":1},{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":2}," STORES_STD);
    expect(__LINE__, SU_E_DUPLICATE_REGISTER, "/instructions/1/dst");
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":1},{\"op\":\"add_q32\",\"dst\":\"y\",\"a\":\"x\",\"b\":\"x\"},"
         STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/1/a");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":1},{\"op\":\"shl_q32\",\"dst\":\"y\",\"a\":\"x\",\"n\":\"x\"},"
         STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/1/n");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":1},{\"op\":\"store_i32\",\"field\":\"n\",\"src\":\"x\"},"
         STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/1/src");
    body("{\"op\":\"const_i32\",\"dst\":\"c\",\"value\":1},{\"op\":\"select_q32\",\"dst\":\"y\",\"cond\":\"c\","
         "\"on_true\":\"c\",\"on_false\":\"c\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/1/cond");
    /* Constants. */
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":2147483648}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/value");
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":-2147483648}," STORES_STD);
    expect(__LINE__, SU_OK, NULL);
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":-2147483649}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/value");
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":1.5}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/value");
    body("{\"op\":\"const_i64\",\"dst\":\"x\",\"value\":-9223372036854775808}," STORES_STD);
    expect(__LINE__, SU_OK, NULL);
    body("{\"op\":\"const_i64\",\"dst\":\"x\",\"value\":9223372036854775808}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/value");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":0}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/den");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":2147483648,\"den\":1}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/num");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":-2147483648,\"den\":1}," STORES_STD);
    expect(__LINE__, SU_OK, NULL);
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":120,\"extra\":1}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/extra");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/0/den");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":1},{\"op\":\"make_v3\",\"dst\":\"v\",\"x\":\"x\","
         "\"y\":\"x\",\"z\":\"x\"},{\"op\":\"v3_get\",\"dst\":\"g\",\"v\":\"v\",\"axis\":3}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/2/axis");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":1},{\"op\":\"make_v3\",\"dst\":\"v\",\"x\":\"x\","
         "\"y\":\"x\",\"z\":\"x\"},{\"op\":\"make_quat\",\"dst\":\"q\",\"w\":\"x\",\"v\":\"v\"},"
         "{\"op\":\"quat_get\",\"dst\":\"g\",\"q\":\"q\",\"axis\":3}," STORES_STD);
    expect(__LINE__, SU_OK, NULL);
    /* Vector operand types. */
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":1},{\"op\":\"v3_len\",\"dst\":\"l\",\"a\":\"x\"},"
         STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/1/a");
    body("{\"op\":\"const_q32\",\"dst\":\"x\",\"num\":1,\"den\":1},{\"op\":\"make_v3\",\"dst\":\"v\",\"x\":\"x\","
         "\"y\":\"x\",\"z\":\"x\"},{\"op\":\"quat_mul\",\"dst\":\"q\",\"a\":\"v\",\"b\":\"v\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/2/a");
    /* bool ops. */
    body("{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":1},{\"op\":\"not_bool\",\"dst\":\"y\",\"a\":\"x\"}," STORES_STD);
    expect(__LINE__, SU_E_TYPE, "/instructions/1/a");
    /* Unknown 0.3-looking names stay unknown. */
    body("{\"op\":\"sin_q32\",\"dst\":\"x\",\"a\":\"x\"}," STORES_STD);
    expect(__LINE__, SU_E_UNKNOWN_OPCODE, "/instructions/0/op");
}

/* ---------- semantic hash ---------- */

static int hash_of(char out[65]) {
    SuLirModule m;
    SuDiag d;
    if (verify(&m, &d) != SU_OK) {
        fprintf(stderr, "hash_of: %s %s\n", d.json_pointer, d.message);
        return 0;
    }
    memcpy(out, m.semantic_hash_hex, 65);
    su_lir_module_free(&m);
    return 1;
}

static void test_hash(void) {
    char h1[65], h2[65], h3[65], h4[65];
    body("{\"op\":\"const_q32\",\"dst\":\"half\",\"num\":1,\"den\":2}," STORES_STD);
    CHECK(hash_of(h1));
    body("{\"op\":\"const_q32\",\"dst\":\"other\",\"den\":4,\"num\":2}," STORES_STD); /* equal value, renamed, key order */
    CHECK(hash_of(h2));
    CHECK(strcmp(h1, h2) == 0);
    body("{\"op\":\"const_q32\",\"dst\":\"half\",\"num\":1,\"den\":3}," STORES_STD);
    CHECK(hash_of(h3));
    CHECK(strcmp(h1, h3) != 0);
    /* Same instructions, different field value type: the hash differs (format 3 hashes value types). */
    make_capsule("0.3", P3, "{\"id\":1,\"name\":\"a\",\"type\":\"u32\",\"read\":true,\"write\":false}");
    make_lir("0.3", P3, "{\"op\":\"const_i32\",\"dst\":\"x\",\"value\":1}");
    CHECK(hash_of(h3));
    make_capsule("0.3", P3, "{\"id\":1,\"name\":\"a\",\"type\":\"i32\",\"read\":true,\"write\":false}");
    CHECK(hash_of(h4));
    CHECK(strcmp(h3, h4) != 0);
    /* Field names are not hashed; IDs are. */
    make_capsule("0.3", P3, "{\"id\":1,\"name\":\"renamed\",\"type\":\"i32\",\"read\":true,\"write\":false}");
    CHECK(hash_of(h3));
    CHECK(strcmp(h3, h4) == 0);
    /* Pinned golden of the STORES_STD module (first computed 2026-10-18; changes only with a format bump). */
    body(STORES_STD);
    CHECK(hash_of(h1));
    printf("ir03 golden semantic hash: %s\n", h1);
    CHECK(strcmp(h1, "72623af9358b93a9e474c8770d06d8c02b2a2c4273c5b6e00ea75ef1aaa3f21f") == 0);
}

/* ---------- VM on a real world ---------- */

typedef struct Row {
    int64_t pos, vel, dt, big;
    int32_t n;
    uint32_t cnt;
} Row;

/* Builds a world from the module's table, runs the module once over rows, and returns the world. */
static SuWorld *run_world(const SuLirModule *m, const Row *rows, uint32_t nrows, uint32_t *st_out) {
    SuWorldFieldDesc fd[64];
    SuWorldDesc wd;
    uint32_t ids[64];
    SuWorld *w = NULL;
    SuExecContext *ctx = NULL;
    uint32_t i, r, st;
    static uint32_t col[64][256];
    memset(&wd, 0, sizeof wd);
    for (i = 0; i < m->field_count; i++) {
        fd[i].id = m->fields[i].id;
        fd[i].type = SU_TYPE_U32;
        fd[i].access = m->fields[i].access;
        fd[i].name = m->fields[i].name;
    }
    for (r = 0; r < nrows; r++) {
        ids[r] = r + 1u;
    }
    wd.schema_id = "t.ship";
    wd.schema_version = 1;
    wd.layout_generation = 1;
    wd.row_count = nrows;
    wd.entity_ids = ids;
    wd.fields = fd;
    wd.field_count = m->field_count;
    st = su_world_create(&wd, &w);
    if (st != SU_OK) {
        *st_out = st;
        return NULL;
    }
    for (i = 0; i < m->field_count; i++) {
        const char *nm = m->fields[i].name;
        for (r = 0; r < nrows; r++) {
            uint64_t v = 0;
            if (strncmp(nm, "pos.", 4) == 0) v = (uint64_t)rows[r].pos;
            if (strncmp(nm, "vel.", 4) == 0) v = (uint64_t)rows[r].vel;
            if (strncmp(nm, "dt.", 3) == 0) v = (uint64_t)rows[r].dt;
            if (strncmp(nm, "big.", 4) == 0) v = (uint64_t)rows[r].big;
            if (strcmp(nm, "n") == 0) v = (uint64_t)(uint32_t)rows[r].n;
            if (strcmp(nm, "cnt") == 0) v = rows[r].cnt;
            col[i][r] = m->fields[i].lane == 1u ? (uint32_t)(v >> 32) : (uint32_t)(v & 0xFFFFFFFFu);
        }
        su_world_init_committed(w, m->fields[i].id, col[i], nrows);
    }
    st = su_vm_context_create(m, 1, &ctx);
    if (st == SU_OK) st = su_world_begin_tick(w);
    if (st == SU_OK) st = su_vm_run_system(w, ctx);
    if (st == SU_OK) st = su_world_publish(w);
    else su_world_abort(w);
    su_vm_context_destroy(ctx);
    *st_out = st;
    return w;
}

static int64_t read64(const SuWorld *w, const char *base, uint32_t row) {
    char nlo[64], nhi[64];
    const uint32_t *lo, *hi;
    uint64_t n;
    snprintf(nlo, sizeof nlo, "%s.lo", base);
    snprintf(nhi, sizeof nhi, "%s.hi", base);
    if (su_world_read_committed_by_name(w, nlo, &lo, &n) != SU_OK ||
        su_world_read_committed_by_name(w, nhi, &hi, &n) != SU_OK) {
        return INT64_MIN + 12345;
    }
    return su_i64_from_bits(((uint64_t)hi[row] << 32) | lo[row]);
}

static uint32_t read32(const SuWorld *w, const char *name, uint32_t row) {
    const uint32_t *c;
    uint64_t n;
    return su_world_read_committed_by_name(w, name, &c, &n) == SU_OK ? c[row] : 0xDEADBEEFu;
}

/* Kinematics fixture: pos += vel * dt (q32), n = n * 3 - 7 (i32 wrap), big = big + raw(pos') (i64 wrap). */
#define KIN_BODY                                                                            \
    "{\"op\":\"load_q32\",\"dst\":\"p\",\"field\":\"pos\"},"                                \
    "{\"op\":\"load_q32\",\"dst\":\"v\",\"field\":\"vel\"},"                                \
    "{\"op\":\"load_q32\",\"dst\":\"dt\",\"field\":\"dt\"},"                                \
    "{\"op\":\"mul_q32\",\"dst\":\"dp\",\"a\":\"v\",\"b\":\"dt\"},"                         \
    "{\"op\":\"add_q32\",\"dst\":\"p2\",\"a\":\"p\",\"b\":\"dp\"},"                         \
    "{\"op\":\"store_q32\",\"field\":\"pos\",\"src\":\"p2\"},"                              \
    "{\"op\":\"load_i32\",\"dst\":\"n\",\"field\":\"n\"},"                                  \
    "{\"op\":\"const_i32\",\"dst\":\"three\",\"value\":3},"                                 \
    "{\"op\":\"const_i32\",\"dst\":\"seven\",\"value\":-7},"                                \
    "{\"op\":\"mul_i32\",\"dst\":\"n3\",\"a\":\"n\",\"b\":\"three\"},"                      \
    "{\"op\":\"add_i32\",\"dst\":\"n4\",\"a\":\"n3\",\"b\":\"seven\"},"                     \
    "{\"op\":\"store_i32\",\"field\":\"n\",\"src\":\"n4\"},"                                \
    "{\"op\":\"load_i64\",\"dst\":\"b\",\"field\":\"big\"},"                                \
    "{\"op\":\"q32_raw\",\"dst\":\"praw\",\"a\":\"p2\"},"                                   \
    "{\"op\":\"add_i64\",\"dst\":\"b2\",\"a\":\"b\",\"b\":\"praw\"},"                       \
    "{\"op\":\"store_i64\",\"field\":\"big\",\"src\":\"b2\"}"

static void test_vm_known(void) {
    SuLirModule m;
    SuDiag d;
    uint32_t st = 0;
    SuWorld *w;
    /* Hand-derived: 1.5 + 2.0 * 0.5 = 2.5; -100000.25 + (-1000) * (1/120 RNE) ...; MAX + 1 saturates. */
    Row rows[4] = {
        {3 * HALF, 2 * ONE, HALF, 10, 5, 0},
        {INT64_MAX - 1, ONE, ONE, INT64_MAX, INT32_MAX, 0},
        {-100000 * ONE - ONE / 4, -1000 * ONE, INT64_C(35791394), -1, -3, 0},
        {0, 0, 0, 0, 0, 0},
    };
    body(KIN_BODY);
    st = verify(&m, &d);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "kin verify: %s %s %s\n", su_status_name(st), d.json_pointer, d.message);
        return;
    }
    w = run_world(&m, rows, 4, &st);
    CHECK(st == SU_OK);
    if (w != NULL && st == SU_OK) {
        CHECK(read64(w, "pos", 0) == 5 * HALF);
        CHECK(read32(w, "n", 0) == 8u);
        CHECK(read64(w, "big", 0) == 10 + 5 * HALF);
        CHECK(read64(w, "pos", 1) == INT64_MAX);         /* saturated */
        CHECK(read32(w, "n", 1) == (uint32_t)(INT32_MAX * 3u - 7u)); /* wrapped */
        CHECK(read64(w, "big", 1) == INT64_MIN + INT64_MAX - 1); /* MAX + MAX wraps to -2 */
        /* -1000 m/s * (35791394 / 2^32 s) = -35791394000 raw exactly (-8.333 m). */
        CHECK(read64(w, "pos", 2) == -100000 * ONE - ONE / 4 - INT64_C(35791394000));
        CHECK(su_i32_from_bits(read32(w, "n", 2)) == -16);
        CHECK(read64(w, "pos", 3) == 0 && read32(w, "n", 3) == (uint32_t)-7 && read64(w, "big", 3) == 0);
    }
    su_world_destroy(w);
    su_lir_module_free(&m);
}

/* Orientation step on the VM equals the library composition (VM plumbing of v3/quat registers). */
static void test_vm_quat(void) {
    SuLirModule m;
    SuDiag d;
    uint32_t st = 0;
    SuWorld *w;
    Row rows[2] = {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}};
    /* q = normalize(1, 0.1*cnt, 0.2, -0.3); w = (vel, 2*vel, -vel); dt = dt; out pos = quat_get(qi, axis 1),
     * big = raw(v3_len(rotate(qi, w))). */
    body("{\"op\":\"load_q32\",\"dst\":\"v\",\"field\":\"vel\"},"
         "{\"op\":\"load_q32\",\"dst\":\"dt\",\"field\":\"dt\"},"
         "{\"op\":\"const_q32\",\"dst\":\"one\",\"num\":1,\"den\":1},"
         "{\"op\":\"const_q32\",\"dst\":\"a\",\"num\":1,\"den\":10},"
         "{\"op\":\"const_q32\",\"dst\":\"b\",\"num\":1,\"den\":5},"
         "{\"op\":\"const_q32\",\"dst\":\"c\",\"num\":-3,\"den\":10},"
         "{\"op\":\"make_v3\",\"dst\":\"u\",\"x\":\"a\",\"y\":\"b\",\"z\":\"c\"},"
         "{\"op\":\"make_quat\",\"dst\":\"q0\",\"w\":\"one\",\"v\":\"u\"},"
         "{\"op\":\"quat_normalize\",\"dst\":\"q\",\"a\":\"q0\"},"
         "{\"op\":\"add_q32\",\"dst\":\"v2\",\"a\":\"v\",\"b\":\"v\"},"
         "{\"op\":\"neg_q32\",\"dst\":\"vn\",\"a\":\"v\"},"
         "{\"op\":\"make_v3\",\"dst\":\"om\",\"x\":\"v\",\"y\":\"v2\",\"z\":\"vn\"},"
         "{\"op\":\"quat_integrate\",\"dst\":\"qi\",\"q\":\"q\",\"w\":\"om\",\"dt\":\"dt\"},"
         "{\"op\":\"quat_get\",\"dst\":\"gx\",\"q\":\"qi\",\"axis\":1},"
         "{\"op\":\"store_q32\",\"field\":\"pos\",\"src\":\"gx\"},"
         "{\"op\":\"quat_rotate\",\"dst\":\"r\",\"q\":\"qi\",\"v\":\"om\"},"
         "{\"op\":\"v3_len\",\"dst\":\"l\",\"a\":\"r\"},"
         "{\"op\":\"q32_raw\",\"dst\":\"lr\",\"a\":\"l\"},"
         "{\"op\":\"store_i64\",\"field\":\"big\",\"src\":\"lr\"},"
         "{\"op\":\"load_i32\",\"dst\":\"n\",\"field\":\"n\"},"
         "{\"op\":\"store_i32\",\"field\":\"n\",\"src\":\"n\"}");
    rows[0].vel = 3 * ONE;
    rows[0].dt = INT64_C(35791394);
    rows[1].vel = -ONE / 7;
    rows[1].dt = ONE / 60;
    st = verify(&m, &d);
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "quat verify: %s %s %s\n", su_status_name(st), d.json_pointer, d.message);
        return;
    }
    w = run_world(&m, rows, 2, &st);
    CHECK(st == SU_OK);
    if (w != NULL && st == SU_OK) {
        uint32_t r;
        for (r = 0; r < 2; r++) {
            SuQuat q0 = {ONE, 0, 0, 0}, q, qi;
            SuV3 om;
            CHECK(su_q32_from_ratio(1, 10, &q0.x) && su_q32_from_ratio(1, 5, &q0.y) && su_q32_from_ratio(-3, 10, &q0.z));
            q = su_quat_normalize(q0);
            om.x = rows[r].vel;
            om.y = su_q32_add(rows[r].vel, rows[r].vel);
            om.z = su_q32_neg(rows[r].vel);
            qi = su_quat_integrate(q, om, rows[r].dt);
            CHECK(read64(w, "pos", r) == qi.x);
            CHECK(read64(w, "big", r) == su_v3_len(su_quat_rotate(qi, om)));
        }
    }
    su_world_destroy(w);
    su_lir_module_free(&m);
}

/* ---------- VM defensive checks ---------- */

static void test_vm_defensive(void) {
    SuLirModule m;
    SuDiag d;
    SuExecContext *ctx = NULL;
    body(KIN_BODY);
    if (verify(&m, &d) != SU_OK) {
        CHECK(0);
        return;
    }
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_OK);
    su_vm_context_destroy(ctx);
    ctx = NULL;
    /* A 0.3 op in a module claiming 0.2 is unknown to the VM. */
    m.ir_version_minor = 2u;
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_E_INVALID_ARGUMENT && ctx == NULL);
    m.ir_version_minor = 3u;
    /* load_q32 naming the high lane (index 1) is refused. */
    m.insns[0].a = 1u;
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_E_INVALID_ARGUMENT && ctx == NULL);
    m.insns[0].a = 0u;
    /* store_q32 (insn 5) on a read-only q32 field (vel, index 2). */
    m.insns[5].a = 2u;
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_E_INVALID_ARGUMENT && ctx == NULL);
    m.insns[5].a = 0u;
    /* A lane pair whose high lane has the wrong value type. */
    m.fields[1].value_type = SU_TYPE_I64;
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_E_INVALID_ARGUMENT && ctx == NULL);
    m.fields[1].value_type = SU_TYPE_Q32;
    /* Register index out of range. */
    m.insns[3].b = m.register_count;
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_E_INVALID_ARGUMENT && ctx == NULL);
    m.insns[3].b = 2u;
    CHECK(su_vm_context_create(&m, 1, &ctx) == SU_OK);
    su_vm_context_destroy(ctx);
    su_lir_module_free(&m);
}

/* ---------- table sanity ---------- */

static void test_table(void) {
    size_t i, j;
    for (i = 0; i < su_op03_count(); i++) {
        const SuOp03 *a = su_op03_at(i);
        CHECK(su_op03_by_code(a->code) == a);
        CHECK(su_op03_by_name(a->name, strlen(a->name)) == a);
        CHECK(a->code >= 32u && a->code < 256u);
        for (j = 0; j < i; j++) {
            CHECK(su_op03_at(j)->code != a->code);
        }
    }
    printf("ir03 opcode table: %zu operations\n", su_op03_count());
}

int main(void) {
    test_table();
    test_accept_and_lanes();
    test_lane_table();
    test_reject();
    test_hash();
    test_vm_known();
    test_vm_quat();
    test_vm_defensive();
    printf("test_ir03: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
