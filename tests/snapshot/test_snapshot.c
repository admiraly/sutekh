/* M0-08 tests: canonical snapshots (T08), no partial restore, bounded checkpoint
 * store. Self-contained assert harness. Usage: test_snapshot [path/to/world.json]
 * (default examples/world.json, relative to the repository root). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_sha256.h"
#include "runtime/checkpoint.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "world/snapshot.h"
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

/* ---- example world ---------------------------------------------------- */

typedef struct Example {
    uint32_t rows;
    uint32_t *ids;
    uint32_t *pos;
    uint32_t *vel;
} Example;

/* Field IDs and access as in examples/movement.capsule.json. Declared out of
 * id order on purpose: the snapshot must still emit ascending order. */
static const SuWorldFieldDesc k_fields_decl[] = {
    {2u, SU_TYPE_U32, SU_ACCESS_READ, "velocity_x"},
    {1u, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "position_x"},
};
#define ID_POS 1u
#define ID_VEL 2u

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc(n > 0 ? (size_t)n : 1u);
    if (buf == NULL || fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static uint32_t *u32_array(const SuJsonValue *arr, uint32_t *rows_out) {
    uint32_t *v;
    size_t i;
    if (arr == NULL || arr->type != SU_JSON_ARRAY) {
        return NULL;
    }
    v = malloc((arr->u.array.count > 0 ? arr->u.array.count : 1u) * sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    for (i = 0; i < arr->u.array.count; i++) {
        uint64_t x = 0;
        if (!su_json_get_uint(arr->u.array.items[i], &x) || x > UINT32_MAX) {
            free(v);
            return NULL;
        }
        v[i] = (uint32_t)x;
    }
    *rows_out = (uint32_t)arr->u.array.count;
    return v;
}

static int load_example(const char *path, Example *ex) {
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    SuJsonLimits lim;
    SuJsonDoc doc;
    SuJsonError je;
    const SuJsonValue *root;
    const SuJsonValue *cols;
    const SuJsonValue *sid;
    uint32_t rows_a = 0;
    uint32_t rows_b = 0;
    uint32_t st;
    int ok = 0;

    memset(ex, 0, sizeof *ex);
    memset(&doc, 0, sizeof doc);
    if (buf == NULL) {
        return 0;
    }
    lim.max_bytes = len;
    lim.max_depth = 16u;
    st = su_json_parse(buf, len, &lim, &doc, &je);
    if (st != SU_OK) {
        goto out;
    }
    root = doc.root;
    sid = su_json_member(root, "schema_id");
    cols = su_json_member(root, "columns");
    if (sid == NULL || sid->type != SU_JSON_STRING || sid->u.string.len != strlen("demo.actor") ||
        memcmp(sid->u.string.data, "demo.actor", 10u) != 0 || cols == NULL) {
        goto out;
    }
    ex->ids = u32_array(su_json_member(root, "entity_ids"), &ex->rows);
    ex->pos = u32_array(su_json_member(cols, "position_x"), &rows_a);
    ex->vel = u32_array(su_json_member(cols, "velocity_x"), &rows_b);
    ok = ex->ids != NULL && ex->pos != NULL && ex->vel != NULL && rows_a == ex->rows &&
         rows_b == ex->rows && ex->rows == 4u;
out:
    su_json_doc_free(&doc);
    free(buf);
    if (!ok) {
        free(ex->ids);
        free(ex->pos);
        free(ex->vel);
        memset(ex, 0, sizeof *ex);
    }
    return ok;
}

static void example_free(Example *ex) {
    free(ex->ids);
    free(ex->pos);
    free(ex->vel);
    memset(ex, 0, sizeof *ex);
}

static void make_desc(const Example *ex, SuWorldDesc *d) {
    memset(d, 0, sizeof *d);
    d->schema_id = "demo.actor";
    d->schema_version = 1u;
    d->layout_generation = 1u;
    d->row_count = ex->rows;
    d->entity_ids = ex->ids;
    d->fields = k_fields_decl;
    d->field_count = 2u;
}

static uint32_t make_example_world(const Example *ex, SuWorld **w) {
    SuWorldDesc d;
    uint32_t st;
    make_desc(ex, &d);
    st = su_world_create(&d, w);
    if (st != SU_OK) {
        return st;
    }
    st = su_world_init_committed(*w, ID_POS, ex->pos, ex->rows);
    if (st == SU_OK) {
        st = su_world_init_committed(*w, ID_VEL, ex->vel, ex->rows);
    }
    if (st != SU_OK) {
        su_world_destroy(*w);
        *w = NULL;
    }
    return st;
}

static const SuColumnBinding *find_binding(const SuColumnBinding *b, uint32_t n, uint32_t id) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (b[i].field_id == id) {
            return &b[i];
        }
    }
    return NULL;
}

