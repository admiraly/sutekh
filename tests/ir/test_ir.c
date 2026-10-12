/* M0-02 tests: SHA-256 vectors, strict JSON reader, SU-LIR verifier.
 *
 * Run from the repository root (fixture paths are relative). Build with:
 *   gcc -std=c17 -Wall -Wextra -Werror -pedantic -Iinclude -Isrc
 *       src/ir/su_sha256.c src/ir/su_json.c src/ir/su_lir.c tests/ir/test_ir.c
 *       -o <scratch>/test_ir
 * then run <scratch>/test_ir from the repository root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "sutekh/status.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

#define CHECK_CODE(got, want)                                                    \
    do {                                                                         \
        uint32_t gc_ = (got), wc_ = (want);                                      \
        if (gc_ == wc_) {                                                        \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: got %s, want %s (%s)\n", __FILE__, __LINE__, \
                    su_status_name(gc_), su_status_name(wc_), #got);             \
        }                                                                        \
    } while (0)

#define CHECK_STR(got, want)                                                     \
    do {                                                                         \
        const char *gs_ = (got), *ws_ = (want);                                  \
        if (strcmp(gs_, ws_) == 0) {                                             \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, gs_, ws_); \
        }                                                                        \
    } while (0)

/* ---------- file and buffer helpers ---------- */

static int read_file(const char *path, uint8_t **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;
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
    buf = malloc((size_t)sz + 1u);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return 0;
    }
    *out = buf;
    *len = got;
    return 1;
}

/* Verifies files relative to a base directory. SU_E_IO when a file is missing. */
static uint32_t verify_files(const char *cap_path, const char *lir_path,
                             SuLirModule *m, SuDiag *d) {
    uint8_t *cb = NULL, *lb = NULL;
    size_t cl = 0, ll = 0;
    uint32_t rc;

    if (!read_file(cap_path, &cb, &cl) || !read_file(lir_path, &lb, &ll)) {
        fprintf(stderr, "cannot read %s or %s\n", cap_path, lir_path);
        free(cb);
        free(lb);
        su_diag_init(d);
        memset(m, 0, sizeof *m);
        return SU_E_IO;
    }
    rc = su_lir_verify(cb, cl, lb, ll, m, d);
    free(cb);
    free(lb);
    return rc;
}

static uint32_t verify_mem(const char *cap, const char *lir, SuLirModule *m, SuDiag *d) {
    return su_lir_verify((const uint8_t *)cap, strlen(cap), (const uint8_t *)lir, strlen(lir), m, d);
}

static void join_path(char *out, size_t cap, const char *base, const char *rel) {
    snprintf(out, cap, "%s%s", base, rel);
}

/* ---------- SHA-256 ---------- */

static void sha_hex_of(const uint8_t *p, size_t n, char hex[65]) {
    SuSha256 c;
    uint8_t dg[32];
    su_sha256_init(&c);
    su_sha256_update(&c, p, n);
    su_sha256_final(&c, dg);
    su_sha256_hex(dg, hex);
}

