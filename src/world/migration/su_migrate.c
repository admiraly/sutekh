/* Schema migration engine (M2-01a). See su_migrate.h and planning/contracts/M2_MIGRATION_CONTRACT.md. */
#define _POSIX_C_SOURCE 200809L

#include "world/migration/su_migrate.h"
#include "platform/su_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "ir/su_sha256.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "vm/su_vm.h"
#include "world/entities/su_ents.h"

#define MIG_FORMAT "0.1"
#define XFORM_FORMAT "0.1"
#define MIG_SYNTH_PHASE "migrate"
#define MIG_TRANSFORM_SYSTEM_DEFAULT "migration.transform"
/* Estimate constants (bytes): decimal digits and separators per value, and per-slot record overhead. */
#define EST_VALUE_BYTES 11u
#define EST_SLOT_BYTES 48u
#define EST_HEADER_BYTES 4096u

struct SuMigrateCompiled {
    char *schema_id;
    uint32_t schema_version;
    uint32_t from_gen;
    uint32_t to_gen;
    uint32_t dst_n;
    SuWorldFieldDesc *dst;      /* destination schema, ascending id; names owned */
    uint32_t *dst_def;          /* default per destination field */
    uint32_t src_n;
    uint32_t *src_ids;          /* source schema IDs, ascending */
    uint32_t retired_n;
    uint32_t *retired;          /* ascending, old retired IDs plus the IDs removed by this migration */
    uint32_t lineage_n;
    uint8_t (*lineage)[32];     /* old lineage plus this migration's hash */
    int has_transform;
    uint32_t writes_n;
    uint32_t *writes;           /* ascending destination IDs */
    uint8_t *lir;               /* owned copy of the transform IR */
    size_t lir_len;
    uint8_t *capsule;           /* synthesized capsule, owned */
    size_t capsule_len;
    uint64_t pre_tick;
    uint8_t pre_digest[32];     /* recovery digest of the old world at plan time */
    uint8_t after_digest[32];   /* schema digest of the destination */
};

/* ---- small helpers ----------------------------------------------------- */

typedef struct Sb {
    char *p;
    size_t len;
    size_t cap;
    int oom;
} Sb;

/* ---- failure injection (N2) -----------------------------------------------
 * Every allocation the engine makes in plan and prepare is an injection point, counted through the hooks of the
 * call in progress (SuMigrateHooks; NULL counts nothing and never fails). An armed point makes the allocation
 * return NULL before it is made, so the caller's own out-of-memory path runs. The engine is single-threaded per call
 * (migrations run in the world's worker, outside ticks), so the hooks of one call are held in g_alloc_hooks for its
 * duration and cleared on return. */
static SuMigrateHooks *g_alloc_hooks = NULL;

static int inject_point(SuMigrateHooks *h) {
    if (h == NULL) {
        return 0;
    }
    h->points++;
    return h->fail_at != 0u && h->points == h->fail_at;
}

static void *mig_malloc(size_t n) {
    return inject_point(g_alloc_hooks) ? NULL : malloc(n);
}

static void *mig_calloc(size_t n, size_t m) {
    return inject_point(g_alloc_hooks) ? NULL : calloc(n, m);
}

static void *mig_realloc(void *p, size_t n) {
    return inject_point(g_alloc_hooks) ? NULL : realloc(p, n);
}

static void sb_put(Sb *b, const char *s, size_t n) {
    if (b->oom) {
        return;
    }
    if (b->len + n + 1u > b->cap) {
        size_t nc = b->cap > 0u ? b->cap : 256u;
        char *np;
        while (nc < b->len + n + 1u) {
            nc *= 2u;
        }
        np = mig_realloc(b->p, nc);
        if (np == NULL) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_lit(Sb *b, const char *s) {
    sb_put(b, s, strlen(s));
}

static void sb_u(Sb *b, uint64_t v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    sb_put(b, t, (size_t)n);
}

/* JSON string of n bytes (escapes quote, backslash and control bytes). */
static void sb_jstr_n(Sb *b, const char *s, size_t n) {
    size_t i;
    sb_put(b, "\"", 1u);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            sb_put(b, "\\", 1u);
            sb_put(b, s + i, 1u);
        } else if (c < 0x20u) {
            char t[8];
            snprintf(t, sizeof t, "\\u%04x", (unsigned)c);
            sb_put(b, t, 6u);
        } else {
            sb_put(b, s + i, 1u);
        }
    }
    sb_put(b, "\"", 1u);
}

static void sb_jstr(Sb *b, const char *s) {
    sb_jstr_n(b, s, strlen(s));
}

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64le(uint8_t *p, uint64_t v) {
    put_u32le(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32le(p + 4, (uint32_t)(v >> 32));
}

static uint64_t now_ns(void) {
    uint64_t ns = 0;
    (void)su_platform_monotonic_ns(&ns); /* 0 when the clock is unreadable */
    return ns;
}

static void set_message(SuMigratePlan *p, const char *msg) {
    size_t n = strlen(msg);
    if (n >= sizeof p->message) {
        n = sizeof p->message - 1u;
    }
    memcpy(p->message, msg, n);
    p->message[n] = '\0';
}

/* Identifier rule of the capsule verifier: [A-Za-z_][A-Za-z0-9_]*, 1..255 bytes. */
static int ident_ok(const char *s, size_t n) {
    size_t i;
    if (n == 0u || n > 255u) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        int digit = c >= '0' && c <= '9';
        if (!(alpha || (i > 0u && digit))) {
            return 0;
        }
    }
    return 1;
}

