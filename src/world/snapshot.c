/* Canonical JSON snapshots of a committed world (M0-08; format 0.2 and entity table from M2-00c1).
 * See snapshot.h. */
#include "world/snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_sha256.h"
#include "sutekh/abi.h"
#include "sutekh/ir.h"
#include "world/entities/su_ents.h"

#define SNAP_TYPE_NAME "u32"
#define SNAP_ENT_HEADER_BYTES 60u
#define SNAP_ENT_RECORD_BYTES 9u
#define SNAP_EVENT_TUPLE 8u
#define SNAP_SLOT_TUPLE 3u

static const char *const k_top_01[] = {
    "snapshot_format", "world_revision", "tick", "schema_id", "schema_version",
    "layout_generation", "numeric_profile", "fields", "semantic_manifest",
    "entity_ids", "columns"};
#define K_TOP_01_COUNT (sizeof k_top_01 / sizeof k_top_01[0])

static const char *const k_top_02[] = {
    "snapshot_format", "world_revision", "tick", "schema_id", "schema_version",
    "layout_generation", "numeric_profile", "fields", "semantic_manifest",
    "entity_ids", "columns", "capacity", "high_water", "free_policy", "retired_count",
    "slots", "event_next_seq", "event_dropped", "event_log_max", "events"};
#define K_TOP_02_COUNT (sizeof k_top_02 / sizeof k_top_02[0])

static const char *const k_top_03[] = {
    "snapshot_format", "world_revision", "tick", "schema_id", "schema_version",
    "layout_generation", "numeric_profile", "fields", "semantic_manifest",
    "entity_ids", "columns", "capacity", "high_water", "free_policy", "retired_count",
    "slots", "event_next_seq", "event_dropped", "event_log_max", "events",
    "migration_lineage", "retired_field_ids"};
#define K_TOP_03_COUNT (sizeof k_top_03 / sizeof k_top_03[0])

static const char *const k_field_keys[] = {"id", "name", "type"};
static const char *const k_field_keys_03[] = {"id", "name", "type", "default"};
static const char *const k_manifest_keys[] = {"system_id", "semantic_hash"};

/* Emission-side view of one snapshot. Fields are ascending by id; columns[i] holds row_count values
 * for fields[i]. Format 1 takes entity_ids from the descriptor; format 2 takes the table. */