static void test_sha256(void) {
    char hex[65];
    static const uint8_t e[1] = {0};
    static const char abc[] = "abc";
    static const char two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t chunk[1000];
    SuSha256 c;
    uint8_t dg[32];
    size_t i;

    sha_hex_of(e, 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    sha_hex_of((const uint8_t *)abc, 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    sha_hex_of((const uint8_t *)two_block, strlen(two_block), hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a', fed in 1000-byte pieces to exercise block buffering. */
    memset(chunk, 'a', sizeof chunk);
    su_sha256_init(&c);
    for (i = 0; i < 1000u; i++) {
        su_sha256_update(&c, chunk, sizeof chunk);
    }
    su_sha256_final(&c, dg);
    su_sha256_hex(dg, hex);
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* ---------- strict JSON reader ---------- */

/* The length is taken from the string itself; the size argument documents
 * intent but is not trusted. */
static uint32_t json_rc(const char *s, size_t n_unused, SuJsonDoc *doc, SuJsonError *err) {
    SuJsonLimits lim;
    (void)n_unused;
    lim.max_bytes = 256u * 1024u;
    lim.max_depth = 32u;
    return su_json_parse((const uint8_t *)s, strlen(s), &lim, doc, err);
}

static void test_json(void) {
    SuJsonDoc doc;
    SuJsonError err;
    uint32_t rc;
    uint64_t u = 0;
    char *big;
    char *nest;
    size_t i;

    rc = json_rc("{\"a\":1,\"a\":2}", 13, &doc, &err);
    CHECK_CODE(rc, SU_E_DUPLICATE_KEY);
    CHECK(doc.root != NULL); /* tree is kept so stage-1 checks can run */
    CHECK_STR(err.pointer, "/a");
    su_json_doc_free(&doc);

    rc = json_rc("{\"x\":{\"a\":1,\"a\":2}}", 22, &doc, &err);
    CHECK_CODE(rc, SU_E_DUPLICATE_KEY);
    CHECK_STR(err.pointer, "/x/a");
    su_json_doc_free(&doc);

    /* Syntax errors outrank duplicate keys. */
    rc = json_rc("{\"a\":1,\"a\":2", 13, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    su_json_doc_free(&doc);

    rc = json_rc("{\"a\" 1}", 7, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("{} x", 4, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("01", 2, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("\"\\u0000\"", 8, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("\"\\uD800\"", 8, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("\"a\tb\"", 5, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);

    /* Valid escapes: surrogate pair decodes to a 4-byte UTF-8 sequence. */
    rc = json_rc("\"\\uD83D\\uDE00\"", 14, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(doc.root != NULL && doc.root->u.string.len == 4u);
    su_json_doc_free(&doc);

    /* Malformed UTF-8: overlong, surrogate, and above U+10FFFF. */
    rc = json_rc("\"\xC0\xAF\"", 4, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("\"\xED\xA0\x80\"", 5, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("\"\xF4\x90\x80\x80\"", 7, &doc, &err);
    CHECK_CODE(rc, SU_E_PARSE);
    rc = json_rc("\"\xC3\xA9\"", 4, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    su_json_doc_free(&doc);

    /* Exact integers: no floating point is involved. */
    rc = json_rc("4294967295", 10, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(su_json_get_uint(doc.root, &u) && u == 4294967295u);
    su_json_doc_free(&doc);

    rc = json_rc("18446744073709551615", 20, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(su_json_get_uint(doc.root, &u) && u == UINT64_C(18446744073709551615));
    su_json_doc_free(&doc);

    rc = json_rc("18446744073709551616", 20, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(!su_json_get_uint(doc.root, &u)); /* overflow flagged, not rounded */
    su_json_doc_free(&doc);

    rc = json_rc("4294967296", 10, &doc, &err);
    CHECK(rc == SU_OK && su_json_get_uint(doc.root, &u) && u == 4294967296u);
    su_json_doc_free(&doc);

    rc = json_rc("-1", 2, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(!su_json_get_uint(doc.root, &u));
    su_json_doc_free(&doc);

    rc = json_rc("1.5", 3, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(!su_json_get_uint(doc.root, &u));
    su_json_doc_free(&doc);

    rc = json_rc("1e3", 3, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    CHECK(!su_json_get_uint(doc.root, &u));
    su_json_doc_free(&doc);

    rc = json_rc("-0", 2, &doc, &err);
    CHECK(rc == SU_OK && su_json_get_uint(doc.root, &u) && u == 0u);
    su_json_doc_free(&doc);

    /* Depth: top-level container is depth 1; 32 is allowed, 33 is not. */
    nest = malloc(70);
    for (i = 0; i < 32u; i++) {
        nest[i] = '[';
        nest[63u - i] = ']';
    }
    nest[64] = '\0';
    rc = json_rc(nest, 64, &doc, &err);
    CHECK_CODE(rc, SU_OK);
    su_json_doc_free(&doc);
    for (i = 0; i < 33u; i++) {
        nest[i] = '[';
        nest[65u - i] = ']';
    }
    nest[66] = '\0';
    rc = json_rc(nest, 66, &doc, &err);
    CHECK_CODE(rc, SU_E_RESOURCE_LIMIT);
    free(nest);

    /* Size: 256 KiB + 1 byte is rejected before any parsing. */
    big = malloc(256u * 1024u + 2u);
    memset(big, ' ', 256u * 1024u + 1u);
    big[256u * 1024u + 1u] = '\0';
    rc = json_rc(big, 256u * 1024u + 1u, &doc, &err);
    CHECK_CODE(rc, SU_E_RESOURCE_LIMIT);
    free(big);
}

/* ---------- SU-LIR verification: examples and fixtures ---------- */

static const char *const T03_DIRS[] = {"eq_edges", "lt_edges", "mul_overflow",
                                       "select_min", "sub_underflow", "wrap_add"};

static void test_valid_fixtures(void) {
    SuLirModule m;
    SuDiag d;
    uint32_t rc;
    char cap[256], lir[256];
    size_t i;

    /* examples/movement.* */
    rc = verify_files("examples/movement.capsule.json", "examples/movement.lir.json", &m, &d);
    CHECK_CODE(rc, SU_OK);
    if (rc == SU_OK) {
        CHECK(m.field_count == 2u);
        CHECK(m.fields[0].id == 1u && m.fields[1].id == 2u);
        CHECK(m.fields[0].access == (SU_ACCESS_READ | SU_ACCESS_WRITE));
        CHECK(m.fields[1].access == SU_ACCESS_READ);
        CHECK_STR(m.fields[0].name, "position_x");
        CHECK(m.insn_count == 4u);
        CHECK(m.max_rows == 1000000u);
        CHECK(m.register_count == 3u);
        CHECK(m.insns[0].op == SU_OP_LOAD_U32 && m.insns[0].a == 0u && m.insns[0].dst == 0u);
        CHECK(m.insns[1].op == SU_OP_LOAD_U32 && m.insns[1].a == 1u && m.insns[1].dst == 1u);
        CHECK(m.insns[2].op == SU_OP_ADD_U32 && m.insns[2].a == 0u && m.insns[2].b == 1u && m.insns[2].dst == 2u);
        CHECK(m.insns[3].op == SU_OP_STORE_U32 && m.insns[3].a == 0u && m.insns[3].b == 2u);
        CHECK(m.insns[3].dst == SU_OPERAND_NONE && m.insns[3].c == SU_OPERAND_NONE);
        CHECK(strlen(m.semantic_hash_hex) == 64u);
        CHECK(strlen(m.source_hash_hex) == 64u);
        su_lir_module_free(&m);
    }

    /* movement_v2 is a different program: different semantic hash. */
    {
        SuLirModule m1, m2;
        SuDiag d1, d2;
        CHECK_CODE(verify_files("examples/movement.capsule.json", "examples/movement.lir.json", &m1, &d1), SU_OK);
        CHECK_CODE(verify_files("examples/movement.capsule.json", "examples/movement_v2.lir.json", &m2, &d2), SU_OK);
        CHECK(memcmp(m1.semantic_hash, m2.semantic_hash, 32) != 0);
        su_lir_module_free(&m1);
        su_lir_module_free(&m2);
    }

    /* tests/fixtures/t02 movement. */
    rc = verify_files("tests/fixtures/t02/movement.capsule.json", "tests/fixtures/t02/movement.lir.json", &m, &d);
    CHECK_CODE(rc, SU_OK);
    su_lir_module_free(&m);

    /* tests/fixtures/t03 programs. */
    for (i = 0; i < sizeof T03_DIRS / sizeof T03_DIRS[0]; i++) {
        snprintf(cap, sizeof cap, "tests/fixtures/t03/%s/capsule.json", T03_DIRS[i]);
        snprintf(lir, sizeof lir, "tests/fixtures/t03/%s/program.lir.json", T03_DIRS[i]);
        rc = verify_files(cap, lir, &m, &d);
        if (rc != SU_OK) {
            fprintf(stderr, "t03 %s: %s %s\n", T03_DIRS[i], su_status_name(rc), d.message);
        }
        CHECK_CODE(rc, SU_OK);
        if (rc == SU_OK) {
            su_lir_module_free(&m);
        }
    }
}

/* Manifest-driven negative cases: each entry names a file and its expected code. */
static int run_negative_manifest(const char *manifest_path, const char *base, size_t expect_count) {
    uint8_t *mb = NULL;
    size_t ml = 0, n;
    SuJsonDoc doc;
    SuJsonError jerr;
    uint32_t rc;
    const SuJsonValue *cases, *top_cap;
    char top[256];
    int ok = 1;

    if (!read_file(manifest_path, &mb, &ml)) {
        fprintf(stderr, "cannot read %s\n", manifest_path);
        return 0;
    }
    rc = su_json_parse(mb, ml, NULL, &doc, &jerr);
    free(mb);
    if (rc != SU_OK) {
        fprintf(stderr, "manifest %s: %s\n", manifest_path, jerr.message);
        return 0;
    }
    top_cap = su_json_member(doc.root, "capsule");
    cases = su_json_member(doc.root, "cases");
    CHECK(cases != NULL && cases->type == SU_JSON_ARRAY);
    CHECK(cases != NULL && cases->u.array.count == expect_count);
    if (top_cap != NULL && top_cap->type == SU_JSON_STRING) {
        snprintf(top, sizeof top, "%.*s", (int)(top_cap->u.string.len < 200 ? top_cap->u.string.len : 200),
                 top_cap->u.string.data);
    } else {
        top[0] = '\0';
    }

    for (n = 0; cases != NULL && n < cases->u.array.count; n++) {
        const SuJsonValue *c = cases->u.array.items[n];
        const SuJsonValue *pv = su_json_member(c, "path");
        const SuJsonValue *cv = su_json_member(c, "capsule");
        const SuJsonValue *ev = su_json_member(c, "expected_code");
        char pbuf[300], cbuf[300], full_l[600], full_c[600], want[64];
        SuLirModule m;
        SuDiag d;
        uint32_t got;

        snprintf(pbuf, sizeof pbuf, "%.*s", (int)pv->u.string.len, pv->u.string.data);
        if (cv != NULL) {
            snprintf(cbuf, sizeof cbuf, "%.*s", (int)cv->u.string.len, cv->u.string.data);
        } else {
            snprintf(cbuf, sizeof cbuf, "%s", top);
        }
        snprintf(want, sizeof want, "%.*s", (int)ev->u.string.len, ev->u.string.data);
        join_path(full_l, sizeof full_l, base, pbuf);
        join_path(full_c, sizeof full_c, base, cbuf);

        got = verify_files(full_c, full_l, &m, &d);
        if (strcmp(su_status_name(got), want) != 0) {
            fprintf(stderr, "FAIL negative %s: got %s, want %s (%s)\n", pbuf,
                    su_status_name(got), want, d.message);
            ok = 0;
            g_fail++;
        } else {
            g_pass++;
        }
        if (got == SU_OK) {
            su_lir_module_free(&m);
        }
    }
    su_json_doc_free(&doc);
    return ok;
}

static void test_negative_manifests(void) {
    run_negative_manifest("tests/fixtures/negative_cases.json", "tests/fixtures/", 7u);
    run_negative_manifest("examples/negative_cases.json", "examples/", 7u);
}

/* ---------- semantic hash identity ---------- */

static void test_hash_identity(void) {
    /* movement.lir.json reformatted: compact, keys reordered inside each object. */
    static const char lir_compact[] =
        "{\"query\":\"demo.actor\",\"numeric_profile\":\"u32_mod_v1\",\"system_id\":\"demo.movement\","
        "\"ir_version\":\"0.1\",\"instructions\":["
        "{\"field\":\"position_x\",\"dst\":\"p\",\"op\":\"load_u32\"},"
        "{\"dst\":\"v\",\"op\":\"load_u32\",\"field\":\"velocity_x\"},"
        "{\"b\":\"v\",\"a\":\"p\",\"dst\":\"next\",\"op\":\"add_u32\"},"
        "{\"src\":\"next\",\"op\":\"store_u32\",\"field\":\"position_x\"}]}";
    /* Same capsule with the fields declared in reverse order, renamed, and a
     * different layout_generation. Names, order and layout are not identity. */
    static const char cap_variant[] =
        "{\"capsule_format\":\"0.1\",\"system_id\":\"demo.movement\",\"source\":\"x.lir.json\","
        "\"schema\":{\"id\":\"demo.actor\",\"version\":1,\"layout_generation\":7},"
        "\"numeric_profile\":\"u32_mod_v1\",\"fields\":["
        "{\"id\":2,\"name\":\"speed_x\",\"type\":\"u32\",\"read\":true,\"write\":false},"
        "{\"id\":1,\"name\":\"pos_x\",\"type\":\"u32\",\"read\":true,\"write\":true}],"
        "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
        "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
        "\"tests\":[\"t.json\"]}";
    static const char lir_renamed[] =
        "{\"ir_version\":\"0.1\",\"system_id\":\"demo.movement\",\"numeric_profile\":\"u32_mod_v1\","
        "\"query\":\"demo.actor\",\"instructions\":["
        "{\"op\":\"load_u32\",\"dst\":\"p\",\"field\":\"pos_x\"},"
        "{\"op\":\"load_u32\",\"dst\":\"v\",\"field\":\"speed_x\"},"
        "{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"v\"},"
        "{\"op\":\"store_u32\",\"field\":\"pos_x\",\"src\":\"n\"}]}";
    SuLirModule base, compact, renamed;
    SuDiag d;
    uint8_t *cb = NULL, *lb = NULL;
    size_t cl = 0;

    CHECK_CODE(verify_files("examples/movement.capsule.json", "examples/movement.lir.json", &base, &d), SU_OK);

    /* Compact text under the real capsule file. */
    read_file("examples/movement.capsule.json", &cb, &cl);
    CHECK_CODE(su_lir_verify(cb, cl, (const uint8_t *)lir_compact, strlen(lir_compact), &compact, &d), SU_OK);
    CHECK(memcmp(base.semantic_hash, compact.semantic_hash, 32) == 0);
    CHECK(memcmp(base.source_hash, compact.source_hash, 32) != 0); /* bytes differ */
    CHECK(memcmp(base.capsule_hash, compact.capsule_hash, 32) == 0);
    su_lir_module_free(&compact);

    /* Renamed fields and registers, reordered declarations, new layout. */
    CHECK_CODE(verify_mem(cap_variant, lir_renamed, &renamed, &d), SU_OK);
    if (renamed.field_count == 2u) {
        CHECK_STR(renamed.fields[0].name, "pos_x");
        CHECK_STR(renamed.fields[1].name, "speed_x");
    }
    CHECK(memcmp(base.semantic_hash, renamed.semantic_hash, 32) == 0);
    CHECK(memcmp(base.capsule_hash, renamed.capsule_hash, 32) != 0);
    su_lir_module_free(&renamed);

    su_lir_module_free(&base);
    free(cb);
    free(lb);
}

/* ---------- targeted verifier cases (stage order and rules) ---------- */

static const char CAP_MOVE[] =
    "{\"capsule_format\":\"0.1\",\"system_id\":\"demo.movement\",\"source\":\"x\","
    "\"schema\":{\"id\":\"demo.actor\",\"version\":1,\"layout_generation\":1},"
    "\"numeric_profile\":\"u32_mod_v1\",\"fields\":["
    "{\"id\":1,\"name\":\"position_x\",\"type\":\"u32\",\"read\":true,\"write\":true},"
    "{\"id\":2,\"name\":\"velocity_x\",\"type\":\"u32\",\"read\":true,\"write\":false}],"
    "\"schedule\":{\"phase\":\"simulate\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},"
    "\"limits\":{\"max_instructions\":4096,\"max_rows\":1000000,\"allocation\":\"forbidden\"},"
    "\"tests\":[\"t.json\"]}";

#define IR_HEAD "{\"ir_version\":\"0.1\",\"system_id\":\"demo.movement\",\"numeric_profile\":\"u32_mod_v1\",\"query\":\"demo.actor\","
#define LOAD_P "{\"op\":\"load_u32\",\"dst\":\"p\",\"field\":\"position_x\"},"
#define LOAD_V "{\"op\":\"load_u32\",\"dst\":\"v\",\"field\":\"velocity_x\"},"
#define ADD_N "{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"p\",\"b\":\"v\"},"
#define STORE_N "{\"op\":\"store_u32\",\"field\":\"position_x\",\"src\":\"n\"}"

static void expect_mem(const char *cap, const char *lir, uint32_t want, const char *ptr_want) {
    SuLirModule m;
    SuDiag d;
    uint32_t rc = verify_mem(cap, lir, &m, &d);
    if (rc != want) {
        fprintf(stderr, "FAIL inline: got %s, want %s (%s at %s)\n", su_status_name(rc),
                su_status_name(want), d.message, d.json_pointer);
        g_fail++;
    } else {
        g_pass++;
        if (ptr_want != NULL) {
            CHECK_STR(d.json_pointer, ptr_want);
        }
    }
    if (rc == SU_OK) {
        su_lir_module_free(&m);
    }
}

static void test_targeted(void) {
    /* Missing store at end of body (stage 7). */
    expect_mem(CAP_MOVE, IR_HEAD "\"instructions\":[" LOAD_P "{\"op\":\"load_u32\",\"dst\":\"v\",\"field\":\"velocity_x\"}]}",
               SU_E_MISSING_STORE, "/instructions");

    /* Select condition must be bool. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[" LOAD_P LOAD_V
               "{\"op\":\"select_u32\",\"dst\":\"r\",\"cond\":\"p\",\"on_true\":\"p\",\"on_false\":\"v\"},"
               "{\"op\":\"store_u32\",\"field\":\"position_x\",\"src\":\"r\"}]}",
               SU_E_TYPE, "/instructions/2/cond");

    /* Equality on bool operand. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[" LOAD_P LOAD_V
               "{\"op\":\"eq_u32\",\"dst\":\"c\",\"a\":\"p\",\"b\":\"v\"},"
               "{\"op\":\"eq_u32\",\"dst\":\"c2\",\"a\":\"c\",\"b\":\"v\"},"
               STORE_N "]}",
               SU_E_TYPE, "/instructions/3/a");

    /* Load of an unknown field: undeclared read. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"p\",\"field\":\"nope\"},"
               "{\"op\":\"store_u32\",\"field\":\"position_x\",\"src\":\"p\"}]}",
               SU_E_UNDECLARED_READ, "/instructions/0/field");

    /* Unknown key on an instruction is a type error, not an opcode error. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[{\"op\":\"load_u32\",\"dst\":\"p\",\"field\":\"position_x\",\"extra\":1}]}",
               SU_E_TYPE, "/instructions/0/extra");

    /* Unknown top-level key outranks instruction-level unknown opcode. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"extra\":1,\"instructions\":[{\"op\":\"teleport\"}]}",
               SU_E_TYPE, "/extra");

    /* Unknown opcode in instruction order: earlier instruction's shape wins. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[{\"op\":\"teleport\",\"dst\":\"x\"}," LOAD_P STORE_N "]}",
               SU_E_UNKNOWN_OPCODE, "/instructions/0/op");

    /* Duplicate key anywhere outranks a later semantic error. */
    expect_mem(CAP_MOVE,
               "{\"ir_version\":\"0.1\",\"ir_version\":\"0.1\",\"system_id\":\"demo.movement\","
               "\"numeric_profile\":\"u32_mod_v1\",\"query\":\"demo.actor\",\"instructions\":["
               "{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"zz\",\"b\":\"zz\"}]}",
               SU_E_DUPLICATE_KEY, "/ir_version");

    /* Unsupported numeric profile in the IR: top-level shape. */
    expect_mem(CAP_MOVE,
               "{\"ir_version\":\"0.1\",\"system_id\":\"demo.movement\",\"numeric_profile\":\"f32\","
               "\"query\":\"demo.actor\",\"instructions\":[]}",
               SU_E_TYPE, "/numeric_profile");

    /* Query differs from the capsule schema: schema mismatch. */
    expect_mem(CAP_MOVE,
               "{\"ir_version\":\"0.1\",\"system_id\":\"demo.movement\",\"numeric_profile\":\"u32_mod_v1\","
               "\"query\":\"demo.other\",\"instructions\":[]}",
               SU_E_SCHEMA_MISMATCH, "/query");

    /* Second store to a writable field. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[" LOAD_P LOAD_V ADD_N STORE_N
               ",{\"op\":\"store_u32\",\"field\":\"position_x\",\"src\":\"p\"}]}",
               SU_E_UNDECLARED_WRITE, "/instructions/4/field");

    /* Undefined register used before definition. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[{\"op\":\"add_u32\",\"dst\":\"n\",\"a\":\"later\",\"b\":\"later\"}]}",
               SU_E_UNDEFINED_REGISTER, "/instructions/0/a");

    /* Constant range is part of instruction shape. */
    expect_mem(CAP_MOVE,
               IR_HEAD "\"instructions\":[{\"op\":\"const_u32\",\"dst\":\"k\",\"value\":\"7\"}]}",
               SU_E_TYPE, "/instructions/0/value");
}

/* ---------- IR 0.2 (M2-00c2): negatives by stage, positives, hashes ---------- */

static void test_ir02_negatives(void) {
    run_negative_manifest("tests/fixtures/ir02/manifest.json", "tests/fixtures/ir02/", 25u);
}

static void test_ir02_positives(void) {
    static const char *const ok[] = {"ok_valid", "ok_kind_max", "ok_cap_exact", "ok_cap_64"};
    static const uint32_t smax[] = {4u, 4u, 1u, 64u};
    size_t i;
    for (i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        char cap[256], lir[256];
        SuLirModule m;
        SuDiag d;
        uint32_t rc;
        snprintf(cap, sizeof cap, "tests/fixtures/ir02/%s.capsule.json", ok[i]);
        snprintf(lir, sizeof lir, "tests/fixtures/ir02/%s.lir.json", ok[i]);
        rc = verify_files(cap, lir, &m, &d);
        CHECK_CODE(rc, SU_OK);
        if (rc == SU_OK) {
            CHECK(m.ir_version_minor == 2u);
            CHECK(m.artifact_format == SU_ARTIFACT_FORMAT_VERSION_0_2);
            CHECK(m.uses_entity_ops == 1u);
            CHECK(m.max_struct_per_row == smax[i]);
            CHECK(m.max_events_per_row == smax[i]);
            su_lir_module_free(&m);
        } else {
            fprintf(stderr, "  %s: %s\n", ok[i], d.message);
        }
    }
}

/* Movement (0.1) semantic hash: identical to the M0 value recorded before IR 0.2 (M2-00c2 proof). */
static void test_ir02_hashes(void) {
    static const char M0_MOVEMENT_SEMANTIC[] =
        "b9e6bd62b4c9aff3e27c114b14f64f02c074c3ba529c3729bef42f15955065b6";
    SuLirModule a, b, c;
    SuDiag d;
    CHECK_CODE(verify_files("examples/movement.capsule.json", "examples/movement.lir.json", &a, &d), SU_OK);
    CHECK_STR(a.semantic_hash_hex, M0_MOVEMENT_SEMANTIC);
    CHECK(a.artifact_format == SU_ARTIFACT_FORMAT_VERSION);
    CHECK(a.ir_version_minor == 1u);
    CHECK(a.uses_entity_ops == 0u);
    su_lir_module_free(&a);

    /* 0.2: caps are part of the hash (same body, different max_struct_per_row). */
    CHECK_CODE(verify_files("tests/fixtures/ir02/ok_cap_exact.capsule.json",
                            "tests/fixtures/ir02/ok_cap_exact.lir.json", &b, &d), SU_OK);
    CHECK_CODE(verify_files("tests/fixtures/ir02/ok_cap_64.capsule.json",
                            "tests/fixtures/ir02/ok_cap_64.lir.json", &c, &d), SU_OK);
    CHECK(memcmp(b.semantic_hash, c.semantic_hash, 32) != 0);
    su_lir_module_free(&b);
    su_lir_module_free(&c);

    /* Regression pin of the 0.2 encoding (value produced by this encoding, not an external vector). */
    CHECK_CODE(verify_files("tests/fixtures/ir02/ok_valid.capsule.json",
                            "tests/fixtures/ir02/ok_valid.lir.json", &b, &d), SU_OK);
    CHECK_STR(b.semantic_hash_hex, "cbd6d237363cd208b71c02de59f03ecd7f6d0e0c0b4b609bee002964e4db682d");
    su_lir_module_free(&b);
}

/* Audit item 4 (C04): capsule "capabilities" are enforced and "system_rev" is recorded. The verdicts are
 * in tests/fixtures/caps/manifest.json; here the recorded fields are checked on the granted module, and
 * the semantic hash is shown to exclude both keys (the ok_valid pin is unchanged). */
static void test_caps(void) {
    SuLirModule a, b, c;
    SuDiag d;

    run_negative_manifest("tests/fixtures/caps/manifest.json", "tests/fixtures/caps/", 11u);

    CHECK_CODE(verify_files("tests/fixtures/caps/grant_both_ok.capsule.json",
                            "tests/fixtures/caps/base.lir.json", &a, &d), SU_OK);
    CHECK(a.has_capabilities == 1u);
    CHECK(a.capabilities == (SU_LIR_CAP_DESPAWN_SELF | SU_LIR_CAP_EMIT_EVENT));
    CHECK(a.has_system_rev == 1u);
    CHECK(a.system_rev == 3u);
    CHECK_STR(a.semantic_hash_hex, "cbd6d237363cd208b71c02de59f03ecd7f6d0e0c0b4b609bee002964e4db682d");
    su_lir_module_free(&a);

    CHECK_CODE(verify_files("tests/fixtures/caps/grant_both_no_rev_ok.capsule.json",
                            "tests/fixtures/caps/base.lir.json", &b, &d), SU_OK);
    CHECK(b.has_capabilities == 1u);
    CHECK(b.has_system_rev == 0u);
    su_lir_module_free(&b);

    /* Absent keys: nothing recorded, nothing enforced (M0-M2 verdicts). */
    CHECK_CODE(verify_files("tests/fixtures/ir02/ok_valid.capsule.json",
                            "tests/fixtures/ir02/ok_valid.lir.json", &c, &d), SU_OK);
    CHECK(c.has_capabilities == 0u);
    CHECK(c.has_system_rev == 0u);
    su_lir_module_free(&c);
}

int main(void) {
    test_sha256();
    test_json();
    test_valid_fixtures();
    test_negative_manifests();
    test_hash_identity();
    test_targeted();
    test_ir02_negatives();
    test_ir02_positives();
    test_ir02_hashes();
    test_caps();

    printf("test_ir: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