/* Document identifiers: [A-Za-z0-9._-], 1..max bytes. */
static int doc_id_ok(const char *s, size_t n, size_t max) {
    size_t i;
    if (n == 0u || n > max) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                 c == '_' || c == '-' || c == '/';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static int str_is(const SuJsonValue *v, const char *lit) {
    size_t n = strlen(lit);
    return v != NULL && v->type == SU_JSON_STRING && v->u.string.len == n &&
           memcmp(v->u.string.data, lit, n) == 0;
}

static int is_string(const SuJsonValue *v) {
    return v != NULL && v->type == SU_JSON_STRING;
}

static uint32_t get_u32v(const SuJsonValue *v, uint32_t *out) {
    uint64_t x = 0;
    if (v == NULL || !su_json_get_uint(v, &x) || x > UINT32_MAX) {
        return SU_E_TYPE;
    }
    *out = (uint32_t)x;
    return SU_OK;
}

static int key_eq(const SuJsonMember *m, const char *lit) {
    size_t n = strlen(lit);
    return n == m->key_len && memcmp(m->key, lit, n) == 0;
}

/* Object whose members are exactly the given keys (any order). */
static uint32_t exact_keys(const SuJsonValue *obj, const char *const *keys, size_t nkeys) {
    size_t i;
    size_t k;
    if (obj == NULL || obj->type != SU_JSON_OBJECT || obj->u.object.count != nkeys) {
        return SU_E_TYPE;
    }
    for (i = 0; i < obj->u.object.count; i++) {
        int found = 0;
        for (k = 0; k < nkeys && !found; k++) {
            found = key_eq(&obj->u.object.members[i], keys[k]);
        }
        if (!found) {
            return SU_E_TYPE;
        }
    }
    return SU_OK;
}

static uint32_t fail_plan(SuMigratePlan *p, uint32_t st, const char *msg) {
    if (p != NULL) {
        set_message(p, msg);
    }
    return st;
}

/* ---- schema digest (contract section 1; identical to su_world_schema_digest) ---- */

static void schema_digest_of(const SuWorldFieldDesc *f, const uint32_t *def, uint32_t n, uint8_t out[32]) {
    SuSha256 ctx;
    uint8_t hdr[18];
    uint8_t rec[16];
    uint32_t i;
    memcpy(hdr, "SUWSCHEMA", 9u);
    hdr[9] = 0u;
    put_u32le(hdr + 10, 1u);
    put_u32le(hdr + 14, n);
    su_sha256_init(&ctx);
    su_sha256_update(&ctx, hdr, sizeof hdr);
    for (i = 0; i < n; i++) {
        size_t len = strlen(f[i].name);
        put_u32le(rec, f[i].id);
        put_u32le(rec + 4, f[i].type);
        put_u32le(rec + 8, def[i]);
        put_u32le(rec + 12, (uint32_t)len);
        su_sha256_update(&ctx, rec, sizeof rec);
        su_sha256_update(&ctx, f[i].name, len);
    }
    su_sha256_final(&ctx, out);
}

/* ---- failure injection --------------------------------------------------- */

#define MIG_POINT(h)                                 \
    do {                                             \
        if (inject_point(h)) {                       \
            st = SU_E_RESOURCE_LIMIT;                \
            goto done;                               \
        }                                            \
    } while (0)

/* ---- working schema during compilation ----------------------------------- */

typedef struct WField {
    uint32_t id;
    uint32_t def;
    uint32_t access;
    const char *name;   /* borrowed: the world's or the migration document's */
    uint32_t in_src;
    uint32_t renamed;
} WField;

typedef struct Compile {
    WField f[SU_LIMIT_FIELDS + 1u];
    uint32_t n;
    uint32_t *removed;      /* IDs removed by this migration, in order */
    uint32_t removed_n;
    uint32_t removed_cap;
    int has_transform;
    const char *xform_source;
    size_t xform_source_len;
} Compile;

static long wf_find(const Compile *c, uint32_t id) {
    uint32_t i;
    for (i = 0; i < c->n; i++) {
        if (c->f[i].id == id) {
            return (long)i;
        }
    }
    return -1;
}

static int wf_name_used(const Compile *c, const char *name, size_t len) {
    uint32_t i;
    for (i = 0; i < c->n; i++) {
        if (strlen(c->f[i].name) == len && memcmp(c->f[i].name, name, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int id_in_list(const uint32_t *list, uint32_t n, uint32_t id) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (list[i] == id) {
            return 1;
        }
    }
    return 0;
}

/* ---- migration document: shape ------------------------------------------- */

static const char *const k_mig_keys[] = {"migration_format", "id", "from_layout_generation",
                                         "to_layout_generation", "ops"};
static const char *const k_op_add_keys[] = {"op", "field"};
static const char *const k_op_add_field_keys[] = {"id", "name", "type", "default"};
static const char *const k_op_rename_keys[] = {"op", "id", "new_name"};
static const char *const k_op_remove_keys[] = {"op", "id"};
static const char *const k_op_transform_keys[] = {"op", "document"};
static const char *const k_xf_keys[] = {"transform_format", "source", "writes"};

typedef enum OpKind {
    OP_ADD = 1,
    OP_RENAME,
    OP_REMOVE,
    OP_TRANSFORM,
    OP_UNKNOWN
} OpKind;

static OpKind op_kind(const SuJsonValue *op_name) {
    if (str_is(op_name, "add_field")) {
        return OP_ADD;
    }
    if (str_is(op_name, "rename_field")) {
        return OP_RENAME;
    }
    if (str_is(op_name, "remove_field")) {
        return OP_REMOVE;
    }
    if (str_is(op_name, "transform")) {
        return OP_TRANSFORM;
    }
    return OP_UNKNOWN;
}

/* Shape of one op (keys and JSON types). Unknown op names are a semantic refusal, checked later. */
static uint32_t shape_op(const SuJsonValue *op) {
    const SuJsonValue *name;
    OpKind k;
    uint32_t id = 0;
    if (op == NULL || op->type != SU_JSON_OBJECT) {
        return SU_E_TYPE;
    }
    name = su_json_member(op, "op");
    if (!is_string(name)) {
        return SU_E_TYPE;
    }
    k = op_kind(name);
    switch (k) {
    case OP_ADD: {
        const SuJsonValue *f;
        const SuJsonValue *nm;
        const SuJsonValue *ty;
        uint32_t st;
        if (exact_keys(op, k_op_add_keys, 2u) != SU_OK) {
            return SU_E_TYPE;
        }
        f = su_json_member(op, "field");
        if (exact_keys(f, k_op_add_field_keys, 4u) != SU_OK) {
            return SU_E_TYPE;
        }
        st = get_u32v(su_json_member(f, "id"), &id);
        nm = su_json_member(f, "name");
        ty = su_json_member(f, "type");
        if (st != SU_OK || !is_string(nm) || !is_string(ty) || !str_is(ty, "u32") ||
            get_u32v(su_json_member(f, "default"), &id) != SU_OK) {
            return SU_E_TYPE;
        }
        return SU_OK;
    }
    case OP_RENAME:
        if (exact_keys(op, k_op_rename_keys, 3u) != SU_OK || get_u32v(su_json_member(op, "id"), &id) != SU_OK ||
            !is_string(su_json_member(op, "new_name"))) {
            return SU_E_TYPE;
        }
        return SU_OK;
    case OP_REMOVE:
        if (exact_keys(op, k_op_remove_keys, 2u) != SU_OK || get_u32v(su_json_member(op, "id"), &id) != SU_OK) {
            return SU_E_TYPE;
        }
        return SU_OK;
    case OP_TRANSFORM:
        if (exact_keys(op, k_op_transform_keys, 2u) != SU_OK || !is_string(su_json_member(op, "document"))) {
            return SU_E_TYPE;
        }
        return SU_OK;
    default:
        return SU_OK; /* unknown op: refused in the semantic stage */
    }
}

/* ---- transform document ---------------------------------------------------- */

typedef struct Xform {
    uint32_t writes_n;
    uint32_t *writes;   /* ascending, unique */
    const char *source;
    size_t source_len;
    SuJsonDoc doc;
} Xform;

static void xform_free(Xform *x) {
    free(x->writes);
    x->writes = NULL;
    su_json_doc_free(&x->doc);
}

/* Parses the transform document: limits, parse, duplicate keys, shape. Its writes are checked against the
 * destination schema by the caller (semantic stage). */
static uint32_t parse_xform(const uint8_t *bytes, size_t len, Xform *x, SuMigratePlan *p) {
    SuJsonLimits lim;
    SuJsonError err;
    const SuJsonValue *root;
    const SuJsonValue *src;
    const SuJsonValue *wr;
    uint32_t st;
    uint32_t i;
    memset(x, 0, sizeof *x);
    if (len > SU_MIGRATE_DOC_MAX_BYTES) {
        return fail_plan(p, SU_E_RESOURCE_LIMIT, "transform document exceeds the size limit");
    }
    lim.max_bytes = len;
    lim.max_depth = SU_MIGRATE_MAX_DEPTH;
    st = su_json_parse(bytes, len, &lim, &x->doc, &err);
    if (st != SU_OK) {
        fail_plan(p, st, err.message);
        su_json_doc_free(&x->doc);
        return st;
    }
    root = x->doc.root;
    if (exact_keys(root, k_xf_keys, 3u) != SU_OK || !str_is(su_json_member(root, "transform_format"), XFORM_FORMAT)) {
        return fail_plan(p, SU_E_TYPE, "transform document shape is not transform_format 0.1");
    }
    src = su_json_member(root, "source");
    if (!is_string(src) || !doc_id_ok(src->u.string.data, src->u.string.len, 255u)) {
        return fail_plan(p, SU_E_TYPE, "transform source must be a plain file name");
    }
    x->source = src->u.string.data;
    x->source_len = src->u.string.len;
    wr = su_json_member(root, "writes");
    if (wr == NULL || wr->type != SU_JSON_ARRAY || wr->u.array.count == 0u || wr->u.array.count > SU_LIMIT_FIELDS) {
        return fail_plan(p, SU_E_TYPE, "writes must be a non-empty array of field IDs");
    }
    x->writes_n = (uint32_t)wr->u.array.count;
    x->writes = mig_malloc(x->writes_n * sizeof *x->writes);
    if (x->writes == NULL) {
        return fail_plan(p, SU_E_RESOURCE_LIMIT, "out of memory reading writes");
    }
    for (i = 0; i < x->writes_n; i++) {
        uint32_t id = 0;
        if (get_u32v(wr->u.array.items[i], &id) != SU_OK || id == 0u) {
            return fail_plan(p, SU_E_TYPE, "writes entries must be non-zero u32 field IDs");
        }
        x->writes[i] = id;
    }
    {
        uint32_t a;
        uint32_t b;
        for (a = 1; a < x->writes_n; a++) {
            uint32_t v = x->writes[a];
            for (b = a; b > 0u && x->writes[b - 1u] > v; b--) {
                x->writes[b] = x->writes[b - 1u];
                x->writes[b - 1u] = v;
            }
        }
        for (a = 1; a < x->writes_n; a++) {
            if (x->writes[a] == x->writes[a - 1u]) {
                return fail_plan(p, SU_E_TYPE, "writes must not repeat a field ID");
            }
        }
    }
    return SU_OK;
}

/* ---- capsule synthesis -------------------------------------------------- */

typedef struct CapEntry {
    uint32_t id;
    const char *name;
    int read;
    int write;
} CapEntry;

/* Builds the synthesized capsule: union of the source and destination field tables, system and profile taken
 * from the IR document, schema from the world. Returns a malloc'd buffer in *out. */
static uint32_t build_capsule(const char *sid, uint32_t version, uint32_t gen, const char *system_id,
                              size_t system_len, const char *profile, size_t profile_len,
                              const char *source, size_t source_len, const CapEntry *e, uint32_t n,
                              uint8_t **out, size_t *out_len) {
    Sb b;
    uint32_t i;
    uint32_t j;
    memset(&b, 0, sizeof b);
    sb_lit(&b, "{\"capsule_format\":\"0.1\",\"system_id\":");
    sb_jstr_n(&b, system_id, system_len);
    sb_lit(&b, ",\"source\":");
    sb_jstr_n(&b, source, source_len);
    sb_lit(&b, ",\"schema\":{\"id\":");
    sb_jstr(&b, sid);
    sb_lit(&b, ",\"version\":");
    sb_u(&b, version > 0u ? version : 1u);
    sb_lit(&b, ",\"layout_generation\":");
    sb_u(&b, gen);
    sb_lit(&b, "},\"numeric_profile\":");
    sb_jstr_n(&b, profile, profile_len);
    sb_lit(&b, ",\"fields\":[");
    for (i = 0; i < n; i++) {
        if (i > 0u) {
            sb_lit(&b, ",");
        }
        sb_lit(&b, "{\"id\":");
        sb_u(&b, e[i].id);
        sb_lit(&b, ",\"name\":");
        sb_jstr(&b, e[i].name);
        sb_lit(&b, ",\"type\":\"u32\",\"read\":");
        sb_lit(&b, e[i].read ? "true" : "false");
        sb_lit(&b, ",\"write\":");
        sb_lit(&b, e[i].write ? "true" : "false");
        sb_lit(&b, "}");
    }
    sb_lit(&b, "],\"schedule\":{\"phase\":\"" MIG_SYNTH_PHASE "\",\"after\":[],\"before\":[],\"parallel\":\"serial\"},");
    sb_lit(&b, "\"limits\":{\"max_instructions\":4096,\"max_rows\":");
    sb_u(&b, SU_LIMIT_ROWS);
    sb_lit(&b, ",\"allocation\":\"forbidden\"},\"tests\":[\"migration.synthetic\"]}");
    (void)j;
    if (b.oom) {
        free(b.p);
        return SU_E_RESOURCE_LIMIT;
    }
    *out = (uint8_t *)b.p;
    *out_len = b.len;
    return SU_OK;
}

/* ---- plan ------------------------------------------------------------------ */

/* Reads the IR header fields needed to synthesize the capsule. */
static uint32_t lir_header(const uint8_t *lir, size_t len, SuJsonDoc *doc, const SuJsonValue **sid,
                           const SuJsonValue **prof) {
    SuJsonLimits lim;
    SuJsonError err;
    uint32_t st;
    lim.max_bytes = len;
    lim.max_depth = SU_LIMIT_JSON_DEPTH;
    st = su_json_parse(lir, len, &lim, doc, &err);
    if (st != SU_OK) {
        return SU_E_CANDIDATE_FAILED;
    }
    *sid = su_json_member(doc->root, "system_id");
    *prof = su_json_member(doc->root, "numeric_profile");
    if (!is_string(*sid) || !is_string(*prof)) {
        return SU_E_CANDIDATE_FAILED;
    }
    return SU_OK;
}

static void compile_free(SuMigrateCompiled *c) {
    uint32_t i;
    if (c == NULL) {
        return;
    }
    if (c->dst != NULL) {
        for (i = 0; i < c->dst_n; i++) {
            free((void *)c->dst[i].name);
        }
    }
    free(c->dst);
    free(c->dst_def);
    free(c->schema_id);
    free(c->src_ids);
    free(c->retired);
    free(c->lineage);
    free(c->writes);
    free(c->lir);
    free(c->capsule);
    free(c);
}

void su_migrate_plan_free(SuMigratePlan *p) {
    if (p == NULL) {
        return;
    }
    compile_free(p->impl);
    free(p->systems);
    memset(p, 0, sizeof *p);
}

static uint64_t est_bytes(uint64_t hw, uint64_t fields) {
    return hw * ((fields + 1u) * EST_VALUE_BYTES + EST_SLOT_BYTES) + EST_HEADER_BYTES;
}

static uint32_t plan_body(const SuWorld *w, const SuMigrateInputs *in,
                          const SuMigrateSystem *systems, uint32_t system_count,
                          const char *const *drop_systems, uint32_t drop_count,
                          const SuMigrateBudget *budget, SuMigratePlan *out);

uint32_t su_migrate_plan_inject(const SuWorld *w, const SuMigrateInputs *in,
                                const SuMigrateSystem *systems, uint32_t system_count,
                                const char *const *drop_systems, uint32_t drop_count,
                                const SuMigrateBudget *budget, SuMigrateHooks *hooks, SuMigratePlan *out) {
    uint32_t st;
    g_alloc_hooks = hooks;
    st = plan_body(w, in, systems, system_count, drop_systems, drop_count, budget, out);
    g_alloc_hooks = NULL;
    return st;
}

uint32_t su_migrate_plan(const SuWorld *w, const SuMigrateInputs *in,
                         const SuMigrateSystem *systems, uint32_t system_count,
                         const char *const *drop_systems, uint32_t drop_count,
                         const SuMigrateBudget *budget, SuMigratePlan *out) {
    return su_migrate_plan_inject(w, in, systems, system_count, drop_systems, drop_count, budget, NULL, out);
}

static uint32_t plan_body(const SuWorld *w, const SuMigrateInputs *in,
                          const SuMigrateSystem *systems, uint32_t system_count,
                          const char *const *drop_systems, uint32_t drop_count,
                          const SuMigrateBudget *budget, SuMigratePlan *out) {
    SuJsonLimits lim;
    SuJsonDoc mdoc;
    SuJsonError jerr;
    const SuJsonValue *root;
    const SuJsonValue *ops;
    const SuJsonValue *idv;
    SuMigrateCompiled *c = NULL;
    Compile cc;
    Xform xf;
    SuDiag diag;
    SuLirModule mod;
    CapEntry *ent = NULL;
    uint8_t *capsule = NULL;
    size_t capsule_len = 0;
    SuJsonDoc ldoc;
    const SuJsonValue *lsid = NULL;
    const SuJsonValue *lprof = NULL;
    uint32_t src_n;
    uint32_t hw_ret_n = 0;
    const uint32_t *old_ret = NULL;
    uint32_t i;
    uint32_t k;
    uint32_t st;
    uint32_t from_gen;
    uint32_t to_gen;
    uint64_t hw;
    uint64_t cap;
    uint64_t s_rows;
    uint64_t bytes_new;
    uint64_t scratch;
    uint64_t staging_fields = 0;
    uint32_t n_broken = 0;
    uint32_t n_cap = 0;
    SuWorldFieldDesc *srcf = NULL;
    uint32_t *srcdef = NULL;
    SuSha256 hc;
    const char *sid_name;
    size_t sid_len;
    int have_xf = 0;

    if (out == NULL || w == NULL || in == NULL || in->migration == NULL || budget == NULL ||
        (system_count > 0u && systems == NULL) || (drop_count > 0u && drop_systems == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    memset(&cc, 0, sizeof cc);
    memset(&xf, 0, sizeof xf);
    memset(&mdoc, 0, sizeof mdoc);
    memset(&ldoc, 0, sizeof ldoc);
    memset(&mod, 0, sizeof mod);
    su_diag_init(&diag);

    /* Stage 1-3 (limits, syntax, duplicate keys) and stage 4 (shape) of the migration document. */
    if (in->migration_len > SU_MIGRATE_DOC_MAX_BYTES) {
        return fail_plan(out, SU_E_RESOURCE_LIMIT, "migration document exceeds the size limit");
    }
    lim.max_bytes = in->migration_len;
    lim.max_depth = SU_MIGRATE_MAX_DEPTH;
    st = su_json_parse(in->migration, in->migration_len, &lim, &mdoc, &jerr);
    if (st != SU_OK) {
        fail_plan(out, st, jerr.message);
        su_json_doc_free(&mdoc);
        return st;
    }
    root = mdoc.root;
    if (exact_keys(root, k_mig_keys, 5u) != SU_OK) {
        st = fail_plan(out, SU_E_TYPE, "migration document keys are not exactly the 0.1 set");
        goto done;
    }
    if (!str_is(su_json_member(root, "migration_format"), MIG_FORMAT)) {
        st = fail_plan(out, SU_E_TYPE, "migration_format must be 0.1");
        goto done;
    }
    idv = su_json_member(root, "id");
    if (!is_string(idv) || !doc_id_ok(idv->u.string.data, idv->u.string.len, SU_MIGRATE_ID_MAX)) {
        st = fail_plan(out, SU_E_TYPE, "id must be 1..64 characters of [A-Za-z0-9._-]");
        goto done;
    }
    if (get_u32v(su_json_member(root, "from_layout_generation"), &from_gen) != SU_OK ||
        get_u32v(su_json_member(root, "to_layout_generation"), &to_gen) != SU_OK) {
        st = fail_plan(out, SU_E_TYPE, "layout generations must be u32 integers");
        goto done;
    }
    if (from_gen == 0u || to_gen != from_gen + 1u) {
        st = fail_plan(out, SU_E_TYPE, "to_layout_generation must be from_layout_generation + 1 (from >= 1)");
        goto done;
    }
    ops = su_json_member(root, "ops");
    if (ops == NULL || ops->type != SU_JSON_ARRAY) {
        st = fail_plan(out, SU_E_TYPE, "ops must be an array");
        goto done;
    }
    if (ops->u.array.count > SU_MIGRATE_MAX_OPS) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "more than 64 ops");
        goto done;
    }
    for (i = 0; i < ops->u.array.count; i++) {
        st = shape_op(ops->u.array.items[i]);
        if (st != SU_OK) {
            st = fail_plan(out, st, "op has the wrong keys or JSON types");
            goto done;
        }
    }

    /* Stage 5: semantics. World identity and the generation first. */
    if (su_world_in_tick(w)) {
        st = fail_plan(out, SU_E_INVALID_ARGUMENT, "world is inside a tick");
        goto done;
    }
    if (from_gen != su_world_layout_generation(w)) {
        st = fail_plan(out, SU_E_STALE_REVISION, "from_layout_generation differs from the world");
        goto done;
    }

    /* Source schema (ascending by id, as the world stores it). */
    src_n = su_world_field_count(w);
    if (src_n > SU_LIMIT_FIELDS) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "schema exceeds the field limit");
        goto done;
    }
    srcf = mig_calloc(src_n > 0u ? src_n : 1u, sizeof *srcf);
    srcdef = mig_calloc(src_n > 0u ? src_n : 1u, sizeof *srcdef);
    if (srcf == NULL || srcdef == NULL) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
        goto done;
    }
    for (i = 0; i < src_n; i++) {
        if (su_world_field_at(w, i, &srcf[i], &srcdef[i]) != SU_OK) {
            st = SU_E_INTERNAL;
            goto done;
        }
    }
    cc.n = src_n;
    for (i = 0; i < src_n; i++) {
        cc.f[i].id = srcf[i].id;
        cc.f[i].def = srcdef[i];
        cc.f[i].access = srcf[i].access;
        cc.f[i].name = srcf[i].name;
        cc.f[i].in_src = 1u;
    }
    if (su_world_retired_ids(w, &old_ret, &hw_ret_n) != SU_OK) {
        st = SU_E_INTERNAL;
        goto done;
    }
    /* old_ret points into the world; copy it into the compile-time removed list (grown below). */
    cc.removed_cap = hw_ret_n + SU_MIGRATE_MAX_OPS;
    cc.removed = mig_malloc(cc.removed_cap * sizeof *cc.removed);
    if (cc.removed == NULL) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
        goto done;
    }
    if (hw_ret_n > 0u) {
        memcpy(cc.removed, old_ret, hw_ret_n * sizeof *cc.removed);
    }
    cc.removed_n = hw_ret_n; /* old retired IDs first; doc-removed IDs are appended after them */
    old_ret = NULL;

    for (i = 0; i < ops->u.array.count; i++) {
        const SuJsonValue *op = ops->u.array.items[i];
        OpKind kind = op_kind(su_json_member(op, "op"));
        if (kind == OP_UNKNOWN) {
            st = fail_plan(out, SU_E_UNSUPPORTED_MIGRATION, "unsupported op");
            goto done;
        }
        if (kind == OP_ADD) {
            const SuJsonValue *f = su_json_member(op, "field");
            uint32_t id = 0;
            uint32_t def = 0;
            const SuJsonValue *nm = su_json_member(f, "name");
            (void)get_u32v(su_json_member(f, "id"), &id);
            (void)get_u32v(su_json_member(f, "default"), &def);
            if (id == 0u) {
                st = fail_plan(out, SU_E_TYPE, "add_field id must be non-zero");
                goto done;
            }
            if (wf_find(&cc, id) >= 0 || id_in_list(cc.removed, cc.removed_n, id)) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "add_field id is live or retired");
                goto done;
            }
            if (!ident_ok(nm->u.string.data, nm->u.string.len) || wf_name_used(&cc, nm->u.string.data, nm->u.string.len)) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "add_field name is malformed or already used");
                goto done;
            }
            if (cc.n >= SU_LIMIT_FIELDS) {
                st = fail_plan(out, SU_E_RESOURCE_LIMIT, "schema would exceed the field limit");
                goto done;
            }
            cc.f[cc.n].id = id;
            cc.f[cc.n].def = def;
            cc.f[cc.n].access = SU_ACCESS_READ;
            cc.f[cc.n].name = nm->u.string.data;
            cc.f[cc.n].in_src = 0u;
            cc.f[cc.n].renamed = 0u;
            cc.n++;
        } else if (kind == OP_RENAME) {
            const SuJsonValue *nm = su_json_member(op, "new_name");
            uint32_t id = 0;
            long at;
            (void)get_u32v(su_json_member(op, "id"), &id);
            at = wf_find(&cc, id);
            if (at < 0) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "rename_field id is not a live field");
                goto done;
            }
            if (!ident_ok(nm->u.string.data, nm->u.string.len) || wf_name_used(&cc, nm->u.string.data, nm->u.string.len)) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "rename_field name is malformed or already used");
                goto done;
            }
            cc.f[at].name = nm->u.string.data;
            cc.f[at].renamed = 1u;
        } else if (kind == OP_REMOVE) {
            uint32_t id = 0;
            long at;
            uint32_t j;
            (void)get_u32v(su_json_member(op, "id"), &id);
            at = wf_find(&cc, id);
            if (at < 0) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "remove_field id is not a live field");
                goto done;
            }
            if (cc.removed_n >= cc.removed_cap) {
                st = fail_plan(out, SU_E_RESOURCE_LIMIT, "retired IDs exceed the history limit");
                goto done;
            }
            cc.removed[cc.removed_n++] = id;
            for (j = (uint32_t)at + 1u; j < cc.n; j++) {
                cc.f[j - 1u] = cc.f[j];
            }
            cc.n--;
        } else { /* OP_TRANSFORM */
            if (have_xf) {
                st = fail_plan(out, SU_E_UNSUPPORTED_MIGRATION, "more than one transform op");
                goto done;
            }
            have_xf = 1;
        }
    }
    if (cc.removed_n > SU_WORLD_MAX_HISTORY) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "retired IDs exceed the history limit");
        goto done;
    }

    /* Destination schema: sort ascending by id and copy the names. */
    for (i = 1; i < cc.n; i++) {
        WField t = cc.f[i];
        for (k = i; k > 0u && cc.f[k - 1u].id > t.id; k--) {
            cc.f[k] = cc.f[k - 1u];
        }
        cc.f[k] = t;
    }
    for (i = 1; i < cc.n; i++) {
        if (cc.f[i].id == cc.f[i - 1u].id) {
            st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "duplicate field ID in the destination schema");
            goto done;
        }
    }

    c = mig_calloc(1u, sizeof *c);
    if (c == NULL) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
        goto done;
    }
    out->impl = c;
    c->from_gen = from_gen;
    c->to_gen = to_gen;
    c->schema_version = su_world_schema_version(w);
    c->dst_n = cc.n;
    c->dst = mig_calloc(cc.n > 0u ? cc.n : 1u, sizeof *c->dst);
    c->dst_def = mig_calloc(cc.n > 0u ? cc.n : 1u, sizeof *c->dst_def);
    if (c->dst == NULL || c->dst_def == NULL) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
        goto done;
    }
    for (i = 0; i < cc.n; i++) {
        size_t nl = strlen(cc.f[i].name);
        char *nm = mig_malloc(nl + 1u);
        if (nm == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
        memcpy(nm, cc.f[i].name, nl + 1u);
        c->dst[i].id = cc.f[i].id;
        c->dst[i].type = SU_TYPE_U32;
        c->dst[i].access = cc.f[i].access;
        c->dst[i].name = nm;
        c->dst_def[i] = cc.f[i].def;
    }
    {
        const char *sid = su_world_schema_id(w);
        c->schema_id = mig_malloc(strlen(sid) + 1u);
        if (c->schema_id == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
        strcpy(c->schema_id, sid);
    }
    sid_name = c->schema_id;
    sid_len = strlen(sid_name);
    (void)sid_len;

    /* Retired IDs: old ones plus the ones this migration removes (ascending, unique). */
    c->retired_n = cc.removed_n;
    c->retired = mig_calloc(cc.removed_n > 0u ? cc.removed_n : 1u, sizeof *c->retired);
    if (c->retired == NULL) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
        goto done;
    }
    if (cc.removed_n > 0u) {
        memcpy(c->retired, cc.removed, cc.removed_n * sizeof *c->retired);
        for (i = 1; i < c->retired_n; i++) {
            uint32_t t = c->retired[i];
            for (k = i; k > 0u && c->retired[k - 1u] > t; k--) {
                c->retired[k] = c->retired[k - 1u];
            }
            c->retired[k] = t;
        }
        for (i = 1; i < c->retired_n; i++) {
            if (c->retired[i] == c->retired[i - 1u]) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "retired field IDs repeat");
                goto done;
            }
        }
    }

    /* Source IDs (ascending) for membership and copies. */
    c->src_n = src_n;
    c->src_ids = mig_calloc(src_n > 0u ? src_n : 1u, sizeof *c->src_ids);
    if (c->src_ids == NULL) {
        st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
        goto done;
    }
    for (i = 0; i < src_n; i++) {
        c->src_ids[i] = srcf[i].id;
    }

    /* Transform: parse, check writes, synthesize and verify (contract section 2). */
    if (have_xf) {
        if (in->transform == NULL) {
            st = fail_plan(out, SU_E_TYPE, "migration has a transform op but no transform document was given");
            goto done;
        }
        if (in->lir == NULL) {
            st = fail_plan(out, SU_E_TYPE, "transform document given without its IR");
            goto done;
        }
        st = parse_xform(in->transform, in->transform_len, &xf, out);
        if (st != SU_OK) {
            goto done;
        }
        for (i = 0; i < xf.writes_n; i++) {
            long at = wf_find(&cc, xf.writes[i]);
            if (at < 0) {
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "transform writes an ID that is not in the destination schema");
                goto done;
            }
        }
        c->has_transform = 1;
        c->writes_n = xf.writes_n;
        c->writes = mig_malloc(xf.writes_n * sizeof *c->writes);
        if (c->writes == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
        memcpy(c->writes, xf.writes, xf.writes_n * sizeof *c->writes);
        c->lir = mig_malloc(in->lir_len > 0u ? in->lir_len : 1u);
        if (c->lir == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
        memcpy(c->lir, in->lir, in->lir_len);
        c->lir_len = in->lir_len;

        st = lir_header(in->lir, in->lir_len, &ldoc, &lsid, &lprof);
        if (st != SU_OK) {
            fail_plan(out, st, "transform IR is not valid JSON or lacks system_id or numeric_profile");
            goto done;
        }
        /* Union of the source and destination tables. A field's name is its destination name when it exists. */
        ent = mig_calloc(src_n + cc.n + 1u, sizeof *ent);
        if (ent == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
        for (i = 0; i < src_n; i++) {
            long at = wf_find(&cc, srcf[i].id);
            ent[n_cap].id = srcf[i].id;
            ent[n_cap].name = at >= 0 ? cc.f[at].name : srcf[i].name;
            ent[n_cap].read = 1;
            ent[n_cap].write = id_in_list(xf.writes, xf.writes_n, srcf[i].id);
            n_cap++;
        }
        for (i = 0; i < cc.n; i++) {
            int in_src = 0;
            uint32_t j;
            for (j = 0; j < src_n && !in_src; j++) {
                in_src = srcf[j].id == cc.f[i].id;
            }
            if (!in_src) {
                ent[n_cap].id = cc.f[i].id;
                ent[n_cap].name = cc.f[i].name;
                ent[n_cap].read = 0;
                ent[n_cap].write = id_in_list(xf.writes, xf.writes_n, cc.f[i].id);
                n_cap++;
            }
        }
        for (i = 0; i < n_cap; i++) {
            uint32_t j;
            if (ent[i].id == 0u) {
                st = fail_plan(out, SU_E_TYPE, "transform cannot use field ID 0 (capsule IDs are positive)");
                goto done;
            }
            for (j = 0; j < i; j++) {
                if (strcmp(ent[j].name, ent[i].name) == 0) {
                    st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "field name collides between source and destination");
                    goto done;
                }
            }
        }
        st = build_capsule(c->schema_id, c->schema_version, from_gen, lsid->u.string.data, lsid->u.string.len,
                           lprof->u.string.data, lprof->u.string.len, xf.source, xf.source_len, ent, n_cap,
                           &capsule, &capsule_len);
        if (st != SU_OK) {
            fail_plan(out, st, "cannot synthesize the transform capsule");
            goto done;
        }
        c->capsule = capsule;
        c->capsule_len = capsule_len;
        capsule = NULL;
        st = su_lir_verify(c->capsule, c->capsule_len, c->lir, c->lir_len, &mod, &diag);
        if (st != SU_OK) {
            fail_plan(out, SU_E_CANDIDATE_FAILED, diag.message);
            st = SU_E_CANDIDATE_FAILED;
            goto done;
        }
        memcpy(out->transform_semantic_hash, mod.semantic_hash, 32u);
        out->has_transform = 1;
        out->transform_writes = xf.writes_n;
        su_lir_module_free(&mod);
        memset(&mod, 0, sizeof mod);
    }

    /* Pre-migration state the plan is bound to. */
    c->pre_tick = su_world_tick(w);
    st = su_world_recovery_digest(w, c->pre_digest);
    if (st != SU_OK) {
        goto done;
    }
    /* Copy the lineage (old, then this migration's hash, filled after hashing). */
    {
        const uint8_t (*lin)[32] = NULL;
        uint32_t lin_n = 0;
        if (su_world_lineage(w, &lin, &lin_n) != SU_OK || lin_n >= SU_WORLD_MAX_HISTORY) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "lineage exceeds the history limit");
            goto done;
        }
        c->lineage_n = lin_n + 1u;
        c->lineage = mig_malloc(c->lineage_n * sizeof *c->lineage);
        if (c->lineage == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
        if (lin_n > 0u) {
            memcpy(c->lineage, lin, lin_n * sizeof *c->lineage);
        }
    }

    /* Migration hash: SHA-256 over "SUMIGRATE" 0x00 and length-prefixed migration, transform and IR bytes. */
    su_sha256_init(&hc);
    su_sha256_update(&hc, "SUMIGRATE", 9u);
    su_sha256_update(&hc, "\0", 1u);
    {
        uint8_t l8[8];
        put_u64le(l8, in->migration_len);
        su_sha256_update(&hc, l8, 8u);
        su_sha256_update(&hc, in->migration, in->migration_len);
        put_u64le(l8, have_xf ? in->transform_len : 0u);
        su_sha256_update(&hc, l8, 8u);
        if (have_xf) {
            su_sha256_update(&hc, in->transform, in->transform_len);
        }
        put_u64le(l8, have_xf ? in->lir_len : 0u);
        su_sha256_update(&hc, l8, 8u);
        if (have_xf) {
            su_sha256_update(&hc, in->lir, in->lir_len);
        }
    }
    su_sha256_final(&hc, out->migration_hash);
    su_sha256_hex(out->migration_hash, out->migration_hash_hex);
    memcpy(c->lineage[c->lineage_n - 1u], out->migration_hash, 32u);

    /* Identity of the migration. */
    {
        size_t il = idv->u.string.len;
        memcpy(out->migration_id, idv->u.string.data, il);
        out->migration_id[il] = '\0';
    }
    out->from_layout_generation = from_gen;
    out->to_layout_generation = to_gen;

    /* Schemas before and after. */
    out->schema_before_fields = src_n;
    out->schema_after_fields = c->dst_n;
    st = su_world_schema_digest(w, out->schema_before_digest);
    if (st != SU_OK) {
        goto done;
    }
    schema_digest_of(c->dst, c->dst_def, c->dst_n, c->after_digest);
    memcpy(out->schema_after_digest, c->after_digest, 32u);
    for (i = 0; i < src_n; i++) {
        if (wf_find(&cc, srcf[i].id) < 0) {
            out->columns_removed++;
        } else if (strcmp(srcf[i].name, cc.f[wf_find(&cc, srcf[i].id)].name) != 0) {
            out->columns_renamed++;
        }
    }
    for (i = 0; i < c->dst_n; i++) {
        if (!id_in_list(c->src_ids, src_n, c->dst[i].id)) {
            out->columns_added++;
        }
    }
    for (i = 0; i < c->writes_n; i++) {
        (void)i;
    }

    /* Resource admission (ESTIMATES from rows and fields; contract section 3). */
    hw = su_ents_high_water(su_world_entities_committed(w));
    cap = su_world_row_count(w);
    out->rows = hw;
    out->capacity = cap;
    for (i = 0; i < c->dst_n; i++) {
        if ((c->dst[i].access & SU_ACCESS_WRITE) != 0u) {
            staging_fields++;
        }
    }
    s_rows = cap > 0u ? cap : 1u;
    bytes_new = (2u * (uint64_t)c->dst_n + staging_fields) * s_rows * 4u;
    scratch = (hw > 0u ? hw : 1u) * (5u + 4u * (uint64_t)c->dst_n);
    out->bytes_new_columns = bytes_new;
    out->bytes_peak = bytes_new + scratch;
    out->pre_migration_bytes = est_bytes(hw, src_n);
    out->post_commit_bytes_bound = est_bytes(hw, c->dst_n);

    /* Per-system impact. */
    out->system_count = system_count;
    if (system_count > 0u) {
        out->systems = mig_calloc(system_count, sizeof *out->systems);
        if (out->systems == NULL) {
            st = fail_plan(out, SU_E_RESOURCE_LIMIT, "out of memory");
            goto done;
        }
    }
    for (i = 0; i < system_count; i++) {
        SuMigrateSystemImpact *si = &out->systems[i];
        uint32_t f;
        uint32_t reasons = SU_MIGRATE_REASON_LAYOUT;
        int broken = 0;
        int dropped = 0;
        uint32_t j;
        if (systems[i].system_id == NULL || strlen(systems[i].system_id) >= sizeof si->system_id) {
            st = fail_plan(out, SU_E_INVALID_ARGUMENT, "system_id is missing or too long");
            goto done;
        }
        strcpy(si->system_id, systems[i].system_id);
        for (j = 0; j < drop_count; j++) {
            if (strcmp(drop_systems[j], systems[i].system_id) == 0) {
                dropped = 1;
            }
        }
        for (f = 0; f < systems[i].field_count; f++) {
            const SuWorldFieldDesc *sf = &systems[i].fields[f];
            long at_src = -1;
            long at_dst;
            uint32_t q;
            for (q = 0; q < src_n; q++) {
                if (srcf[q].id == sf->id) {
                    at_src = (long)q;
                }
            }
            at_dst = wf_find(&cc, sf->id);
            if (at_src < 0) {
                /* A field the world does not know: the system was never installable. */
                st = fail_plan(out, SU_E_SCHEMA_MISMATCH, "an installed system uses a field the world does not have");
                goto done;
            }
            if (at_dst < 0) {
                reasons |= SU_MIGRATE_REASON_FIELD_REMOVED;
                broken = 1;
            } else {
                if (strcmp(cc.f[at_dst].name, srcf[at_src].name) != 0) {
                    reasons |= SU_MIGRATE_REASON_RENAMED;
                }
                if (id_in_list(xf.writes, xf.writes_n, sf->id) && have_xf) {
                    reasons |= SU_MIGRATE_REASON_TRANSFORMED;
                }
            }
        }
        si->reasons = reasons;
        if (broken) {
            si->impact = SU_MIGRATE_IMPACT_BROKEN;
            if (!dropped) {
                n_broken++;
            }
        } else if ((reasons & (SU_MIGRATE_REASON_RENAMED | SU_MIGRATE_REASON_TRANSFORMED)) != 0u) {
            si->impact = SU_MIGRATE_IMPACT_NEEDS_RESUBMISSION;
        } else {
            si->impact = SU_MIGRATE_IMPACT_COMPATIBLE;
        }
    }

    /* Admission verdict. Order: field in use, then spool budget, then memory. */
    out->valid = 1;
    if (n_broken > 0u) {
        out->valid = 0;
        out->reason = SU_MIGRATE_REFUSED_FIELD_IN_USE;
        set_message(out, "a removed field is used by an installed system that is not dropped");
    } else if (out->pre_migration_bytes + out->post_commit_bytes_bound > budget->spool_bytes) {
        out->valid = 0;
        out->reason = SU_MIGRATE_REFUSED_SPOOL_BUDGET;
        set_message(out, "pre-migration plus post-commit checkpoint estimate exceeds the spool budget");
    } else if (out->bytes_peak > budget->memory_bytes) {
        out->valid = 0;
        out->reason = SU_MIGRATE_REFUSED_MEMORY;
        set_message(out, "peak memory estimate exceeds the memory budget");
    } else {
        set_message(out, "admitted");
    }

    /* Plan hash: binds the migration, the destination, and the pre-migration state. */
    {
        SuSha256 pc;
        uint8_t tmp[8];
        const char *tag = "SUMPLAN";
        su_sha256_init(&pc);
        su_sha256_update(&pc, tag, 7u);
        su_sha256_update(&pc, "\0", 1u);
        su_sha256_update(&pc, out->migration_hash, 32u);
        put_u32le(tmp, from_gen);
        put_u32le(tmp + 4, to_gen);
        su_sha256_update(&pc, tmp, 8u);
        su_sha256_update(&pc, c->pre_digest, 32u);
        su_sha256_update(&pc, c->after_digest, 32u);
        put_u64le(tmp, c->pre_tick);
        su_sha256_update(&pc, tmp, 8u);
        put_u64le(tmp, hw);
        su_sha256_update(&pc, tmp, 8u);
        su_sha256_final(&pc, out->plan_hash);
        su_sha256_hex(out->plan_hash, out->plan_hash_hex);
    }
    (void)n_cap;
    st = SU_OK;
    goto done_ok;