typedef struct SnapBody {
    const char *rev;
    uint64_t tick;
    const char *schema_id;
    uint32_t schema_version;
    uint32_t layout_generation;
    const SuWorldFieldDesc *fields;
    uint32_t field_count;
    uint32_t format_minor;
    uint64_t row_count;          /* columns length: format 1 row count, format 2 high_water */
    const uint32_t *entity_ids;  /* format 1 only */
    const SuEnts *ents;          /* format 2 only */
    const uint32_t *const *columns;
    const SuSnapshotManifestEntry *manifest;
    uint32_t manifest_count;
    uint64_t event_next_seq;     /* format 2 only */
    uint64_t event_dropped;
    uint64_t event_log_max;
    const SuEvent *events;
    uint64_t event_count;
    const uint32_t *defaults;        /* format 3: per field, desc order */
    const uint8_t (*lineage)[32];    /* format 3 */
    uint32_t lineage_count;
    const uint32_t *retired;         /* format 3, ascending */
    uint32_t retired_count;
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

/* Event log state: log_max in 1..limit, retained events contiguous and ending at next_seq - 1. */
static uint32_t check_events(const SuSnapshotEvents *ev) {
    uint64_t i;
    if (ev->log_max == 0u || ev->log_max > SU_SNAPSHOT_EVENT_LOG_MAX_LIMIT) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (ev->count > ev->log_max || ev->count > ev->next_seq) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (ev->count > 0u) {
        if (ev->entries == NULL || ev->entries[0].event_seq != ev->next_seq - ev->count) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (i = 1; i < ev->count; i++) {
            if (ev->entries[i].event_seq != ev->entries[0].event_seq + i) {
                return SU_E_INVALID_ARGUMENT;
            }
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

/* Appends one byte. The hot emitter writes single punctuation bytes through here, not buf_lit. */
static inline uint32_t buf_ch(SnapBuf *b, char c) {
    if (b->len < b->cap) {
        b->p[b->len++] = (uint8_t)c;
        return SU_OK;
    }
    {
        uint32_t st = buf_reserve(b, 1u);
        if (st != SU_OK) {
            return st;
        }
        b->p[b->len++] = (uint8_t)c;
    }
    return SU_OK;
}

/* Decimal text of v, identical to "%llu". The u32 path (every column value and entity id) counts the digits,
 * then writes them in place from the end two at a time through a table: no temporary and no per-byte copy.
 * Values above 32 bits take the plain division loop through a stack tail. */
static uint32_t buf_dec(SnapBuf *b, uint64_t v) {
    static const char dig2[] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859606162636465666768697071727374757677787980"
        "81828384858687888990919293949596979899";
    static const uint32_t p10[9] = {10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u,
                                    1000000000u};
    uint32_t u;
    size_t n = 1;
    uint8_t *q;
    uint32_t st;

    if (v > UINT32_MAX) {
        char t[24];
        char *e = t + sizeof t;
        char *p = e;
        do {
            *--p = (char)('0' + (char)(v % 10u));
            v /= 10u;
        } while (v != 0u);
        return buf_bytes(b, p, (size_t)(e - p));
    }
    u = (uint32_t)v;
    st = buf_reserve(b, 10u);
    if (st != SU_OK) {
        return st;
    }
    while (n < 10u && u >= p10[n - 1u]) {
        n++;
    }
    q = b->p + b->len + n;
    while (u >= 100u) {
        uint32_t r = (u % 100u) * 2u;
        u /= 100u;
        *--q = (uint8_t)dig2[r + 1u];
        *--q = (uint8_t)dig2[r];
    }
    if (u >= 10u) {
        *--q = (uint8_t)dig2[u * 2u + 1u];
        *--q = (uint8_t)dig2[u * 2u];
    } else {
        *--q = (uint8_t)('0' + u);
    }
    b->len += n;
    return SU_OK;
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

/* External id of slot: the live entity's id, or 0 for a dead slot (canonical). */
static uint32_t slot_external(const SuEnts *e, uint32_t slot, uint32_t *ext) {
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t st;
    if (!su_ents_slot_alive(e, slot)) {
        *ext = 0u;
        return SU_OK;
    }
    st = su_ents_handle_for_slot(e, slot, &h);
    if (st != SU_OK) {
        return st;
    }
    return su_ents_get_external(e, h, ext);
}

static uint32_t emit_body(const SnapBody *s, SnapBuf *o) {
    uint32_t st;
    uint32_t i;
    uint64_t r;
    uint64_t k;

#define SNAP_W(expr)                                                          \
    do {                                                                      \
        st = (expr);                                                          \
        if (st != SU_OK) {                                                    \
            return st;                                                        \
        }                                                                     \
    } while (0)

    SNAP_W(buf_lit(o, "{\"snapshot_format\":\""));
    SNAP_W(buf_lit(o, s->format_minor == 1u ? SU_SNAPSHOT_FORMAT_01
                   : s->format_minor == 3u  ? SU_SNAPSHOT_FORMAT_03
                                            : SU_SNAPSHOT_FORMAT));
    SNAP_W(buf_lit(o, "\",\"world_revision\":"));
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
            SNAP_W(buf_ch(o, ','));
        }
        SNAP_W(buf_lit(o, "{\"id\":"));
        SNAP_W(buf_dec(o, s->fields[i].id));
        SNAP_W(buf_lit(o, ",\"name\":"));
        SNAP_W(buf_jstr(o, s->fields[i].name));
        SNAP_W(buf_lit(o, ",\"type\":\"" SNAP_TYPE_NAME "\""));
        if (s->format_minor == 3u) {
            SNAP_W(buf_lit(o, ",\"default\":"));
            SNAP_W(buf_dec(o, s->defaults[i]));
        }
        SNAP_W(buf_ch(o, '}'));
    }
    SNAP_W(buf_lit(o, "],\"semantic_manifest\":["));
    for (i = 0; i < s->manifest_count; i++) {
        if (i > 0u) {
            SNAP_W(buf_ch(o, ','));
        }
        SNAP_W(buf_lit(o, "{\"system_id\":"));
        SNAP_W(buf_jstr(o, s->manifest[i].system_id));
        SNAP_W(buf_lit(o, ",\"semantic_hash\":\""));
        SNAP_W(buf_bytes(o, s->manifest[i].semantic_hash, 64u));
        SNAP_W(buf_lit(o, "\"}"));
    }
    SNAP_W(buf_lit(o, "],\"entity_ids\":["));
    for (r = 0; r < s->row_count; r++) {
        uint32_t id = 0;
        if (r > 0u) {
            SNAP_W(buf_ch(o, ','));
        }
        if (s->format_minor == 1u) {
            id = s->entity_ids[r];
        } else {
            SNAP_W(slot_external(s->ents, (uint32_t)r, &id));
        }
        SNAP_W(buf_dec(o, id));
    }
    SNAP_W(buf_lit(o, "],\"columns\":{"));
    for (i = 0; i < s->field_count; i++) {
        if (i > 0u) {
            SNAP_W(buf_ch(o, ','));
        }
        SNAP_W(buf_jstr(o, s->fields[i].name));
        SNAP_W(buf_lit(o, ":["));
        for (k = 0; k < s->row_count; k++) {
            if (k > 0u) {
                SNAP_W(buf_ch(o, ','));
            }
            SNAP_W(buf_dec(o, s->columns[i][k]));
        }
        SNAP_W(buf_ch(o, ']'));
    }
    SNAP_W(buf_ch(o, '}'));
    if (s->format_minor >= 2u) {
        uint32_t hw = su_ents_high_water(s->ents);
        if (hw != s->row_count) {
            return SU_E_INTERNAL;
        }
        SNAP_W(buf_lit(o, ",\"capacity\":"));
        SNAP_W(buf_dec(o, su_ents_capacity(s->ents)));
        SNAP_W(buf_lit(o, ",\"high_water\":"));
        SNAP_W(buf_dec(o, hw));
        SNAP_W(buf_lit(o, ",\"free_policy\":\"" SU_ENTS_FREE_POLICY "\""));
        SNAP_W(buf_lit(o, ",\"retired_count\":"));
        SNAP_W(buf_dec(o, su_ents_retired_count(s->ents)));
        SNAP_W(buf_lit(o, ",\"slots\":["));
        for (i = 0; i < hw; i++) {
            uint32_t gen = 0;
            uint32_t ext = 0;
            if (i > 0u) {
                SNAP_W(buf_ch(o, ','));
            }
            SNAP_W(su_ents_slot_generation(s->ents, i, &gen));
            SNAP_W(slot_external(s->ents, i, &ext));
            SNAP_W(buf_ch(o, '['));
            SNAP_W(buf_dec(o, gen));
            SNAP_W(buf_lit(o, su_ents_slot_alive(s->ents, i) ? ",1," : ",0,"));
            SNAP_W(buf_dec(o, ext));
            SNAP_W(buf_ch(o, ']'));
        }
        SNAP_W(buf_lit(o, "],\"event_next_seq\":"));
        SNAP_W(buf_dec(o, s->event_next_seq));
        SNAP_W(buf_lit(o, ",\"event_dropped\":"));
        SNAP_W(buf_dec(o, s->event_dropped));
        SNAP_W(buf_lit(o, ",\"event_log_max\":"));
        SNAP_W(buf_dec(o, s->event_log_max));
        SNAP_W(buf_lit(o, ",\"events\":["));
        for (k = 0; k < s->event_count; k++) {
            const SuEvent *ev = &s->events[k];
            if (k > 0u) {
                SNAP_W(buf_ch(o, ','));
            }
            SNAP_W(buf_ch(o, '['));
            SNAP_W(buf_dec(o, ev->event_seq));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->tick));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->kind));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->system_index));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->slot));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->seq));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->a));
            SNAP_W(buf_ch(o, ','));
            SNAP_W(buf_dec(o, ev->b));
            SNAP_W(buf_ch(o, ']'));
        }
        SNAP_W(buf_ch(o, ']'));
    }
    if (s->format_minor == 3u) {
        char hex[65];
        SNAP_W(buf_lit(o, ",\"migration_lineage\":["));
        for (i = 0; i < s->lineage_count; i++) {
            if (i > 0u) {
                SNAP_W(buf_ch(o, ','));
            }
            su_sha256_hex(s->lineage[i], hex);
            SNAP_W(buf_lit(o, "\""));
            SNAP_W(buf_bytes(o, hex, 64u));
            SNAP_W(buf_lit(o, "\""));
        }
        SNAP_W(buf_lit(o, "],\"retired_field_ids\":["));
        for (i = 0; i < s->retired_count; i++) {
            if (i > 0u) {
                SNAP_W(buf_ch(o, ','));
            }
            SNAP_W(buf_dec(o, s->retired[i]));
        }
        SNAP_W(buf_ch(o, ']'));
    }
    SNAP_W(buf_ch(o, '}'));