/* One real tick: position_x += velocity_x (mod 2^32), staged then committed.
 * Returns SU_OK only if every protocol step succeeded. */
static uint32_t tick_once(SuWorld *w) {
    SuInvocation inv;
    SuOutputs out;
    const SuColumnBinding *pin;
    const SuColumnBinding *vin;
    const SuColumnBinding *pout;
    uint32_t ids[1] = {ID_POS};
    uint64_t r;
    uint32_t st = su_world_begin_tick(w);
    if (st != SU_OK) {
        return st;
    }
    st = su_world_build_invocation(w, &inv);
    if (st == SU_OK) {
        st = su_world_build_outputs(w, &out);
    }
    if (st != SU_OK) {
        su_world_abort(w);
        return st;
    }
    pin = find_binding(inv.bindings, inv.binding_count, ID_POS);
    vin = find_binding(inv.bindings, inv.binding_count, ID_VEL);
    pout = find_binding(out.bindings, out.binding_count, ID_POS);
    if (pin == NULL || vin == NULL || pout == NULL) {
        su_world_abort(w);
        return SU_E_INTERNAL;
    }
    for (r = 0; r < inv.row_count; r++) {
        ((uint32_t *)pout->data)[r] = ((const uint32_t *)pin->data)[r] + ((const uint32_t *)vin->data)[r];
    }
    st = su_world_commit_staging(w, ids, 1u);
    if (st != SU_OK) {
        su_world_abort(w);
        return st;
    }
    return su_world_publish(w);
}

static uint32_t run_ticks(SuWorld *w, uint32_t n) {
    uint32_t i;
    uint32_t st = SU_OK;
    for (i = 0; i < n && st == SU_OK; i++) {
        st = tick_once(w);
    }
    return st;
}

/* Canonical state dump of the committed root (su_world's documented encoding). */
static uint8_t *dump_world(const SuWorld *w, size_t *len) {
    size_t n = 0;
    uint8_t *b;
    if (su_world_canonical_size(w, &n) != SU_OK) {
        return NULL;
    }
    b = malloc(n > 0 ? n : 1u);
    if (b == NULL) {
        return NULL;
    }
    if (su_world_canonical_write(w, b, n, len) != SU_OK) {
        free(b);
        return NULL;
    }
    return b;
}

static int dump_equal(const SuWorld *w, const uint8_t *want, size_t want_len) {
    size_t n = 0;
    uint8_t *got = dump_world(w, &n);
    int eq = got != NULL && n == want_len && memcmp(got, want, n) == 0;
    free(got);
    return eq;
}

/* ---- manifest --------------------------------------------------------- */

static SuSnapshotManifestEntry k_manifest[2];

static void init_manifest(void) {
    memset(k_manifest, 0, sizeof k_manifest);
    k_manifest[0].system_id = "demo.movement";
    memset(k_manifest[0].semantic_hash, 'a', 64u);
    k_manifest[1].system_id = "demo.audit";
    memset(k_manifest[1].semantic_hash, '0', 63u);
    k_manifest[1].semantic_hash[63] = '9';
    k_manifest[1].semantic_hash[64] = '\0';
    k_manifest[0].semantic_hash[64] = '\0';
}