done:
    /* Error path: the plan keeps only its message. */
    if (out != NULL) {
        out->valid = 0;
        compile_free(out->impl);
        out->impl = NULL;
        free(out->systems);
        out->systems = NULL;
        out->system_count = 0u;
    }
done_ok:
    free(ent);
    free(capsule);
    free(srcf);
    free(srcdef);
    free(cc.removed);
    xform_free(&xf);
    su_json_doc_free(&ldoc);
    su_json_doc_free(&mdoc);
    return st;
}

/* ---- prepare ---------------------------------------------------------------- */

static uint32_t find_src_index(const SuMigrateCompiled *c, uint32_t id, int *found) {
    uint32_t i;
    *found = 0;
    for (i = 0; i < c->src_n; i++) {
        if (c->src_ids[i] == id) {
            *found = 1;
            return i;
        }
    }
    return 0;
}

static int in_writes(const SuMigrateCompiled *c, uint32_t id) {
    return id_in_list(c->writes, c->writes_n, id);
}

static uint32_t prepare_body(const SuWorld *old, const SuMigratePlan *p, SuMigrateHooks *hooks,
                             SuWorld **out, SuMigrateTimings *timings);

uint32_t su_migrate_prepare(const SuWorld *old, const SuMigratePlan *p, SuMigrateHooks *hooks,
                            SuWorld **out, SuMigrateTimings *timings) {
    uint32_t st;
    g_alloc_hooks = hooks;
    st = prepare_body(old, p, hooks, out, timings);
    g_alloc_hooks = NULL;
    return st;
}