#undef SNAP_W
    return SU_OK;
}

/* ---- lifetime --------------------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

void su_snapshot_destroy(SuSnapshot *s) {
    if (s == NULL) {
        return;
    }
    free(s->bytes);
    memset(s, 0, sizeof *s);
}

/* ---- capture ----------------------------------------------------------- */

uint32_t su_snapshot_capture(const SuWorld *w, const SuWorldDesc *desc,
                             const char *world_revision,
                             const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                             SuSnapshot *out) {
    return su_snapshot_capture_ex(w, desc, world_revision, manifest, manifest_count, NULL, out);
}

uint32_t su_snapshot_capture_ex(const SuWorld *w, const SuWorldDesc *desc,
                                const char *world_revision,
                                const SuSnapshotManifestEntry *manifest, uint32_t manifest_count,
                                const SuSnapshotEvents *events, SuSnapshot *out) {
    SuWorldFieldDesc *sf = NULL;
    const uint32_t **cols = NULL;
    const SuEnts *ents;
    SnapBody body;
    SnapBuf buf;
    SuSnapshotEvents empty;
    const SuSnapshotEvents *ev;
    uint64_t cap;
    uint64_t hw;
    uint32_t i;
    uint32_t st = SU_OK;
    uint32_t *defaults = NULL;
    const uint8_t (*hist_lineage)[32] = NULL;
    uint32_t hist_lineage_n = 0;
    const uint32_t *hist_retired = NULL;
    uint32_t hist_retired_n = 0;

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
    if (events == NULL) {
        memset(&empty, 0, sizeof empty);
        empty.log_max = SU_SNAPSHOT_DEFAULT_EVENT_LOG_MAX;
        ev = &empty;
    } else {
        ev = events;
    }
    st = check_events(ev);
    if (st != SU_OK) {
        goto done;
    }
    ents = su_world_entities_committed(w);
    cap = su_ents_capacity(ents);
    hw = su_ents_high_water(ents);
    if (desc->row_count > cap || su_world_row_count(w) != cap ||
        su_world_field_count(w) != desc->field_count ||
        su_world_layout_generation(w) != desc->layout_generation) {
        st = SU_E_SCHEMA_MISMATCH;
        goto done;
    }
    /* Cheap lower bound on the encoded size (two bytes per value), checked before any copy. */
    if (hw * ((uint64_t)desc->field_count + 1u) > SU_SNAPSHOT_MAX_BYTES / 2u) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
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
            by_name != by_id || r1 != cap || r2 != cap) {
            st = SU_E_SCHEMA_MISMATCH;
            goto done;
        }
        cols[i] = by_id;
    }
    /* The descriptor's entity IDs must agree with the creation identities it describes: a slot that is
     * alive at its first generation has never been reused, so its external id must be the descriptor's. */
    for (uint64_t r = 0; r < desc->row_count; r++) {
        uint32_t ext = 0;
        uint32_t gen = 0;
        st = slot_external(ents, (uint32_t)r, &ext);
        if (st == SU_OK) {
            st = su_ents_slot_generation(ents, (uint32_t)r, &gen);
        }
        if (st != SU_OK) {
            goto done;
        }
        if (su_ents_slot_alive(ents, (uint32_t)r) && gen == SU_GEN_FIRST && ext != desc->entity_ids[r]) {
            st = SU_E_SCHEMA_MISMATCH;
            goto done;
        }
    }

    /* Schema history (M2-01a): format 0.3 only when the lineage or the retired IDs are non-empty. */
    {
        const uint8_t (*lin)[32] = NULL;
        const uint32_t *ret = NULL;
        uint32_t lin_n = 0;
        uint32_t ret_n = 0;
        if (su_world_lineage(w, &lin, &lin_n) != SU_OK || su_world_retired_ids(w, &ret, &ret_n) != SU_OK) {
            st = SU_E_INTERNAL;
            goto done;
        }
        hist_lineage = lin;
        hist_lineage_n = lin_n;
        hist_retired = ret;
        hist_retired_n = ret_n;
        if (desc->field_count > 0u) {
            defaults = malloc(desc->field_count * sizeof *defaults);
            if (defaults == NULL) {
                st = SU_E_RESOURCE_LIMIT;
                goto done;
            }
        }
        for (i = 0; i < desc->field_count; i++) {
            if (su_world_field_default(w, sf[i].id, &defaults[i]) != SU_OK) {
                st = SU_E_SCHEMA_MISMATCH;
                goto done;
            }
        }
    }

    memset(&body, 0, sizeof body);
    body.rev = world_revision;
    body.tick = su_world_tick(w);
    body.schema_id = desc->schema_id;
    body.schema_version = desc->schema_version;
    body.layout_generation = desc->layout_generation;
    body.fields = sf;
    body.field_count = desc->field_count;
    body.format_minor = (hist_lineage_n > 0u || hist_retired_n > 0u) ? 3u : 2u;
    body.defaults = defaults;
    body.lineage = hist_lineage;
    body.lineage_count = hist_lineage_n;
    body.retired = hist_retired;
    body.retired_count = hist_retired_n;
    body.row_count = hw;
    body.ents = ents;
    body.columns = cols;
    body.manifest = manifest;
    body.manifest_count = manifest_count;
    body.event_next_seq = ev->next_seq;
    body.event_dropped = ev->dropped;
    body.event_log_max = ev->log_max;
    body.events = ev->entries;
    body.event_count = ev->count;

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
    free(cols);
    free(sf);
    free(defaults);
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
    su_ents_destroy(d->entities);
    free(d->events);
    free(d->field_defaults);
    free(d->lineage);
    free(d->retired);
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

