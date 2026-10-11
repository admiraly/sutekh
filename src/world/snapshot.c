/* Canonical JSON snapshots of a committed world (M0-08). See snapshot.h. */
#include "world/snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_sha256.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"

/* Canonical encoding of the world's committed root, as documented in
 * su_world.h: "SUWSTATE" 0x00 | u32 version | u64 tick | u64 rows |
 * u32 field_count | u32 entity_ids[rows] | per-field records. */
#define SNAP_CANON_HDR_BYTES 33u
#define SNAP_CANON_MAGIC_BYTES 9u
#define SNAP_CANON_ROWS_OFF (SNAP_CANON_MAGIC_BYTES + 4u + 8u)
#define SNAP_CANON_FC_OFF (SNAP_CANON_ROWS_OFF + 8u)
#define SNAP_CANON_TICK_OFF (SNAP_CANON_MAGIC_BYTES + 4u)

#define SNAP_TYPE_NAME "u32"

static const char *const k_top_keys[] = {
    "snapshot_format", "world_revision", "tick", "schema_id", "schema_version",
    "layout_generation", "numeric_profile", "fields", "semantic_manifest",
    "entity_ids", "columns"};
#define K_TOP_COUNT (sizeof k_top_keys / sizeof k_top_keys[0])

static const char *const k_field_keys[] = {"id", "name", "type"};
static const char *const k_manifest_keys[] = {"system_id", "semantic_hash"};

/* Emission-side view of one snapshot. Fields are ascending by id; columns[i]
 * holds row_count committed values for fields[i]. */
typedef struct SnapBody {
    const char *rev;
    uint64_t tick;
    const char *schema_id;
    uint32_t schema_version;
    uint32_t layout_generation;
    const SuWorldFieldDesc *fields;
    uint32_t field_count;
    uint64_t row_count;
    const uint32_t *entity_ids;
    const uint32_t *const *columns;
    const SuSnapshotManifestEntry *manifest;
    uint32_t manifest_count;
} SnapBody;

typedef struct SnapBuf {
    uint8_t *p;
    size_t len;
    size_t cap;
} SnapBuf;

static void fail_msg(SuSnapshotError *e, const char *msg) {
    size_t n;
    if (e == NULL) {
        return;
    }
    n = strlen(msg);
    if (n >= sizeof e->message) {
        n = sizeof e->message - 1u;
    }
    memcpy(e->message, msg, n);
    e->message[n] = '\0';
}

/* ---- validation helpers ----------------------------------------------- */

/* Strict UTF-8 (no overlongs, no surrogates, max U+10FFFF). Embedded NUL is
 * rejected. */
static int valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        unsigned cp;
        unsigned min;
        size_t need;
        size_t k;
        if (c < 0x80u) {
            if (c == 0u) {
                return 0;
            }
            i++;
            continue;
        }
        if (c >= 0xC2u && c <= 0xDFu) {
            need = 1;
            cp = c & 0x1Fu;
            min = 0x80u;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            need = 2;
            cp = c & 0x0Fu;
            min = 0x800u;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            need = 3;
            cp = c & 0x07u;
            min = 0x10000u;
        } else {
            return 0;
        }
        if (n - i <= need) {
            return 0;
        }
        for (k = 1; k <= need; k++) {
            unsigned cc = s[i + k];
            if ((cc & 0xC0u) != 0x80u) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return 0;
        }
        i += need + 1u;
    }
    return 1;
}

/* Text of 1..max bytes, valid UTF-8. */
static int text_ok(const char *s, size_t len, size_t max) {
    return len >= 1u && len <= max && valid_utf8((const unsigned char *)s, len);
}

/* C string variant with a bounded scan. */
static int cstr_text_ok(const char *s, size_t max) {
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (n <= max && s[n] != '\0') {
        n++;
    }
    return n <= max && text_ok(s, n, max);
}