static uint32_t prepare_body(const SuWorld *old, const SuMigratePlan *p, SuMigrateHooks *hooks,
                             SuWorld **out, SuMigrateTimings *timings) {
    SuMigrateCompiled *c;
    uint32_t st = SU_OK;
    uint64_t t0;
    uint64_t hw;
    uint64_t cap;
    uint64_t tick_before;
    uint8_t *alive = NULL;
    uint32_t *gen = NULL;
    uint32_t **buf = NULL;
    SuWorld *nw = NULL;
    SuWorldDesc desc;
    SuWorldSchemaExt ext;
    uint8_t dig[32];
    uint32_t i;
    uint32_t j;
    SuMigrateTimings tt;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (old == NULL || p == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&tt, 0, sizeof tt);
    c = p->impl;
    if (!p->valid || c == NULL) {
        return SU_E_UNSUPPORTED_MIGRATION;
    }
    t0 = now_ns();
    if (su_world_in_tick(old) || su_world_tick(old) != c->pre_tick || su_world_layout_generation(old) != c->from_gen) {
        return SU_E_STALE_REVISION;
    }
    st = su_world_recovery_digest(old, dig);
    if (st != SU_OK) {
        return st;
    }
    if (memcmp(dig, c->pre_digest, 32u) != 0) {
        return SU_E_STALE_REVISION;
    }
    tick_before = su_world_tick(old);
    hw = su_ents_high_water(su_world_entities_committed(old));
    cap = su_world_row_count(old);

    /* Entity scratch: alive and generation views of the stored rows. */
    MIG_POINT(hooks);
    alive = mig_malloc(hw > 0u ? (size_t)hw : 1u);
    gen = mig_malloc((hw > 0u ? (size_t)hw : 1u) * sizeof *gen);
    if (alive == NULL || gen == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (i = 0; i < hw; i++) {
        alive[i] = (uint8_t)(su_ents_slot_alive(su_world_entities_committed(old), i) ? 1 : 0);
        (void)su_ents_slot_generation(su_world_entities_committed(old), i, &gen[i]);
    }

    /* One column buffer per destination field (hw rows; rows at or above high_water stay implicit zero). */
    MIG_POINT(hooks);
    buf = mig_calloc(c->dst_n > 0u ? c->dst_n : 1u, sizeof *buf);
    if (buf == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (j = 0; j < c->dst_n; j++) {
        int found = 0;
        const uint32_t *src_col = NULL;
        uint64_t rows = 0;
        uint32_t r;
        uint32_t sidx = find_src_index(c, c->dst[j].id, &found);
        (void)sidx;
        MIG_POINT(hooks);
        buf[j] = mig_calloc(hw > 0u ? (size_t)hw : 1u, sizeof **buf);
        if (buf[j] == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
        if (found && !in_writes(c, c->dst[j].id)) {
            st = su_world_read_committed(old, c->dst[j].id, &src_col, &rows);
            if (st != SU_OK) {
                st = SU_E_INTERNAL;
                goto done;
            }
            for (r = 0; r < hw; r++) {
                buf[j][r] = alive[r] ? src_col[r] : 0u;
            }
        } else if (!found) {
            for (r = 0; r < hw; r++) {
                buf[j][r] = alive[r] ? c->dst_def[j] : 0u;
            }
        }
        /* Written fields (found or not) are overwritten by the transform for every alive row. */
    }

    /* Transform through the reference VM (unchanged verifier and invoke). */
    if (c->has_transform) {
        SuLirModule mod;
        SuDiag diag;
        SuExecContext *ctx = NULL;
        SuColumnBinding *in_b = NULL;
        SuColumnBinding *out_b = NULL;
        SuInvocation inv;
        SuOutputs outs;
        uint32_t n_in = 0;
        uint32_t n_out = 0;
        uint32_t k;
        uint64_t t_tr0;
        uint64_t t_tr1;
        memset(&mod, 0, sizeof mod);
        su_diag_init(&diag);
        MIG_POINT(hooks);
        st = su_lir_verify(c->capsule, c->capsule_len, c->lir, c->lir_len, &mod, &diag);
        if (st != SU_OK) {
            st = SU_E_CANDIDATE_FAILED;
            goto done;
        }
        if (inject_point(hooks)) {
            st = SU_E_RESOURCE_LIMIT;
            goto transform_cleanup;
        }
        st = su_vm_context_create(&mod, c->from_gen, &ctx);
        if (st != SU_OK) {
            su_lir_module_free(&mod);
            st = SU_E_CANDIDATE_FAILED;
            goto done;
        }
        for (k = 0; k < mod.field_count; k++) {
            if ((mod.fields[k].access & SU_ACCESS_READ) != 0u) {
                n_in++;
            }
            if ((mod.fields[k].access & SU_ACCESS_WRITE) != 0u) {
                n_out++;
            }
        }
        if (inject_point(hooks)) {
            st = SU_E_RESOURCE_LIMIT;
            goto transform_cleanup;
        }
        in_b = mig_calloc(n_in > 0u ? n_in : 1u, sizeof *in_b);
        out_b = mig_calloc(n_out > 0u ? n_out : 1u, sizeof *out_b);
        if (in_b == NULL || out_b == NULL) {
            free(in_b);
            free(out_b);
            su_vm_context_destroy(ctx);
            su_lir_module_free(&mod);
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
        n_in = 0;
        n_out = 0;
        for (k = 0; k < mod.field_count; k++) {
            uint32_t id = mod.fields[k].id;
            if ((mod.fields[k].access & SU_ACCESS_READ) != 0u) {
                const uint32_t *col = NULL;
                uint64_t rows = 0;
                if (su_world_read_committed(old, id, &col, &rows) != SU_OK) {
                    st = SU_E_INTERNAL;
                    goto transform_cleanup;
                }
                in_b[n_in].struct_size = sizeof(SuColumnBinding);
                in_b[n_in].field_id = id;
                in_b[n_in].elem_type = SU_TYPE_U32;
                in_b[n_in].access = SU_ACCESS_READ;
                in_b[n_in].layout_generation = c->from_gen;
                in_b[n_in].stride_bytes = 4u;
                in_b[n_in].row_count = hw;
                in_b[n_in].data = (void *)col;
                n_in++;
            }
            if ((mod.fields[k].access & SU_ACCESS_WRITE) != 0u) {
                uint32_t d;
                int hit = 0;
                for (d = 0; d < c->dst_n && !hit; d++) {
                    if (c->dst[d].id == id) {
                        out_b[n_out].struct_size = sizeof(SuColumnBinding);
                        out_b[n_out].field_id = id;
                        out_b[n_out].elem_type = SU_TYPE_U32;
                        out_b[n_out].access = SU_ACCESS_WRITE;
                        out_b[n_out].layout_generation = c->from_gen;
                        out_b[n_out].stride_bytes = 4u;
                        out_b[n_out].row_count = hw;
                        out_b[n_out].data = buf[d];
                        hit = 1;
                    }
                }
                if (!hit) {
                    st = SU_E_INTERNAL;
                    goto transform_cleanup;
                }
                n_out++;
            }
        }
        st = su_vm_context_set_entity_view(ctx, alive, gen, 0u, (uint32_t)(tick_before + 1u), NULL);
        if (st != SU_OK) {
            goto transform_cleanup;
        }
        memset(&inv, 0, sizeof inv);
        inv.struct_size = sizeof inv;
        inv.abi_version = SU_ABI_VERSION;
        inv.layout_generation = c->from_gen;
        inv.binding_count = n_in;
        inv.row_count = hw;
        inv.bindings = in_b;
        memset(&outs, 0, sizeof outs);
        outs.struct_size = sizeof outs;
        outs.binding_count = n_out;
        outs.failed_insn = UINT32_MAX;
        outs.bindings = out_b;
        if (hooks != NULL && hooks->fail_transform) {
            st = SU_E_CANDIDATE_FAILED;
            goto transform_cleanup;
        }
        t_tr0 = now_ns();
        st = su_vm_invoke_v1(ctx, &inv, &outs);
        t_tr1 = now_ns();
        tt.transform_ns = t_tr1 >= t_tr0 ? t_tr1 - t_tr0 : 0u;
        if (st != SU_OK) {
            st = SU_E_CANDIDATE_FAILED;
        }
transform_cleanup:
        free(in_b);
        free(out_b);
        su_vm_context_destroy(ctx);
        su_lir_module_free(&mod);
        if (st != SU_OK) {
            goto done;
        }
    }

    /* Buffer self-check before anything is published: dead rows below high_water hold zero in every column.
     * (The world's put also zeroes them, so this check is what makes a buffer defect visible.) */
    for (j = 0; j < c->dst_n; j++) {
        uint64_t rr;
        for (rr = 0; rr < hw; rr++) {
            if (!alive[rr] && buf[j][rr] != 0u) {
                st = SU_E_INTERNAL;
                goto done;
            }
        }
    }

    /* Successor world: same entity table, capacity and tick; new schema, defaults, lineage and generation. */
    MIG_POINT(hooks);
    memset(&desc, 0, sizeof desc);
    desc.schema_id = c->schema_id;
    desc.schema_version = c->schema_version;
    desc.layout_generation = c->to_gen;
    desc.row_count = 0u;
    desc.entity_ids = NULL;
    desc.fields = c->dst;
    desc.field_count = c->dst_n;
    memset(&ext, 0, sizeof ext);
    ext.defaults = c->dst_def;
    ext.retired_ids = c->retired;
    ext.retired_count = c->retired_n;
    ext.lineage = (const uint8_t (*)[32])c->lineage;
    ext.lineage_count = c->lineage_n;
    st = su_world_create_successor(old, &desc, &ext, &nw);
    if (st != SU_OK) {
        goto done;
    }
    for (j = 0; j < c->dst_n; j++) {
        MIG_POINT(hooks);
        st = su_world_put_committed(nw, c->dst[j].id, buf[j], hw);
        if (st != SU_OK) {
            st = SU_E_INTERNAL;
            goto done;
        }
    }

    /* Self-checks. Each is an invariant of the contract, checked against the old world and the plan. */
    {
        int eq = 0;
        const uint32_t *col = NULL;
        uint64_t rows = 0;
        uint8_t nd[32];
        uint64_t r;
        if (su_world_tick(nw) != tick_before || su_world_layout_generation(nw) != c->to_gen ||
            su_world_row_count(nw) != cap) {
            st = SU_E_INTERNAL;
            goto done;
        }
        if (su_world_schema_digest(nw, nd) != SU_OK || memcmp(nd, c->after_digest, 32u) != 0) {
            st = SU_E_INTERNAL;
            goto done;
        }
        if (su_ents_state_equal(su_world_entities_committed(old), su_world_entities_committed(nw), &eq) != SU_OK ||
            !eq) {
            st = SU_E_INTERNAL;
            goto done;
        }
        for (j = 0; j < c->dst_n; j++) {
            int found = 0;
            uint32_t sidx = find_src_index(c, c->dst[j].id, &found);
            const uint32_t *old_col = NULL;
            uint64_t old_rows = 0;
            (void)sidx;
            if (su_world_read_committed(nw, c->dst[j].id, &col, &rows) != SU_OK || rows != cap) {
                st = SU_E_INTERNAL;
                goto done;
            }
            if (found && !in_writes(c, c->dst[j].id)) {
                if (su_world_read_committed(old, c->dst[j].id, &old_col, &old_rows) != SU_OK) {
                    st = SU_E_INTERNAL;
                    goto done;
                }
            }
            for (r = 0; r < cap; r++) {
                int alive_row = r < hw && alive[r];
                if (!alive_row) {
                    if (col[r] != 0u) {
                        st = SU_E_INTERNAL;
                        goto done;
                    }
                } else if (found && !in_writes(c, c->dst[j].id)) {
                    if (col[r] != old_col[r]) {
                        st = SU_E_INTERNAL;
                        goto done;
                    }
                } else if (!found && !in_writes(c, c->dst[j].id)) {
                    if (col[r] != c->dst_def[j]) {
                        st = SU_E_INTERNAL;
                        goto done;
                    }
                }
            }
        }
    }

    *out = nw;
    nw = NULL;
    st = SU_OK;

done:
    if (nw != NULL) {
        su_world_destroy(nw);
    }
    if (buf != NULL) {
        for (j = 0; j < c->dst_n; j++) {
            free(buf[j]);
        }
    }
    free(buf);
    free(alive);
    free(gen);
    if (timings != NULL) {
        tt.prepare_ns = now_ns() - t0;
        *timings = tt;
    }
    return st;
}

/* ---- swap ------------------------------------------------------------------- */

uint32_t su_migrate_swap_check(const SuWorld *live, const SuMigratePlan *p, const SuWorld *candidate) {
    SuMigrateCompiled *c;
    uint8_t dig[32];
    uint8_t nd[32];
    if (live == NULL || p == NULL || candidate == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    c = p->impl;
    if (!p->valid || c == NULL || su_world_in_tick(live) || su_world_in_tick(candidate)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_world_tick(live) != c->pre_tick || su_world_layout_generation(live) != c->from_gen) {
        return SU_E_STALE_REVISION;
    }
    if (su_world_recovery_digest(live, dig) != SU_OK || memcmp(dig, c->pre_digest, 32u) != 0) {
        return SU_E_STALE_REVISION;
    }
    if (su_world_layout_generation(candidate) != c->to_gen || su_world_tick(candidate) != c->pre_tick ||
        su_world_schema_digest(candidate, nd) != SU_OK || memcmp(nd, c->after_digest, 32u) != 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    return SU_OK;
}

uint32_t su_migrate_swap(SuWorld **live, const SuMigratePlan *p, SuWorld *candidate, SuWorld **retired) {
    uint32_t st;
    if (live == NULL || *live == NULL || p == NULL || candidate == NULL || retired == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *retired = NULL;
    st = su_migrate_swap_check(*live, p, candidate);
    if (st != SU_OK) {
        return st;
    }
    /* Publication: two pointer writes after every check. */
    *retired = *live;
    *live = candidate;
    return SU_OK;
}