/* Builds an entity table from per-slot records through the canonical parser, so every table invariant
 * is checked by one implementation. */
static uint32_t build_table(uint64_t capacity, uint64_t hw, const uint32_t *gen, const uint8_t *alive,
                            const uint32_t *ext, uint64_t retired, SuEnts **out) {
    size_t total;
    size_t i;
    uint64_t alive_count = 0;
    uint8_t *b;
    uint32_t st;
    *out = NULL;
    if (hw > capacity || capacity > SU_LIMIT_ROWS) {
        return SU_E_TYPE;
    }
    total = SNAP_ENT_HEADER_BYTES + (size_t)hw * SNAP_ENT_RECORD_BYTES;
    b = malloc(total);
    if (b == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(b, "SUENTABL", 8u);
    b[8] = 0u;
    put_u32(b + 9, 1u);
    put_u32(b + 13, (uint32_t)strlen(SU_ENTS_FREE_POLICY));
    memcpy(b + 17, SU_ENTS_FREE_POLICY, strlen(SU_ENTS_FREE_POLICY));
    for (i = 0; i < hw; i++) {
        alive_count += alive[i] ? 1u : 0u;
    }
    put_u64(b + 28, capacity);
    put_u64(b + 36, hw);
    put_u64(b + 44, alive_count);
    put_u64(b + 52, retired);
    for (i = 0; i < hw; i++) {
        uint8_t *r = b + SNAP_ENT_HEADER_BYTES + i * SNAP_ENT_RECORD_BYTES;
        put_u32(r, gen[i]);
        put_u32(r + 4, ext[i]);
        r[8] = alive[i] ? 1u : 0u;
    }
    st = su_ents_canonical_parse(b, total, out);
    free(b);
    if (st == SU_E_RESOURCE_LIMIT) {
        return st;
    }
    return st == SU_OK ? SU_OK : SU_E_TYPE;
}

/* Reads a slot record [generation, alive, external_id]. */
static uint32_t read_slot(const SuJsonValue *v, uint32_t *gen, uint8_t *alive, uint32_t *ext) {
    uint64_t a = 0;
    if (v == NULL || v->type != SU_JSON_ARRAY || v->u.array.count != SNAP_SLOT_TUPLE) {
        return SU_E_TYPE;
    }
    if (get_u32(v->u.array.items[0], gen) != SU_OK || get_u64(v->u.array.items[1], &a) != SU_OK ||
        a > 1u || get_u32(v->u.array.items[2], ext) != SU_OK) {
        return SU_E_TYPE;
    }
    *alive = (uint8_t)a;
    return SU_OK;
}

/* Reads one event tuple [event_seq, tick, kind, system_index, slot, seq, a, b]. */
static uint32_t read_event(const SuJsonValue *v, SuEvent *ev) {
    const SuJsonValue *it[SNAP_EVENT_TUPLE];
    uint32_t k;
    if (v == NULL || v->type != SU_JSON_ARRAY || v->u.array.count != SNAP_EVENT_TUPLE) {
        return SU_E_TYPE;
    }
    for (k = 0; k < SNAP_EVENT_TUPLE; k++) {
        it[k] = v->u.array.items[k];
    }
    if (get_u64(it[0], &ev->event_seq) != SU_OK || get_u64(it[1], &ev->tick) != SU_OK ||
        get_u32(it[2], &ev->kind) != SU_OK || get_u32(it[3], &ev->system_index) != SU_OK ||
        get_u32(it[4], &ev->slot) != SU_OK || get_u32(it[5], &ev->seq) != SU_OK ||
        get_u32(it[6], &ev->a) != SU_OK || get_u32(it[7], &ev->b) != SU_OK) {
        return SU_E_TYPE;
    }
    return SU_OK;
}

/* Strict parse against desc (formats 0.1, 0.2 and 0.3). Shared by the public entry points. */
static uint32_t parse_core(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
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
    const SuJsonValue *fmt;
    uint64_t tick = 0;
    uint64_t prev_id = 0;
    uint64_t capacity = 0;
    uint64_t hw = 0;
    uint64_t retired = 0;
    uint64_t rows = 0;
    uint32_t minor = 0;
    uint32_t *gen = NULL;
    uint8_t *alive = NULL;
    uint32_t *ext = NULL;
    SuEnts *table = NULL;
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
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        st = SU_E_TYPE;
        fail_msg(err, "snapshot must be a JSON object");
        goto done;
    }
    fmt = su_json_member(root, "snapshot_format");
    if (str_is(fmt, SU_SNAPSHOT_FORMAT_01)) {
        minor = 1u;
    } else if (str_is(fmt, SU_SNAPSHOT_FORMAT)) {
        minor = 2u;
    } else if (str_is(fmt, SU_SNAPSHOT_FORMAT_03)) {
        minor = 3u;
    } else {
        st = SU_E_TYPE;
        fail_msg(err, "unsupported snapshot_format");
        goto done;
    }
    st = minor == 1u   ? check_keys(root, k_top_01, K_TOP_01_COUNT)
         : minor == 2u ? check_keys(root, k_top_02, K_TOP_02_COUNT)
                       : check_keys(root, k_top_03, K_TOP_03_COUNT);
    if (st != SU_OK) {
        fail_msg(err, "top-level keys missing, unknown or not an object");
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
    d.field_defaults = calloc(desc->field_count > 0u ? desc->field_count : 1u, sizeof *d.field_defaults);
    if (d.field_defaults == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto done;
    }
    for (i = 0; i < desc->field_count; i++) {
        const SuJsonValue *f = arr->u.array.items[i];
        const SuJsonValue *id;
        const SuJsonValue *nm;
        const SuJsonValue *ty;
        uint32_t fid = 0;
        st = minor == 3u ? check_keys(f, k_field_keys_03, 4u) : check_keys(f, k_field_keys, 3u);
        if (st != SU_OK) {
            fail_msg(err, "field entries need exactly id, name and type (and default in 0.3)");
            goto done;
        }
        if (minor == 3u) {
            st = get_u32(su_json_member(f, "default"), &d.field_defaults[i]);
            if (st != SU_OK) {
                fail_msg(err, "field default must be a u32 integer");
                goto done;
            }
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

    /* Schema history (format 0.3): lineage and retired IDs. */
    if (minor == 3u) {
        uint32_t k;
        arr = su_json_member(root, "migration_lineage");
        if (arr->type != SU_JSON_ARRAY || arr->u.array.count > SU_WORLD_MAX_HISTORY) {
            st = arr->type != SU_JSON_ARRAY ? SU_E_TYPE : SU_E_RESOURCE_LIMIT;
            fail_msg(err, "migration_lineage must be an array of at most the history limit");
            goto done;
        }
        d.lineage_count = (uint32_t)arr->u.array.count;
        if (d.lineage_count > 0u) {
            d.lineage = malloc(d.lineage_count * sizeof *d.lineage);
            if (d.lineage == NULL) {
                st = SU_E_RESOURCE_LIMIT;
                goto done;
            }
        }
        for (k = 0; k < d.lineage_count; k++) {
            const SuJsonValue *h = arr->u.array.items[k];
            uint32_t b;
            if (h->type != SU_JSON_STRING || !hash_ok(h->u.string.data, h->u.string.len)) {
                st = SU_E_TYPE;
                fail_msg(err, "migration_lineage entries must be 64 lowercase hex characters");
                goto done;
            }
            for (b = 0; b < 32u; b++) {
                int hi = h->u.string.data[2u * b];
                int lo = h->u.string.data[2u * b + 1u];
                hi = hi <= '9' ? hi - '0' : hi - 'a' + 10;
                lo = lo <= '9' ? lo - '0' : lo - 'a' + 10;
                d.lineage[k][b] = (uint8_t)((hi << 4) | lo);
            }
        }
        arr = su_json_member(root, "retired_field_ids");
        if (arr->type != SU_JSON_ARRAY || arr->u.array.count > SU_WORLD_MAX_HISTORY) {
            st = arr->type != SU_JSON_ARRAY ? SU_E_TYPE : SU_E_RESOURCE_LIMIT;
            fail_msg(err, "retired_field_ids must be an array of at most the history limit");
            goto done;
        }
        d.retired_count = (uint32_t)arr->u.array.count;
        if (d.retired_count > 0u) {
            d.retired = malloc(d.retired_count * sizeof *d.retired);
            if (d.retired == NULL) {
                st = SU_E_RESOURCE_LIMIT;
                goto done;
            }
        }
        for (k = 0; k < d.retired_count; k++) {
            uint32_t rid = 0;
            uint32_t f;
            st = get_u32(arr->u.array.items[k], &rid);
            if (st != SU_OK || (k > 0u && rid <= d.retired[k - 1u])) {
                st = SU_E_TYPE;
                fail_msg(err, "retired_field_ids must be u32 integers, strictly ascending");
                goto done;
            }
            for (f = 0; f < desc->field_count; f++) {
                if (sf[f].id == rid) {
                    st = SU_E_SCHEMA_MISMATCH;
                    fail_msg(err, "a retired field ID is a live field of the schema");
                    goto done;
                }
            }
            d.retired[k] = rid;
        }
        if (d.lineage_count == 0u && d.retired_count == 0u) {
            st = SU_E_TYPE;
            fail_msg(err, "format 0.3 requires a non-empty lineage or retired list");
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

    /* Entity table, stored rows and event log. */
    arr = su_json_member(root, "entity_ids");
    if (arr->type != SU_JSON_ARRAY) {
        st = SU_E_TYPE;
        fail_msg(err, "entity_ids must be an array");
        goto done;
    }
    if (minor == 1u) {
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
        capacity = desc->row_count;
        hw = desc->row_count;
        rows = hw;
        if (rows > 0u) {
            gen = malloc(rows * sizeof *gen);
            alive = malloc(rows);
            ext = malloc(rows * sizeof *ext);
            if (gen == NULL || alive == NULL || ext == NULL) {
                st = SU_E_RESOURCE_LIMIT;
                goto done;
            }
        }
        for (i = 0; i < (uint32_t)rows; i++) {
            gen[i] = 1u;
            alive[i] = 1u;
            ext[i] = desc->entity_ids[i];
        }
        st = build_table(capacity, hw, gen, alive, ext, 0u, &table);
        if (st != SU_OK) {
            fail_msg(err, "entity table cannot be built");
            goto done;
        }
        d.entities = table;
        table = NULL;
    } else {
        uint64_t ev_count = 0;
        uint64_t log_max = 0;
        uint64_t next_seq = 0;
        const SuJsonValue *slots;
        if (get_u64(su_json_member(root, "capacity"), &capacity) != SU_OK || capacity > SU_LIMIT_ROWS) {
            st = SU_E_RESOURCE_LIMIT;
            fail_msg(err, "capacity is missing or over the limit");
            goto done;
        }
        if (capacity < desc->row_count) {
            st = SU_E_SCHEMA_MISMATCH;
            fail_msg(err, "capacity is below the target world's row count");
            goto done;
        }
        if (get_u64(su_json_member(root, "high_water"), &hw) != SU_OK || hw > capacity) {
            st = SU_E_TYPE;
            fail_msg(err, "high_water must be an integer at most capacity");
            goto done;
        }
        if (!str_is(su_json_member(root, "free_policy"), SU_ENTS_FREE_POLICY)) {
            st = SU_E_TYPE;
            fail_msg(err, "free_policy is not supported");
            goto done;
        }
        if (get_u64(su_json_member(root, "retired_count"), &retired) != SU_OK || retired > hw) {
            st = SU_E_TYPE;
            fail_msg(err, "retired_count must be an integer at most high_water");
            goto done;
        }
        rows = hw;
        slots = su_json_member(root, "slots");
        if (slots->type != SU_JSON_ARRAY || slots->u.array.count != hw) {
            st = SU_E_TYPE;
            fail_msg(err, "slots must hold one record per slot below high_water");
            goto done;
        }
        if (rows > 0u) {
            gen = malloc(rows * sizeof *gen);
            alive = malloc(rows);
            ext = malloc(rows * sizeof *ext);
            if (gen == NULL || alive == NULL || ext == NULL) {
                st = SU_E_RESOURCE_LIMIT;
                goto done;
            }
        }
        for (i = 0; i < (uint32_t)rows; i++) {
            st = read_slot(slots->u.array.items[i], &gen[i], &alive[i], &ext[i]);
            if (st != SU_OK) {
                fail_msg(err, "slot records need [generation, alive 0|1, external_id]");
                goto done;
            }
        }
        if (arr->u.array.count != hw) {
            st = SU_E_SCHEMA_MISMATCH;
            fail_msg(err, "row count does not match the high_water of the snapshot");
            goto done;
        }
        for (i = 0; i < (uint32_t)rows; i++) {
            uint32_t ev = 0;
            st = get_u32(arr->u.array.items[i], &ev);
            if (st != SU_OK) {
                fail_msg(err, "entity_ids must be u32 integers");
                goto done;
            }
            if (i < desc->row_count && alive[i] && gen[i] == SU_GEN_FIRST && ev != desc->entity_ids[i]) {
                st = SU_E_SCHEMA_MISMATCH;
                fail_msg(err, "entity_ids do not match the target world");
                goto done;
            }
        }
        for (i = 0; i < (uint32_t)rows; i++) {
            uint32_t ev = 0;
            (void)get_u32(arr->u.array.items[i], &ev);
            if (ev != ext[i]) {
                st = SU_E_TYPE;
                fail_msg(err, "entity_ids disagree with the slot records");
                goto done;
            }
        }
        st = build_table(capacity, hw, gen, alive, ext, retired, &table);
        if (st != SU_OK) {
            fail_msg(err, "entity table is not canonical");
            goto done;
        }
        if (su_ents_retired_count(table) != retired) {
            st = SU_E_TYPE;
            fail_msg(err, "retired_count disagrees with the slot records");
            goto done;
        }
        d.entities = table;
        table = NULL;

        if (get_u64(su_json_member(root, "event_next_seq"), &next_seq) != SU_OK ||
            get_u64(su_json_member(root, "event_dropped"), &d.event_dropped) != SU_OK ||
            get_u64(su_json_member(root, "event_log_max"), &log_max) != SU_OK ||
            log_max == 0u || log_max > SU_SNAPSHOT_EVENT_LOG_MAX_LIMIT) {
            st = SU_E_TYPE;
            fail_msg(err, "event log counters are malformed or out of range");
            goto done;
        }
        arr = su_json_member(root, "events");
        if (arr->type != SU_JSON_ARRAY || arr->u.array.count > log_max || arr->u.array.count > next_seq) {
            st = SU_E_TYPE;
            fail_msg(err, "events must be an array no longer than the log");
            goto done;
        }
        ev_count = arr->u.array.count;
        if (ev_count > 0u) {
            d.events = malloc(ev_count * sizeof *d.events);
            if (d.events == NULL) {
                st = SU_E_RESOURCE_LIMIT;
                goto done;
            }
        }
        for (i = 0; i < (uint32_t)ev_count; i++) {
            st = read_event(arr->u.array.items[i], &d.events[i]);
            if (st != SU_OK) {
                fail_msg(err, "event entries need eight integers");
                goto done;
            }
        }
        if (ev_count > 0u) {
            uint64_t k;
            if (d.events[0].event_seq != next_seq - ev_count) {
                st = SU_E_TYPE;
                fail_msg(err, "events must end at event_next_seq - 1");
                goto done;
            }
            for (k = 1; k < ev_count; k++) {
                if (d.events[k].event_seq != d.events[0].event_seq + k) {
                    st = SU_E_TYPE;
                    fail_msg(err, "events must have consecutive sequence numbers");
                    goto done;
                }
            }
        }
        d.event_next_seq = next_seq;
        d.event_log_max = log_max;
        d.event_count = ev_count;
    }

    /* columns: one array per field, keyed by name, each of rows values. */
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
    d.row_count = rows;
    d.capacity = capacity;
    d.format_minor = minor;
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
            /* Dead rows are canonically zero. */
            if (d.columns[i][k] != 0u && !su_ents_slot_alive(d.entities, (uint32_t)k)) {
                st = SU_E_TYPE;
                fail_msg(err, "dead rows must hold zero values");
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
    body.format_minor = minor;
    body.row_count = d.row_count;
    body.entity_ids = desc->entity_ids;
    body.ents = d.entities;
    body.columns = (const uint32_t *const *)d.columns;
    body.manifest = d.manifest;
    body.manifest_count = d.manifest_count;
    body.event_next_seq = d.event_next_seq;
    body.event_dropped = d.event_dropped;
    body.event_log_max = d.event_log_max;
    body.events = d.events;
    body.event_count = d.event_count;
    body.defaults = d.field_defaults;
    body.lineage = (const uint8_t (*)[32])d.lineage;
    body.lineage_count = d.lineage_count;
    body.retired = d.retired;
    body.retired_count = d.retired_count;
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
    su_ents_destroy(table);
    free(gen);
    free(alive);
    free(ext);
    free(canon.p);
    free(sf);
    su_json_doc_free(&doc);
    return st;
}

uint32_t su_snapshot_parse(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
                           SuSnapshotData *out, SuSnapshotError *err) {
    return parse_core(bytes, len, desc, out, err);
}

/* ---- restore ----------------------------------------------------------- */

uint32_t su_snapshot_restore(const SuWorldDesc *desc, const SuSnapshotData *data,
                             SuWorld **out_world,
                             const SuSnapshotManifestEntry **manifest, uint32_t *manifest_count) {
    SuWorldSchemaExt ext;
    SuWorldFieldDesc *sf = NULL;
    SuWorld *w = NULL;
    uint32_t *pad = NULL;
    uint64_t cap;
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
    if (data->entities == NULL) {
        /* Empty data (a rejected or unfilled parse) does not fit any world. */
        st = SU_E_SCHEMA_MISMATCH;
        goto done;
    }
    cap = su_ents_capacity(data->entities);
    if (data->field_count != desc->field_count || (data->field_count > 0u && data->columns == NULL) ||
        cap != data->capacity || cap < desc->row_count || data->row_count != su_ents_high_water(data->entities)) {
        st = SU_E_SCHEMA_MISMATCH;
        goto done;
    }
    ext.defaults = data->field_defaults;
    ext.retired_ids = data->retired;
    ext.retired_count = data->retired_count;
    ext.lineage = (const uint8_t (*)[32])data->lineage;
    ext.lineage_count = data->lineage_count;
    st = su_world_create_ext(desc, cap, &ext, &w);
    if (st != SU_OK) {
        goto done;
    }
    /* Fresh world at tick 0: the table and committed columns are settable only here. */
    st = su_world_adopt_entities(w, data->entities);
    if (st != SU_OK) {
        goto fail_world;
    }
    pad = calloc(cap > 0u ? (size_t)cap : 1u, sizeof *pad);
    if (pad == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        goto fail_world;
    }
    for (i = 0; i < desc->field_count; i++) {
        memset(pad, 0, (size_t)cap * sizeof *pad);
        if (data->row_count > 0u) {
            memcpy(pad, data->columns[i], (size_t)data->row_count * sizeof *pad);
        }
        st = su_world_init_committed(w, sf[i].id, pad, cap);
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
    free(pad);
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
    free(pad);
    su_world_destroy(w);
done:
    free(sf);
    return st;
}

/* ---- schema from the header (M2-01a) ----------------------------------- */

/* Builds a descriptor from the snapshot header alone: schema_id, schema_version, layout_generation and the
 * field table. Names point into doc, which the caller keeps alive. Access is SU_ACCESS_READ for every field.
 * Only the header is checked here; parse_core performs every other check. */
static uint32_t header_desc(const uint8_t *bytes, size_t len, SuJsonDoc *doc, SuWorldDesc *desc,
                            SuWorldFieldDesc **fields, SuSnapshotError *err) {
    SuJsonLimits lim;
    SuJsonError jerr;
    const SuJsonValue *root;
    const SuJsonValue *sid;
    const SuJsonValue *arr;
    uint32_t ver = 0;
    uint32_t gen = 0;
    uint32_t n;
    uint32_t i;
    uint32_t st;

    memset(doc, 0, sizeof *doc);
    *fields = NULL;
    memset(desc, 0, sizeof *desc);
    if (len > SU_SNAPSHOT_MAX_BYTES) {
        fail_msg(err, "snapshot exceeds the size limit");
        return SU_E_RESOURCE_LIMIT;
    }
    if (bytes == NULL || len == 0u) {
        fail_msg(err, "empty snapshot");
        return SU_E_PARSE;
    }
    lim.max_bytes = len;
    lim.max_depth = SU_SNAPSHOT_MAX_DEPTH;
    st = su_json_parse(bytes, len, &lim, doc, &jerr);
    if (st != SU_OK) {
        fail_msg(err, jerr.message);
        return st;
    }
    root = doc->root;
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        fail_msg(err, "snapshot must be a JSON object");
        return SU_E_TYPE;
    }
    sid = su_json_member(root, "schema_id");
    if (sid == NULL || sid->type != SU_JSON_STRING || !text_ok(sid->u.string.data, sid->u.string.len, SU_WORLD_NAME_MAX)) {
        fail_msg(err, "schema_id is missing or malformed");
        return SU_E_TYPE;
    }
    st = get_u32(su_json_member(root, "schema_version"), &ver);
    if (st == SU_OK) {
        st = get_u32(su_json_member(root, "layout_generation"), &gen);
    }
    if (st != SU_OK) {
        fail_msg(err, "schema_version and layout_generation must be u32 integers");
        return SU_E_TYPE;
    }
    arr = su_json_member(root, "fields");
    if (arr == NULL || arr->type != SU_JSON_ARRAY) {
        fail_msg(err, "fields must be an array");
        return SU_E_TYPE;
    }
    if (arr->u.array.count > SU_LIMIT_FIELDS) {
        fail_msg(err, "field count exceeds the limit");
        return SU_E_RESOURCE_LIMIT;
    }
    n = (uint32_t)arr->u.array.count;
    *fields = calloc(n > 0u ? n : 1u, sizeof **fields);
    if (*fields == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < n; i++) {
        const SuJsonValue *f = arr->u.array.items[i];
        const SuJsonValue *nm;
        uint32_t fid = 0;
        if (f == NULL || f->type != SU_JSON_OBJECT) {
            fail_msg(err, "field entries must be objects");
            return SU_E_TYPE;
        }
        st = get_u32(su_json_member(f, "id"), &fid);
        nm = su_json_member(f, "name");
        if (st != SU_OK || nm == NULL || nm->type != SU_JSON_STRING ||
            !text_ok(nm->u.string.data, nm->u.string.len, SU_WORLD_NAME_MAX)) {
            fail_msg(err, "field id or name is malformed");
            return SU_E_TYPE;
        }
        (*fields)[i].id = fid;
        (*fields)[i].type = SU_TYPE_U32;
        (*fields)[i].access = SU_ACCESS_READ;
        (*fields)[i].name = nm->u.string.data;
    }
    desc->schema_id = sid->u.string.data;
    desc->schema_version = ver;
    desc->layout_generation = gen;
    desc->row_count = 0u;
    desc->entity_ids = NULL;
    desc->fields = *fields;
    desc->field_count = n;
    return SU_OK;
}

uint32_t su_snapshot_restore_world(const uint8_t *bytes, size_t len, const SuWorldDesc *desc,
                                   int allow_schema_change, SuWorld **out_world,
                                   SuSnapshotData *data_out, SuSnapshotError *err) {
    SuJsonDoc hdoc;
    SuWorldDesc hd;
    SuWorldFieldDesc *hf = NULL;
    char *header_names = NULL;
    const SuWorldDesc *use = desc;
    SuSnapshotData local;
    SuSnapshotData *d = data_out != NULL ? data_out : &local;
    uint32_t st;

    if (out_world == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out_world = NULL;
    memset(&hdoc, 0, sizeof hdoc);
    memset(&local, 0, sizeof local);
    if (data_out != NULL) {
        memset(data_out, 0, sizeof *data_out);
    }
    if (allow_schema_change) {
        st = header_desc(bytes, len, &hdoc, &hd, &hf, err);
        if (st != SU_OK) {
            goto done;
        }
        /* Keep only bounded schema names, not a second full JSON tree. Holding hdoc
         * during parse_core doubled row-node memory and exhausted a 4 GiB worker at 1M RTS rows. */
        header_names = malloc(((size_t)hd.field_count + 1u) * (SU_WORLD_NAME_MAX + 1u));
        if (header_names == NULL) {
            st = SU_E_RESOURCE_LIMIT;
            fail_msg(err, "out of memory copying schema names");
            goto done;
        }
        memcpy(header_names, hd.schema_id, strlen(hd.schema_id) + 1u);
        hd.schema_id = header_names;
        for (uint32_t i = 0; i < hd.field_count; i++) {
            char *name = header_names + ((size_t)i + 1u) * (SU_WORLD_NAME_MAX + 1u);
            memcpy(name, hf[i].name, strlen(hf[i].name) + 1u);
            hf[i].name = name;
        }
        su_json_doc_free(&hdoc);
        use = &hd;
    } else if (desc == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = parse_core(bytes, len, use, d, err);
    if (st != SU_OK) {
        goto done;
    }
    st = su_snapshot_restore(use, d, out_world, NULL, NULL);

done:
    if (st != SU_OK) {
        data_free(d);
    }
    if (data_out == NULL) {
        data_free(&local);
    }
    free(header_names);
    free(hf);
    su_json_doc_free(&hdoc);
    return st;
}