/* Length of a NUL-terminated string within cap bytes; cap if no NUL. */
static size_t bounded_len(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

static int hash_ok(const char *s, size_t len) {
    size_t i;
    if (len != 64u) {
        return 0;
    }
    for (i = 0; i < 64u; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static uint32_t check_manifest(const SuSnapshotManifestEntry *m, uint32_t count) {
    uint32_t i;
    if (count > SU_SNAPSHOT_MAX_MANIFEST) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (count > 0u && m == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        if (!cstr_text_ok(m[i].system_id, SU_SNAPSHOT_MAX_NAME)) {
            return SU_E_INVALID_ARGUMENT;
        }
        if (!hash_ok(m[i].semantic_hash, bounded_len(m[i].semantic_hash, sizeof m[i].semantic_hash))) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    return SU_OK;
}

static int field_id_cmp(const void *pa, const void *pb) {
    const SuWorldFieldDesc *a = pa;
    const SuWorldFieldDesc *b = pb;
    return (a->id > b->id) - (a->id < b->id);
}

/* Validates a descriptor the way su_world_create does, and returns a copy of
 * its fields sorted ascending by id (caller frees *sorted). */
static uint32_t check_desc(const SuWorldDesc *d, SuWorldFieldDesc **sorted) {
    uint32_t i;
    uint32_t j;
    *sorted = NULL;
    if (d == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (d->row_count > SU_LIMIT_ROWS || d->field_count > SU_LIMIT_FIELDS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if ((d->row_count > 0u && d->entity_ids == NULL) ||
        (d->field_count > 0u && d->fields == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!cstr_text_ok(d->schema_id, SU_WORLD_NAME_MAX)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < d->field_count; i++) {
        const SuWorldFieldDesc *f = &d->fields[i];
        if (f->type != SU_TYPE_U32) {
            return SU_E_TYPE;
        }
        if (!cstr_text_ok(f->name, SU_WORLD_NAME_MAX)) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (j = 0; j < i; j++) {
            if (d->fields[j].id == f->id || strcmp(d->fields[j].name, f->name) == 0) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
    }
    if (d->field_count > 0u) {
        *sorted = malloc(d->field_count * sizeof **sorted);
        if (*sorted == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        memcpy(*sorted, d->fields, d->field_count * sizeof **sorted);
        qsort(*sorted, d->field_count, sizeof **sorted, field_id_cmp);
    }
    return SU_OK;
}

/* ---- byte buffer and emitter ------------------------------------------ */

static uint32_t buf_reserve(SnapBuf *b, size_t extra) {
    size_t need;
    size_t cap;
    uint8_t *p;
    if (extra > SU_SNAPSHOT_MAX_BYTES || b->len > SU_SNAPSHOT_MAX_BYTES - extra) {
        return SU_E_RESOURCE_LIMIT;
    }
    need = b->len + extra;
    if (need <= b->cap) {
        return SU_OK;
    }
    cap = b->cap > 0u ? b->cap : 4096u;
    while (cap < need) {
        cap = cap > SU_SNAPSHOT_MAX_BYTES / 2u ? SU_SNAPSHOT_MAX_BYTES : cap * 2u;
    }
    p = realloc(b->p, cap);
    if (p == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    b->p = p;
    b->cap = cap;
    return SU_OK;
}

static uint32_t buf_bytes(SnapBuf *b, const void *s, size_t n) {
    uint32_t st;
    if (n == 0u) {
        return SU_OK;
    }
    st = buf_reserve(b, n);
    if (st != SU_OK) {
        return st;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return SU_OK;
}

static uint32_t buf_lit(SnapBuf *b, const char *s) {
    return buf_bytes(b, s, strlen(s));
}

static uint32_t buf_dec(SnapBuf *b, uint64_t v) {
    char t[24];
    size_t n = 0;
    size_t i;
    do {
        t[n++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    for (i = 0; i < n / 2u; i++) {
        char c = t[i];
        t[i] = t[n - 1u - i];
        t[n - 1u - i] = c;
    }
    return buf_bytes(b, t, n);
}

/* JSON string with the escapes this format needs: quote, backslash, and
 * control characters as \u00XX. Other bytes are valid UTF-8 and pass through. */
static uint32_t buf_jstr(SnapBuf *b, const char *s) {
    static const char hex[] = "0123456789abcdef";
    uint32_t st;
    const unsigned char *p = (const unsigned char *)s;
    st = buf_lit(b, "\"");
    while (st == SU_OK && *p != '\0') {
        unsigned c = *p++;
        if (c == '"') {
            st = buf_lit(b, "\\\"");
        } else if (c == '\\') {
            st = buf_lit(b, "\\\\");
        } else if (c < 0x20u) {
            char esc[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xFu], hex[c & 0xFu]};
            st = buf_bytes(b, esc, sizeof esc);
        } else {
            st = buf_bytes(b, &c, 1u);
        }
    }
    if (st != SU_OK) {
        return st;
    }
    return buf_lit(b, "\"");
}

static uint32_t emit_body(const SnapBody *s, SnapBuf *o) {
    uint32_t st;
    uint32_t i;
    uint64_t r;

#define SNAP_W(expr)                                                          \
    do {                                                                      \
        st = (expr);                                                          \
        if (st != SU_OK) {                                                    \
            return st;                                                        \
        }                                                                     \
    } while (0)

    SNAP_W(buf_lit(o, "{\"snapshot_format\":\"" SU_SNAPSHOT_FORMAT "\",\"world_revision\":"));
    SNAP_W(buf_jstr(o, s->rev));
    SNAP_W(buf_lit(o, ",\"tick\":"));
    SNAP_W(buf_dec(o, s->tick));
    SNAP_W(buf_lit(o, ",\"schema_id\":"));
    SNAP_W(buf_jstr(o, s->schema_id));
    SNAP_W(buf_lit(o, ",\"schema_version\":"));
    SNAP_W(buf_dec(o, s->schema_version));
    SNAP_W(buf_lit(o, ",\"layout_generation\":"));
    SNAP_W(buf_dec(o, s->layout_generation));
    SNAP_W(buf_lit(o, ",\"numeric_profile\":\"" SU_NUMERIC_PROFILE_U32_MOD_V1 "\""));

    SNAP_W(buf_lit(o, ",\"fields\":["));
    for (i = 0; i < s->field_count; i++) {
        if (i > 0u) {
            SNAP_W(buf_lit(o, ","));
        }
        SNAP_W(buf_lit(o, "{\"id\":"));
        SNAP_W(buf_dec(o, s->fields[i].id));
        SNAP_W(buf_lit(o, ",\"name\":"));
        SNAP_W(buf_jstr(o, s->fields[i].name));
        SNAP_W(buf_lit(o, ",\"type\":\"" SNAP_TYPE_NAME "\"}"));
    }

    SNAP_W(buf_lit(o, "],\"semantic_manifest\":["));
    for (i = 0; i < s->manifest_count; i++) {
        if (i > 0u) {
            SNAP_W(buf_lit(o, ","));
        }
        SNAP_W(buf_lit(o, "{\"system_id\":"));
        SNAP_W(buf_jstr(o, s->manifest[i].system_id));
        SNAP_W(buf_lit(o, ",\"semantic_hash\":\""));
        SNAP_W(buf_bytes(o, s->manifest[i].semantic_hash, 64u));
        SNAP_W(buf_lit(o, "\"}"));
    }

    SNAP_W(buf_lit(o, "],\"entity_ids\":["));
    for (r = 0; r < s->row_count; r++) {
        if (r > 0u) {
            SNAP_W(buf_lit(o, ","));
        }
        SNAP_W(buf_dec(o, s->entity_ids[r]));
    }

    SNAP_W(buf_lit(o, "],\"columns\":{"));
    for (i = 0; i < s->field_count; i++) {
        uint64_t k;
        if (i > 0u) {
            SNAP_W(buf_lit(o, ","));
        }
        SNAP_W(buf_jstr(o, s->fields[i].name));
        SNAP_W(buf_lit(o, ":["));
        for (k = 0; k < s->row_count; k++) {
            if (k > 0u) {
                SNAP_W(buf_lit(o, ","));
            }
            SNAP_W(buf_dec(o, s->columns[i][k]));
        }
        SNAP_W(buf_lit(o, "]"));
    }
    SNAP_W(buf_lit(o, "}}"));
#undef SNAP_W
    return SU_OK;
}

/* ---- capture ----------------------------------------------------------- */

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

void su_snapshot_destroy(SuSnapshot *s) {
    if (s == NULL) {
        return;
    }
    free(s->bytes);
    memset(s, 0, sizeof *s);
}

uint32_t su_snapshot_capture(const SuWorld *w, const SuWorldDesc *desc,
                             const char *world_revision,
                             const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                             SuSnapshot *out) {
    SuWorldFieldDesc *sf = NULL;
    uint8_t *canon = NULL;
    size_t canon_len = 0;
    const uint32_t **cols = NULL;
    SnapBody body;
    SnapBuf buf;
    uint64_t rows;
    uint32_t i;
    uint32_t st = SU_OK;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    memset(&buf, 0, sizeof buf);
    if (w == NULL || desc == NULL || world_revision == NULL ||
        (manifest_count > 0u && manifest == NULL)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!cstr_text_ok(world_revision, SU_SNAPSHOT_MAX_NAME)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (su_world_in_tick(w)) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = check_manifest(manifest, manifest_count);
    if (st != SU_OK) {
        return st;
    }
    st = check_desc(desc, &sf);
    if (st != SU_OK) {
        return st;
    }
    rows = desc->row_count;
    if (su_world_row_count(w) != rows || su_world_field_count(w) != desc->field_count ||
        su_world_layout_generation(w) != desc->layout_generation) {
        st = SU_E_SCHEMA_MISMATCH;
        goto done;
    }
    /* Cheap lower bound on the encoded size (two bytes per value), checked
     * before the transient canonical copy is allocated. */
    if (rows * ((uint64_t)desc->field_count + 1u) > SU_SNAPSHOT_MAX_BYTES / 2u) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }

    /* Entity IDs and tick come from the world's documented canonical encoding.
     * The descriptor's IDs must agree with it. */
    st = su_world_canonical_size(w, &canon_len);
    if (st != SU_OK) {
        goto done;
    }
    canon = malloc(canon_len > 0u ? canon_len : 1u);
    if (canon == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    {
        size_t need = canon_len;
        st = su_world_canonical_write(w, canon, need, &canon_len);
        if (st != SU_OK || canon_len != need) {
            st = (st != SU_OK) ? st : SU_E_INTERNAL;
            goto done;
        }
    }
    if (canon_len < SNAP_CANON_HDR_BYTES + 4u * (size_t)rows ||
        rd_u64(canon + SNAP_CANON_ROWS_OFF) != rows ||
        rd_u32(canon + SNAP_CANON_FC_OFF) != desc->field_count ||
        rd_u64(canon + SNAP_CANON_TICK_OFF) != su_world_tick(w)) {
        st = SU_E_INTERNAL;
        goto done;
    }
    for (i = 0; i < (uint32_t)rows; i++) {
        if (rd_u32(canon + SNAP_CANON_HDR_BYTES + 4u * (size_t)i) != desc->entity_ids[i]) {
            st = SU_E_SCHEMA_MISMATCH;
            goto done;
        }
    }

    /* Committed columns, cross-checked by name. */
    cols = malloc((desc->field_count > 0u ? desc->field_count : 1u) * sizeof *cols);
    if (cols == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (i = 0; i < desc->field_count; i++) {
        const uint32_t *by_id = NULL;
        const uint32_t *by_name = NULL;
        uint64_t r1 = 0;
        uint64_t r2 = 0;
        st = su_world_read_committed(w, sf[i].id, &by_id, &r1);
        if (st != SU_OK) {
            st = SU_E_SCHEMA_MISMATCH;
            goto done;
        }
        if (su_world_read_committed_by_name(w, sf[i].name, &by_name, &r2) != SU_OK ||
            by_name != by_id || r1 != rows || r2 != rows) {
            st = SU_E_SCHEMA_MISMATCH;
            goto done;
        }
        cols[i] = by_id;
    }

    memset(&body, 0, sizeof body);
    body.rev = world_revision;
    body.tick = su_world_tick(w);
    body.schema_id = desc->schema_id;
    body.schema_version = desc->schema_version;
    body.layout_generation = desc->layout_generation;
    body.fields = sf;
    body.field_count = desc->field_count;
    body.row_count = rows;
    body.entity_ids = desc->entity_ids;
    body.columns = (const uint32_t *const *)cols;
    body.manifest = manifest;
    body.manifest_count = manifest_count;

    st = emit_body(&body, &buf);
    if (st != SU_OK) {
        goto done;
    }
    {
        SuSha256 ctx;
        su_sha256_init(&ctx);
        su_sha256_update(&ctx, buf.p, buf.len);
        su_sha256_final(&ctx, out->digest);
        su_sha256_hex(out->digest, out->id);
    }
    out->bytes = buf.p;
    out->len = buf.len;
    out->tick = body.tick;
    buf.p = NULL;

done:
    free(canon);
    free(sf);
    free(cols);
    free(buf.p);
    if (st != SU_OK) {
        memset(out, 0, sizeof *out);
    }
    return st;
}

/* ---- parse ------------------------------------------------------------- */

static int key_eq(const SuJsonMember *m, const char *lit) {
    size_t n = strlen(lit);
    return n == m->key_len && memcmp(m->key, lit, n) == 0;
}

/* Object with exactly these keys: every member is one of them, and the member
 * count matches. Duplicates were rejected by the parser. */
static uint32_t check_keys(const SuJsonValue *obj, const char *const *keys, size_t nkeys) {
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

static uint32_t get_u64(const SuJsonValue *v, uint64_t *out) {
    if (!su_json_get_uint(v, out)) {
        return SU_E_TYPE;
    }
    return SU_OK;
}

static uint32_t get_u32(const SuJsonValue *v, uint32_t *out) {
    uint64_t x = 0;
    if (get_u64(v, &x) != SU_OK || x > UINT32_MAX) {
        return SU_E_TYPE;
    }
    *out = (uint32_t)x;
    return SU_OK;
}

static int str_is(const SuJsonValue *v, const char *lit) {
    size_t n = strlen(lit);
    return v != NULL && v->type == SU_JSON_STRING && v->u.string.len == n &&
           memcmp(v->u.string.data, lit, n) == 0;
}

static void data_free(SuSnapshotData *d) {
    uint32_t i;
    if (d == NULL) {
        return;
    }
    if (d->manifest != NULL) {
        for (i = 0; i < d->manifest_count; i++) {
            free((void *)d->manifest[i].system_id);
        }
    }
    free(d->manifest);
    if (d->columns != NULL) {
        for (i = 0; i < d->field_count; i++) {
            free(d->columns[i]);
        }
    }
    free(d->columns);
    free(d->world_revision);
    memset(d, 0, sizeof *d);
}

void su_snapshot_data_free(SuSnapshotData *d) {
    data_free(d);
}

/* Parses one manifest entry object into dst, copying system_id. */
static uint32_t read_manifest_entry(const SuJsonValue *obj, SuSnapshotManifestEntry *dst) {
    const SuJsonValue *id;
    const SuJsonValue *h;
    char *copy;
    uint32_t st = check_keys(obj, k_manifest_keys, 2u);
    if (st != SU_OK) {
        return st;
    }
    id = su_json_member(obj, "system_id");
    h = su_json_member(obj, "semantic_hash");
    if (id->type != SU_JSON_STRING || !text_ok(id->u.string.data, id->u.string.len, SU_SNAPSHOT_MAX_NAME)) {
        return SU_E_TYPE;
    }
    if (h->type != SU_JSON_STRING || !hash_ok(h->u.string.data, h->u.string.len)) {
        return SU_E_TYPE;
    }
    copy = malloc(id->u.string.len + 1u);
    if (copy == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(copy, id->u.string.data, id->u.string.len);
    copy[id->u.string.len] = '\0';
    dst->system_id = copy;
    memcpy(dst->semantic_hash, h->u.string.data, 64u);
    dst->semantic_hash[64] = '\0';
    return SU_OK;
}

uint32_t su_snapshot_parse(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
                           SuSnapshotData *out, SuSnapshotError *err) {
    SuJsonLimits lim;
    SuJsonDoc doc;
    SuJsonError jerr;
    SuWorldFieldDesc *sf = NULL;
    SuSnapshotData d;
    SnapBody body;
    SnapBuf canon;
    const SuJsonValue *root;
    const SuJsonValue *arr;
    const SuJsonValue *cols;
    uint64_t tick = 0;
    uint64_t prev_id = 0;
    uint32_t i;
    uint32_t st;

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    memset(&d, 0, sizeof d);
    memset(&doc, 0, sizeof doc);
    memset(&canon, 0, sizeof canon);
    if (err != NULL) {
        err->message[0] = '\0';
    }

    if (len > SU_SNAPSHOT_MAX_BYTES) {
        fail_msg(err, "snapshot exceeds the size limit");
        return SU_E_RESOURCE_LIMIT;
    }
    if (bytes == NULL || len == 0u) {
        fail_msg(err, "empty snapshot");
        return SU_E_PARSE;
    }
    st = check_desc(desc, &sf);
    if (st != SU_OK) {
        fail_msg(err, "invalid schema descriptor");
        return st;
    }

    lim.max_bytes = len;
    lim.max_depth = SU_SNAPSHOT_MAX_DEPTH;
    st = su_json_parse(bytes, len, &lim, &doc, &jerr);
    if (st != SU_OK) {
        fail_msg(err, jerr.message);
        goto done;
    }
    root = doc.root;
    st = check_keys(root, k_top_keys, K_TOP_COUNT);
    if (st != SU_OK) {
        fail_msg(err, "top-level keys missing, unknown or not an object");
        goto done;
    }

    if (!str_is(su_json_member(root, "snapshot_format"), SU_SNAPSHOT_FORMAT)) {
        st = SU_E_TYPE;
        fail_msg(err, "unsupported snapshot_format");
        goto done;
    }
    {
        const SuJsonValue *rv = su_json_member(root, "world_revision");
        if (rv->type != SU_JSON_STRING || !text_ok(rv->u.string.data, rv->u.string.len, SU_SNAPSHOT_MAX_NAME)) {
            st = SU_E_TYPE;
            fail_msg(err, "world_revision must be a non-empty string");
            goto done;
        }
        d.world_revision = malloc(rv->u.string.len + 1u);
        if (d.world_revision == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
        memcpy(d.world_revision, rv->u.string.data, rv->u.string.len);
        d.world_revision[rv->u.string.len] = '\0';
    }
    st = get_u64(su_json_member(root, "tick"), &tick);
    if (st != SU_OK) {
        fail_msg(err, "tick must be a non-negative integer");
        goto done;
    }
    if (!str_is(su_json_member(root, "schema_id"), desc->schema_id)) {
        st = SU_E_SCHEMA_MISMATCH;
        fail_msg(err, "schema_id does not match the target world");
        goto done;
    }
    {
        uint32_t v = 0;
        st = get_u32(su_json_member(root, "schema_version"), &v);
        if (st != SU_OK) {
            fail_msg(err, "schema_version must be a u32 integer");
            goto done;
        }
        if (v != desc->schema_version) {
            st = SU_E_SCHEMA_MISMATCH;
            fail_msg(err, "schema_version does not match the target world");
            goto done;
        }
        st = get_u32(su_json_member(root, "layout_generation"), &v);
        if (st != SU_OK) {
            fail_msg(err, "layout_generation must be a u32 integer");
            goto done;
        }
        if (v != desc->layout_generation) {
            st = SU_E_SCHEMA_MISMATCH;
            fail_msg(err, "layout_generation does not match the target world");
            goto done;
        }
    }
    if (!str_is(su_json_member(root, "numeric_profile"), SU_NUMERIC_PROFILE_U32_MOD_V1)) {
        st = SU_E_SCHEMA_MISMATCH;
        fail_msg(err, "numeric_profile is not supported");
        goto done;
    }

    /* fields: exactly the descriptor's table, ascending by id. */
    arr = su_json_member(root, "fields");
    if (arr->type != SU_JSON_ARRAY) {
        st = SU_E_TYPE;
        fail_msg(err, "fields must be an array");
        goto done;
    }
    if (arr->u.array.count != desc->field_count) {
        st = SU_E_SCHEMA_MISMATCH;
        fail_msg(err, "field count does not match the target world");
        goto done;
    }
    for (i = 0; i < desc->field_count; i++) {
        const SuJsonValue *f = arr->u.array.items[i];
        const SuJsonValue *id;
        const SuJsonValue *nm;
        const SuJsonValue *ty;
        uint32_t fid = 0;
        st = check_keys(f, k_field_keys, 3u);
        if (st != SU_OK) {
            fail_msg(err, "field entries need exactly id, name and type");
            goto done;
        }
        id = su_json_member(f, "id");
        nm = su_json_member(f, "name");
        ty = su_json_member(f, "type");
        st = get_u32(id, &fid);
        if (st != SU_OK) {
            fail_msg(err, "field id must be a u32 integer");
            goto done;
        }
        if (i > 0u && (uint64_t)fid <= prev_id) {
            st = SU_E_TYPE;
            fail_msg(err, "fields must be strictly ascending by id");
            goto done;
        }
        prev_id = fid;
        if (nm->type != SU_JSON_STRING || !text_ok(nm->u.string.data, nm->u.string.len, SU_WORLD_NAME_MAX) ||
            !str_is(ty, SNAP_TYPE_NAME)) {
            st = SU_E_TYPE;
            fail_msg(err, "field name or type is malformed");
            goto done;
        }
        if (fid != sf[i].id || nm->u.string.len != strlen(sf[i].name) ||
            memcmp(nm->u.string.data, sf[i].name, nm->u.string.len) != 0) {
            st = SU_E_SCHEMA_MISMATCH;
            fail_msg(err, "field table does not match the target world");
            goto done;
        }
    }

    /* semantic_manifest, in schedule order. */
    arr = su_json_member(root, "semantic_manifest");
    if (arr->type != SU_JSON_ARRAY) {
        st = SU_E_TYPE;
        fail_msg(err, "semantic_manifest must be an array");
        goto done;
    }
    if (arr->u.array.count > SU_SNAPSHOT_MAX_MANIFEST) {
        st = SU_E_RESOURCE_LIMIT;
        fail_msg(err, "semantic_manifest exceeds the entry limit");
        goto done;
    }
    d.manifest_count = (uint32_t)arr->u.array.count;
    if (d.manifest_count > 0u) {
        d.manifest = calloc(d.manifest_count, sizeof *d.manifest);
        if (d.manifest == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
    }
    for (i = 0; i < d.manifest_count; i++) {
        st = read_manifest_entry(arr->u.array.items[i], &d.manifest[i]);
        if (st != SU_OK) {
            fail_msg(err, "semantic_manifest entries need a system_id and a 64-hex semantic_hash");
            goto done;
        }
    }

    /* entity_ids: the row count and identities of the target world. */
    arr = su_json_member(root, "entity_ids");
    if (arr->type != SU_JSON_ARRAY) {
        st = SU_E_TYPE;
        fail_msg(err, "entity_ids must be an array");
        goto done;
    }
    if (arr->u.array.count != desc->row_count) {
        st = SU_E_SCHEMA_MISMATCH;
        fail_msg(err, "row count does not match the target world");
        goto done;
    }
    for (i = 0; i < (uint32_t)desc->row_count; i++) {
        uint32_t ev = 0;
        st = get_u32(arr->u.array.items[i], &ev);
        if (st != SU_OK) {
            fail_msg(err, "entity_ids must be u32 integers");
            goto done;
        }
        if (ev != desc->entity_ids[i]) {
            st = SU_E_SCHEMA_MISMATCH;
            fail_msg(err, "entity_ids do not match the target world");
            goto done;
        }
    }

    /* columns: one array per field, keyed by name, each of row_count values. */
    cols = su_json_member(root, "columns");
    if (cols->type != SU_JSON_OBJECT || cols->u.object.count != desc->field_count) {
        st = SU_E_TYPE;
        fail_msg(err, "columns must be an object with one entry per field");
        goto done;
    }
    for (i = 0; i < cols->u.object.count; i++) {
        uint32_t k;
        int known = 0;
        for (k = 0; k < desc->field_count && !known; k++) {
            known = key_eq(&cols->u.object.members[i], sf[k].name);
        }
        if (!known) {
            st = SU_E_TYPE;
            fail_msg(err, "columns contain an unknown field name");
            goto done;
        }
    }
    d.row_count = desc->row_count;
    d.field_count = desc->field_count;
    if (d.field_count > 0u) {
        d.columns = calloc(d.field_count, sizeof *d.columns);
        if (d.columns == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
    }
    for (i = 0; i < d.field_count; i++) {
        const SuJsonValue *col = su_json_member(cols, sf[i].name);
        uint64_t k;
        if (col == NULL || col->type != SU_JSON_ARRAY || col->u.array.count != d.row_count) {
            st = SU_E_TYPE;
            fail_msg(err, "column is missing or its length differs from the row count");
            goto done;
        }
        d.columns[i] = malloc((d.row_count > 0u ? (size_t)d.row_count : 1u) * sizeof **d.columns);
        if (d.columns[i] == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            goto done;
        }
        for (k = 0; k < d.row_count; k++) {
            st = get_u32(col->u.array.items[k], &d.columns[i][k]);
            if (st != SU_OK) {
                fail_msg(err, "column values must be u32 integers");
                goto done;
            }
        }
    }
    d.tick = tick;

    /* Canonical form: re-encoding the decoded data must reproduce the input
     * byte for byte. This rejects whitespace, key order and escape variants,
     * so the snapshot identity is well defined. */
    memset(&body, 0, sizeof body);
    body.rev = d.world_revision;
    body.tick = d.tick;
    body.schema_id = desc->schema_id;
    body.schema_version = desc->schema_version;
    body.layout_generation = desc->layout_generation;
    body.fields = sf;
    body.field_count = desc->field_count;
    body.row_count = d.row_count;
    body.entity_ids = desc->entity_ids;
    body.columns = (const uint32_t *const *)d.columns;
    body.manifest = d.manifest;
    body.manifest_count = d.manifest_count;
    st = emit_body(&body, &canon);
    if (st != SU_OK) {
        fail_msg(err, "snapshot cannot be re-encoded canonically");
        goto done;
    }
    if (canon.len != len || memcmp(canon.p, bytes, len) != 0) {
        st = SU_E_TYPE;
        fail_msg(err, "snapshot is not in canonical form");
        goto done;
    }
    *out = d;
    memset(&d, 0, sizeof d);
    st = SU_OK;

done:
    if (st != SU_OK) {
        data_free(&d);
    }
    free(canon.p);
    free(sf);
    su_json_doc_free(&doc);
    return st;
}

/* ---- restore ----------------------------------------------------------- */

uint32_t su_snapshot_restore(const SuWorldDesc *desc, const SuSnapshotData *data,
                             SuWorld **out_world,
                             const SuSnapshotManifestEntry **manifest, uint32_t *manifest_count) {
    SuWorldFieldDesc *sf = NULL;
    SuWorld *w = NULL;
    uint32_t i;
    uint64_t t;
    uint32_t st;

    if (out_world == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out_world = NULL;
    if (manifest != NULL) {
        *manifest = NULL;
    }
    if (manifest_count != NULL) {
        *manifest_count = 0u;
    }
    if (data == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = check_desc(desc, &sf);
    if (st != SU_OK) {
        return st;
    }
    if (data->row_count != desc->row_count || data->field_count != desc->field_count ||
        (data->field_count > 0u && data->columns == NULL)) {
        st = SU_E_SCHEMA_MISMATCH;
        goto done;
    }
    st = su_world_create(desc, &w);
    if (st != SU_OK) {
        goto done;
    }
    /* Fresh world at tick 0: committed columns are settable only here. */
    for (i = 0; i < desc->field_count; i++) {
        st = su_world_init_committed(w, sf[i].id, data->columns[i], data->row_count);
        if (st != SU_OK) {
            goto fail_world;
        }
    }
    /* Advance to the snapshot tick with systemless publishes. Each one copies
     * committed to working and back, so the columns are unchanged. */
    for (t = 0; t < data->tick; t++) {
        if (su_world_begin_tick(w) != SU_OK || su_world_publish(w) != SU_OK) {
            st = SU_E_INTERNAL;
            goto fail_world;
        }
    }
    if (su_world_tick(w) != data->tick) {
        st = SU_E_INTERNAL;
        goto fail_world;
    }
    *out_world = w;
    if (manifest != NULL) {
        *manifest = data->manifest;
    }
    if (manifest_count != NULL) {
        *manifest_count = data->manifest_count;
    }
    st = SU_OK;
    free(sf);
    return st;

fail_world:
    su_world_destroy(w);
done:
    free(sf);
    return st;
}