static int manifest_equal(const SuSnapshotManifestEntry *a, uint32_t na,
                          const SuSnapshotManifestEntry *b, uint32_t nb) {
    uint32_t i;
    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; i++) {
        if (strcmp(a[i].system_id, b[i].system_id) != 0 ||
            strcmp(a[i].semantic_hash, b[i].semantic_hash) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- byte helpers ----------------------------------------------------- */

static uint8_t *replace_once(const uint8_t *src, size_t n, const char *needle, const char *repl,
                             size_t *out_n) {
    size_t nl = strlen(needle);
    size_t rl = strlen(repl);
    size_t i;
    size_t total;
    uint8_t *o;
    for (i = 0; i + nl <= n; i++) {
        if (memcmp(src + i, needle, nl) == 0) {
            break;
        }
    }
    if (i + nl > n) {
        return NULL;
    }
    total = n - nl + rl;
    o = malloc(total > 0 ? total : 1u);
    if (o == NULL) {
        return NULL;
    }
    memcpy(o, src, i);
    memcpy(o + i, repl, rl);
    memcpy(o + i + rl, src + i + nl, n - i - nl);
    *out_n = total;
    return o;
}

static void capture_or_die(const SuWorld *w, const SuWorldDesc *d, const char *rev,
                           SuSnapshot *snap) {
    uint32_t st = su_snapshot_capture(w, d, rev, k_manifest, 2u, snap);
    if (st != SU_OK) {
        fprintf(stderr, "capture failed: %s\n", su_status_name(st));
        abort();
    }
}

/* ---- T08: round trip -------------------------------------------------- */

static void test_round_trip(const Example *ex) {
    static const uint32_t ticks[3] = {0u, 1u, 3u};
    uint32_t k;
    for (k = 0; k < 3u; k++) {
        uint32_t n = ticks[k];
        SuWorld *w = NULL;
        SuWorld *w2 = NULL;
        SuWorldDesc d;
        SuSnapshot snap;
        SuSnapshotData data;
        SuSnapshotError err;
        const SuSnapshotManifestEntry *man = NULL;
        uint32_t man_n = 0;
        SuSnapshot again;
        uint32_t i;
        char rev[32];

        make_desc(ex, &d);
        CHECK_ST(make_example_world(ex, &w), SU_OK);
        CHECK_ST(run_ticks(w, n), SU_OK);
        CHECK(su_world_tick(w) == n);
        snprintf(rev, sizeof rev, "rev-%u", (unsigned)n);
        capture_or_die(w, &d, rev, &snap);
        CHECK(snap.tick == n);
        CHECK(snap.len > 0u);

        /* Parse and compare everything against the live world. */
        memset(&data, 0, sizeof data);
        CHECK_ST(su_snapshot_parse(snap.bytes, snap.len, &d, &data, &err), SU_OK);
        CHECK(data.tick == n);
        CHECK(strcmp(data.world_revision, rev) == 0);
        CHECK(manifest_equal(data.manifest, data.manifest_count, k_manifest, 2u));
        CHECK(data.row_count == ex->rows);
        for (i = 0; i < 2u && data.columns != NULL; i++) {
            const uint32_t *committed = NULL;
            uint64_t rows = 0;
            uint32_t id = i == 0u ? ID_POS : ID_VEL;
            CHECK_ST(su_world_read_committed(w, id, &committed, &rows), SU_OK);
            /* data.columns is in ascending id order: 1 then 2. */
            CHECK(memcmp(data.columns[i], committed, ex->rows * sizeof(uint32_t)) == 0);
        }

        /* Restore into a fresh world; state, tick and manifest must be exact. */
        CHECK_ST(su_snapshot_restore(&d, &data, &w2, &man, &man_n), SU_OK);
        CHECK(w2 != NULL);
        if (w2 != NULL) {
            int eq = 0;
            CHECK(su_world_tick(w2) == n);
            CHECK_ST(su_world_state_equal(w, w2, &eq), SU_OK);
            CHECK(eq == 1);
            CHECK(manifest_equal(man, man_n, k_manifest, 2u));

            /* Re-capture with the same revision: bytes must be identical. */
            capture_or_die(w2, &d, rev, &again);
            CHECK(again.len == snap.len);
            CHECK(again.len == snap.len && memcmp(again.bytes, snap.bytes, snap.len) == 0);
            CHECK(strcmp(again.id, snap.id) == 0);
            su_snapshot_destroy(&again);

            /* Both worlds keep evolving identically. */
            CHECK_ST(run_ticks(w, 2u), SU_OK);
            CHECK_ST(run_ticks(w2, 2u), SU_OK);
            CHECK_ST(su_world_state_equal(w, w2, &eq), SU_OK);
            CHECK(eq == 1);
            su_world_destroy(w2);
        }
        su_snapshot_data_free(&data);
        su_snapshot_destroy(&snap);
        su_world_destroy(w);
    }
}

/* ---- T08: determinism and format -------------------------------------- */

static void test_determinism_and_format(const Example *ex) {
    SuWorld *a = NULL;
    SuWorld *b = NULL;
    SuWorldDesc d;
    SuSnapshot sa;
    SuSnapshot sb;
    SuSnapshot sa2;
    static const char want_cols[] =
        "\"columns\":{\"position_x\":[0,1,4294967295,7],\"velocity_x\":[1,2,1,0]}}";

    make_desc(ex, &d);
    CHECK_ST(make_example_world(ex, &a), SU_OK);
    CHECK_ST(make_example_world(ex, &b), SU_OK);
    capture_or_die(a, &d, "rev-det", &sa);
    capture_or_die(b, &d, "rev-det", &sb);
    CHECK(sa.len == sb.len && memcmp(sa.bytes, sb.bytes, sa.len) == 0);
    CHECK(memcmp(sa.digest, sb.digest, 32u) == 0);
    capture_or_die(a, &d, "rev-det", &sa2);
    CHECK(sa.len == sa2.len && memcmp(sa.bytes, sa2.bytes, sa.len) == 0);
    CHECK(strcmp(sa.id, sb.id) == 0);

    /* Identity is SHA-256 of the canonical bytes. */
    {
        SuSha256 ctx;
        uint8_t dig[32];
        char hex[65];
        su_sha256_init(&ctx);
        su_sha256_update(&ctx, sa.bytes, sa.len);
        su_sha256_final(&ctx, dig);
        su_sha256_hex(dig, hex);
        CHECK(strcmp(hex, sa.id) == 0);
    }

    /* Canonical shape: starts with the format and ends with columns. */
    {
        static const char prefix[] = "{\"snapshot_format\":\"0.1\",\"world_revision\":\"rev-det\"";
        CHECK(sa.len >= strlen(prefix) && memcmp(sa.bytes, prefix, strlen(prefix)) == 0);
    }
    CHECK(sa.len >= strlen(want_cols) &&
          memcmp(sa.bytes + sa.len - strlen(want_cols), want_cols, strlen(want_cols)) == 0);

    /* Equal worlds stay equal after identical ticks; unequal worlds differ. */
    CHECK_ST(run_ticks(a, 1u), SU_OK);
    CHECK_ST(run_ticks(b, 1u), SU_OK);
    {
        SuSnapshot sc;
        SuSnapshot sd;
        capture_or_die(a, &d, "rev-det", &sc);
        capture_or_die(b, &d, "rev-det", &sd);
        CHECK(sc.len == sd.len && memcmp(sc.bytes, sd.bytes, sc.len) == 0);
        CHECK(!(sc.len == sa.len && memcmp(sc.bytes, sa.bytes, sa.len) == 0));
        su_snapshot_destroy(&sc);
        su_snapshot_destroy(&sd);
    }

    su_snapshot_destroy(&sa);
    su_snapshot_destroy(&sb);
    su_snapshot_destroy(&sa2);
    su_world_destroy(a);
    su_world_destroy(b);
}

/* ---- capture refusals -------------------------------------------------- */

static void test_capture_refusals(const Example *ex) {
    SuWorld *w = NULL;
    SuWorldDesc d;
    SuSnapshot snap;
    SuWorldFieldDesc renamed[2];
    SuWorldDesc bad;
    SuSnapshotManifestEntry badm[1];
    size_t before_len = 0;
    uint8_t *before;

    make_desc(ex, &d);
    CHECK_ST(make_example_world(ex, &w), SU_OK);
    before = dump_world(w, &before_len);
    CHECK(before != NULL);

    /* Inside a tick: refused, world untouched. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    memset(&snap, 0, sizeof snap);
    CHECK_ST(su_snapshot_capture(w, &d, "rev", k_manifest, 2u, &snap), SU_E_INVALID_ARGUMENT);
    CHECK(snap.bytes == NULL && snap.len == 0u);
    CHECK_ST(su_world_abort(w), SU_OK);

    /* Descriptor disagrees with the world: layout generation. */
    bad = d;
    bad.layout_generation = 2u;
    CHECK_ST(su_snapshot_capture(w, &bad, "rev", k_manifest, 2u, &snap), SU_E_SCHEMA_MISMATCH);
    /* Descriptor names a field the world does not have. */
    renamed[0] = k_fields_decl[0];
    renamed[1] = k_fields_decl[1];
    renamed[1].name = "position_y";
    bad = d;
    bad.fields = renamed;
    CHECK_ST(su_snapshot_capture(w, &bad, "rev", k_manifest, 2u, &snap), SU_E_SCHEMA_MISMATCH);
    /* Descriptor entity IDs disagree with the world. */
    {
        uint32_t ids[4];
        memcpy(ids, ex->ids, sizeof ids);
        ids[0] = 99u;
        bad = d;
        bad.entity_ids = ids;
        CHECK_ST(su_snapshot_capture(w, &bad, "rev", k_manifest, 2u, &snap), SU_E_SCHEMA_MISMATCH);
    }

    /* Invalid arguments. */
    CHECK_ST(su_snapshot_capture(w, &d, "", k_manifest, 2u, &snap), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_snapshot_capture(NULL, &d, "rev", k_manifest, 2u, &snap), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_snapshot_capture(w, &d, "rev", NULL, 2u, &snap), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_snapshot_capture(w, &d, "rev", k_manifest, 2u, NULL), SU_E_INVALID_ARGUMENT);
    badm[0] = k_manifest[0];
    memset(badm[0].semantic_hash, 'A', 64u); /* uppercase hex is not canonical */
    badm[0].semantic_hash[64] = '\0';
    CHECK_ST(su_snapshot_capture(w, &d, "rev", badm, 1u, &snap), SU_E_INVALID_ARGUMENT);

    /* Nothing was changed by any refusal. */
    CHECK(dump_equal(w, before, before_len));
    CHECK(su_world_tick(w) == 0u);

    free(before);
    su_world_destroy(w);
}

/* ---- corrupt, truncated, oversize, duplicate, wrong schema ------------- */

typedef struct Corruption {
    const char *name;
    uint32_t want;
    /* Exactly one of: needle/repl mutation, truncation (cut), or oversize. */
    const char *needle;
    const char *repl;
    size_t cut;      /* bytes removed from the end when > 0 */
    int oversize;    /* pass len > SU_SNAPSHOT_MAX_BYTES */
    int empty;       /* pass len 0 */
} Corruption;

static const Corruption k_corruptions[] = {
    {"truncated by one byte", SU_E_PARSE, NULL, NULL, 1u, 0, 0},
    {"truncated to half", SU_E_PARSE, NULL, NULL, 0u, 0, 0}, /* handled specially */
    {"empty input", SU_E_PARSE, NULL, NULL, 0u, 0, 1},
    {"oversize input", SU_E_RESOURCE_LIMIT, NULL, NULL, 0u, 1, 0},
    {"duplicate key", SU_E_DUPLICATE_KEY, "{\"snapshot_format\"", "{\"tick\":3,\"snapshot_format\"", 0u, 0, 0},
    {"unknown top-level key", SU_E_TYPE, "{\"snapshot_format\"", "{\"extra\":0,\"snapshot_format\"", 0u, 0, 0},
    {"tick as string", SU_E_TYPE, "\"tick\":3,", "\"tick\":\"3\",", 0u, 0, 0},
    {"tick negative", SU_E_TYPE, "\"tick\":3,", "\"tick\":-3,", 0u, 0, 0},
    {"tick fractional", SU_E_TYPE, "\"tick\":3,", "\"tick\":3.0,", 0u, 0, 0},
    {"wrong snapshot_format", SU_E_TYPE, "\"snapshot_format\":\"0.1\"", "\"snapshot_format\":\"0.2\"", 0u, 0, 0},
    {"invalid UTF-8 in revision", SU_E_PARSE, "\"rev-3\"", "\"\xff\"", 0u, 0, 0},
    {"wrong schema_id", SU_E_SCHEMA_MISMATCH, "\"schema_id\":\"demo.actor\"", "\"schema_id\":\"demo.other\"", 0u, 0, 0},
    {"wrong schema_version", SU_E_SCHEMA_MISMATCH, "\"schema_version\":1", "\"schema_version\":2", 0u, 0, 0},
    {"wrong layout_generation", SU_E_SCHEMA_MISMATCH, "\"layout_generation\":1", "\"layout_generation\":2", 0u, 0, 0},
    {"wrong numeric_profile", SU_E_SCHEMA_MISMATCH, "\"numeric_profile\":\"u32_mod_v1\"", "\"numeric_profile\":\"u32_mod_v2\"", 0u, 0, 0},
    {"field renamed", SU_E_SCHEMA_MISMATCH, "{\"id\":1,\"name\":\"position_x\"", "{\"id\":1,\"name\":\"posx_x\"", 0u, 0, 0},
    {"field type not u32", SU_E_TYPE, "\"type\":\"u32\"}", "\"type\":\"u64\"}", 0u, 0, 0},
    {"manifest hash uppercase", SU_E_TYPE, "\"semantic_hash\":\"aaaa", "\"semantic_hash\":\"AAAA", 0u, 0, 0},
    {"manifest hash short", SU_E_TYPE, "\"semantic_hash\":\"a", "\"semantic_hash\":\"", 0u, 0, 0},
    {"entity count short", SU_E_SCHEMA_MISMATCH, "\"entity_ids\":[1,2,3,4]", "\"entity_ids\":[1,2,3]", 0u, 0, 0},
    {"entity id changed", SU_E_SCHEMA_MISMATCH, "\"entity_ids\":[1,2,3,4]", "\"entity_ids\":[1,2,3,5]", 0u, 0, 0},
    {"column length short", SU_E_TYPE, "\"velocity_x\":[1,2,1,0]", "\"velocity_x\":[1,2,1]", 0u, 0, 0},
    {"column value above u32", SU_E_TYPE, "\"velocity_x\":[1,2,1,0]", "\"velocity_x\":[1,2,1,4294967296]", 0u, 0, 0},
    {"column value negative", SU_E_TYPE, "\"velocity_x\":[1,2,1,0]", "\"velocity_x\":[1,2,1,-1]", 0u, 0, 0},
    {"non-canonical whitespace", SU_E_TYPE, "{\"snapshot_format\"", "{ \"snapshot_format\"", 0u, 0, 0},
    {"non-canonical escape", SU_E_TYPE, "\"rev-3\"", "\"\\u0072ev-3\"", 0u, 0, 0},
};

static void check_parse_rejects(const SuWorldDesc *d, const uint8_t *bytes, size_t len, uint32_t want,
                                const char *what) {
    SuSnapshotData data;
    SuSnapshotError err;
    uint32_t st;
    memset(&data, 0, sizeof data);
    st = su_snapshot_parse(bytes, len, d, &data, &err);
    if (st != want) {
        fprintf(stderr, "%s: got %s, want %s\n", what, su_status_name(st), su_status_name(want));
    }
    g_checks++;
    if (st != want) {
        g_failures++;
    }
    /* A rejected parse yields nothing to restore. */
    CHECK(data.columns == NULL && data.manifest == NULL && data.world_revision == NULL);
    su_snapshot_data_free(&data);
}

static void test_corruption(const Example *ex) {
    SuWorld *live = NULL;
    SuWorldDesc d;
    SuSnapshot good;
    size_t dump_len = 0;
    uint8_t *dump;
    size_t i;
    SuWorld *tmp = NULL;

    make_desc(ex, &d);
    CHECK_ST(make_example_world(ex, &live), SU_OK);
    CHECK_ST(run_ticks(live, 3u), SU_OK);
    capture_or_die(live, &d, "rev-3", &good);
    dump = dump_world(live, &dump_len);
    CHECK(dump != NULL);

    /* Clean snapshot parses; a live restore is never attempted on failure. */
    for (i = 0; i < sizeof k_corruptions / sizeof k_corruptions[0]; i++) {
        const Corruption *c = &k_corruptions[i];
        uint8_t *mut = NULL;
        size_t mut_len = 0;
        if (c->oversize) {
            /* Length is over the limit; the parser must refuse before reading. */
            check_parse_rejects(&d, good.bytes, SU_SNAPSHOT_MAX_BYTES + 1u, c->want, c->name);
        } else if (c->empty) {
            check_parse_rejects(&d, good.bytes, 0u, c->want, c->name);
        } else if (c->cut > 0u) {
            check_parse_rejects(&d, good.bytes, good.len - c->cut, c->want, c->name);
        } else if (c->needle == NULL) {
            /* Half-length truncation. */
            check_parse_rejects(&d, good.bytes, good.len / 2u, c->want, c->name);
        } else {
            mut = replace_once(good.bytes, good.len, c->needle, c->repl, &mut_len);
            CHECK(mut != NULL);
            if (mut != NULL) {
                check_parse_rejects(&d, mut, mut_len, c->want, c->name);
                free(mut);
            }
        }
        /* Live world unchanged after every rejected snapshot. */
        CHECK(dump_equal(live, dump, dump_len));
        CHECK(su_world_tick(live) == 3u);
    }

    /* Parsing against a different row count is a schema mismatch. */
    {
        SuWorldDesc small = d;
        small.row_count = 3u;
        check_parse_rejects(&small, good.bytes, good.len, SU_E_SCHEMA_MISMATCH, "row count mismatch");
    }

    /* A failed restore produces no world and leaves the live world alone. */
    {
        SuSnapshotData data;
        SuSnapshotError err;
        size_t mut_len = 0;
        uint8_t *mut = replace_once(good.bytes, good.len, "\"velocity_x\":[1,2,1,0]",
                                    "\"velocity_x\":[1,2,1]", &mut_len);
        CHECK(mut != NULL);
        memset(&data, 0, sizeof data);
        if (mut != NULL) {
            CHECK_ST(su_snapshot_parse(mut, mut_len, &d, &data, &err), SU_E_TYPE);
            /* Restore of a zeroed data struct is refused: no partial world. */
            CHECK_ST(su_snapshot_restore(&d, &data, &tmp, NULL, NULL), SU_E_SCHEMA_MISMATCH);
            CHECK(tmp == NULL);
            free(mut);
        }
        su_snapshot_data_free(&data);
    }
    CHECK(dump_equal(live, dump, dump_len));

    free(dump);
    su_snapshot_destroy(&good);
    su_world_destroy(live);
}

/* ---- restore into a world at a different tick ------------------------- */

static void test_restore_other_tick(const Example *ex) {
    SuWorld *src = NULL;
    SuWorld *other = NULL;
    SuWorld *restored = NULL;
    SuWorldDesc d;
    SuSnapshot snap;
    SuSnapshotData data;
    SuSnapshotError err;
    size_t dl = 0;
    uint8_t *other_dump;
    int eq = 0;

    make_desc(ex, &d);
    CHECK_ST(make_example_world(ex, &src), SU_OK);
    CHECK_ST(run_ticks(src, 1u), SU_OK);
    capture_or_die(src, &d, "rev-1", &snap);
    CHECK(snap.tick == 1u);

    /* Live world at tick 5 with different state. */
    CHECK_ST(make_example_world(ex, &other), SU_OK);
    CHECK_ST(run_ticks(other, 5u), SU_OK);
    other_dump = dump_world(other, &dl);
    CHECK(other_dump != NULL);

    memset(&data, 0, sizeof data);
    CHECK_ST(su_snapshot_parse(snap.bytes, snap.len, &d, &data, &err), SU_OK);
    CHECK_ST(su_snapshot_restore(&d, &data, &restored, NULL, NULL), SU_OK);
    if (restored != NULL) {
        CHECK(su_world_tick(restored) == 1u);
        CHECK_ST(su_world_state_equal(src, restored, &eq), SU_OK);
        CHECK(eq == 1);
        su_world_destroy(restored);
    }
    /* The other world is untouched: restore builds a new world. */
    CHECK(su_world_tick(other) == 5u);
    CHECK(other_dump != NULL && dump_equal(other, other_dump, dl));

    free(other_dump);
    su_snapshot_data_free(&data);
    su_snapshot_destroy(&snap);
    su_world_destroy(src);
    su_world_destroy(other);
}

/* ---- checkpoint store: retention, eviction, release ------------------- */

static void test_checkpoints(const Example *ex) {
    SuCheckpointStore *cs = NULL;
    SuCheckpointStore *def = NULL;
    SuCheckpointStats st;
    SuCheckpointInfo info;
    SuWorld *w = NULL;
    SuWorldDesc d;
    uint64_t ids[5];
    const SuSnapshot *got = NULL;
    uint64_t sum = 0;
    uint32_t i;

    make_desc(ex, &d);
    CHECK_ST(su_checkpoint_store_create(0u, &def), SU_OK);
    CHECK_ST(su_checkpoint_stats(def, &st), SU_OK);
    CHECK(st.retain_limit == SU_CHECKPOINT_DEFAULT_RETAIN);
    CHECK(st.retain_limit == 8u);
    su_checkpoint_store_destroy(def);

    CHECK_ST(su_checkpoint_store_create(SU_CHECKPOINT_MAX_RETAIN + 1u, &def), SU_E_RESOURCE_LIMIT);
    CHECK(def == NULL);
    CHECK_ST(su_checkpoint_store_create(3u, NULL), SU_E_INVALID_ARGUMENT);

    CHECK_ST(su_checkpoint_store_create(3u, &cs), SU_OK);
    CHECK_ST(make_example_world(ex, &w), SU_OK);
    memset(ids, 0, sizeof ids);
    for (i = 0; i < 5u; i++) {
        CHECK_ST(su_checkpoint_create(cs, w, &d, "rev-cp", k_manifest, 2u, &ids[i]), SU_OK);
        CHECK(ids[i] != 0u);
        CHECK_ST(tick_once(w), SU_OK);
    }
    /* Ids are monotonic and unique. */
    CHECK(ids[0] < ids[1] && ids[1] < ids[2] && ids[2] < ids[3] && ids[3] < ids[4]);

    CHECK_ST(su_checkpoint_stats(cs, &st), SU_OK);
    CHECK(st.retain_limit == 3u);
    CHECK(st.retained_count == 3u);
    CHECK(st.created_total == 5u);
    CHECK(st.evicted_total == 2u);
    CHECK(st.overhead_bytes > 0u);

    /* Evicted ids are never served; retained ids are, with their tick. */
    CHECK_ST(su_checkpoint_get(cs, ids[0], &got), SU_E_INVALID_ARGUMENT);
    CHECK(got == NULL);
    CHECK_ST(su_checkpoint_get(cs, ids[1], &got), SU_E_INVALID_ARGUMENT);
    CHECK(got == NULL);
    CHECK_ST(su_checkpoint_get(cs, 0u, &got), SU_E_INVALID_ARGUMENT);
    CHECK(got == NULL);
    CHECK_ST(su_checkpoint_get(cs, ids[2], &got), SU_OK);
    CHECK(got != NULL && got->tick == 2u);
    CHECK_ST(su_checkpoint_get(cs, ids[4], &got), SU_OK);
    CHECK(got != NULL && got->tick == 4u);

    /* Per-checkpoint info, and retained bytes equal the sum of the parts. */
    CHECK_ST(su_checkpoint_stats(cs, &st), SU_OK);
    for (i = 2u; i < 5u; i++) {
        CHECK_ST(su_checkpoint_info(cs, ids[i], &info), SU_OK);
        CHECK(info.id == ids[i]);
        CHECK(info.tick == i);
        CHECK(info.bytes > 0u);
        CHECK(info.capture_ns_known == 1);
        sum += info.bytes;
        /* The reported id is the SHA-256 identity of the stored bytes. */
        CHECK_ST(su_checkpoint_get(cs, ids[i], &got), SU_OK);
        if (got != NULL) {
            SuSha256 ctx;
            uint8_t dig[32];
            char hex[65];
            su_sha256_init(&ctx);
            su_sha256_update(&ctx, got->bytes, got->len);
            su_sha256_final(&ctx, dig);
            su_sha256_hex(dig, hex);
            CHECK(strcmp(hex, info.snapshot_id) == 0);
            CHECK(got->len == info.bytes);
        }
    }
    CHECK(st.retained_bytes == sum);
    CHECK(st.last_capture_ns_known == 1);
    CHECK_ST(su_checkpoint_info(cs, ids[0], &info), SU_E_INVALID_ARGUMENT);

    /* Restore from a retained checkpoint works end to end. */
    {
        SuSnapshotData data;
        SuSnapshotError err;
        SuWorld *r = NULL;
        SuWorld *ref = NULL;
        int eq = 0;
        memset(&data, 0, sizeof data);
        CHECK_ST(su_checkpoint_get(cs, ids[3], &got), SU_OK);
        if (got != NULL) {
            CHECK_ST(su_snapshot_parse(got->bytes, got->len, &d, &data, &err), SU_OK);
            CHECK_ST(su_snapshot_restore(&d, &data, &r, NULL, NULL), SU_OK);
            /* Reference: the example world run to tick 3 from scratch. */
            CHECK_ST(make_example_world(ex, &ref), SU_OK);
            CHECK_ST(run_ticks(ref, 3u), SU_OK);
            if (r != NULL && ref != NULL) {
                CHECK(su_world_tick(r) == 3u);
                CHECK_ST(su_world_state_equal(ref, r, &eq), SU_OK);
                CHECK(eq == 1);
            }
            su_world_destroy(r);
            su_world_destroy(ref);
        }
        su_snapshot_data_free(&data);
    }

    /* Release: returns the slot, later gets fail, double release is safe. */
    CHECK_ST(su_checkpoint_release(cs, ids[3]), SU_OK);
    CHECK_ST(su_checkpoint_get(cs, ids[3], &got), SU_E_INVALID_ARGUMENT);
    CHECK(got == NULL);
    CHECK_ST(su_checkpoint_release(cs, ids[3]), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_checkpoint_release(cs, 9999u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_checkpoint_release(cs, 0u), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_checkpoint_release(NULL, ids[4]), SU_E_INVALID_ARGUMENT);
    CHECK_ST(su_checkpoint_stats(cs, &st), SU_OK);
    CHECK(st.retained_count == 2u);
    CHECK(st.released_total == 1u);

    /* A new checkpoint after a release gets a fresh id and evicts nothing. */
    CHECK_ST(su_checkpoint_create(cs, w, &d, "rev-cp", k_manifest, 2u, &ids[0]), SU_OK);
    CHECK(ids[0] > ids[4]);
    CHECK_ST(su_checkpoint_stats(cs, &st), SU_OK);
    CHECK(st.retained_count == 3u);
    CHECK(st.evicted_total == 2u);

    /* Refused creates store nothing and evict nothing. */
    CHECK_ST(su_world_begin_tick(w), SU_OK);
    {
        uint64_t rid = 77u;
        CHECK_ST(su_checkpoint_create(cs, w, &d, "rev-cp", k_manifest, 2u, &rid), SU_E_INVALID_ARGUMENT);
        CHECK(rid == 0u);
    }
    CHECK_ST(su_world_abort(w), SU_OK);
    CHECK_ST(su_checkpoint_stats(cs, &st), SU_OK);
    CHECK(st.retained_count == 3u);
    CHECK(st.created_total == 6u);

    su_checkpoint_store_destroy(cs);
    su_world_destroy(w);
    /* Destroy with live entries must free them (ASan checks leaks). */
    {
        SuCheckpointStore *leak = NULL;
        SuWorld *lw = NULL;
        uint64_t lid = 0;
        CHECK_ST(su_checkpoint_store_create(0u, &leak), SU_OK);
        CHECK_ST(make_example_world(ex, &lw), SU_OK);
        CHECK_ST(su_checkpoint_create(leak, lw, &d, "rev-cp", k_manifest, 2u, &lid), SU_OK);
        su_checkpoint_store_destroy(leak);
        su_world_destroy(lw);
    }
    su_checkpoint_store_destroy(NULL);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "examples/world.json";
    Example ex;

    init_manifest();
    if (!load_example(path, &ex)) {
        fprintf(stderr, "cannot load example world %s\n", path);
        return 1;
    }
    test_round_trip(&ex);
    test_determinism_and_format(&ex);
    test_capture_refusals(&ex);
    test_corruption(&ex);
    test_restore_other_tick(&ex);
    test_checkpoints(&ex);
    example_free(&ex);

    printf("test_snapshot: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
