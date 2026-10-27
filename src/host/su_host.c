/* In-process host API (include/sutekh/host.h): worlds driven directly through the schedule, without the
 * supervisor. The pieces are the ones the worker uses for world.create and world.step (su_lir_verify,
 * su_sched_*, su_effects_build, su_world_*, the entity runtime); this file adds memory-based loading, the
 * per-tick input records, the binary snapshot and the digest. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/host.h"
#include "host/su_host_internal.h"
#include "host/su_host_state.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_json.h"
#include "ir/su_lir.h"
#include "host/su_xxh64.h"
#include "ir/su_sha256.h"
#include "runtime/entity_tick.h"
#include "runtime/jobs/su_effects.h"
#include "runtime/rts/host_decl.h"
#include "runtime/schedule.h"
#include "vm/su_vm.h"
#ifdef SU_HAVE_NATIVE
#include "runtime/native_versions.h"
#include "sutekh/native/abi.h"
#endif
#include "sutekh/entities.h"
#include "sutekh/ir.h"
#include "world/entities/su_cmdq.h"
#include "world/entities/su_ents.h"
#include "world/su_world.h"

#define HOST_CAPSULE_JSON_MAX (16u * 1024u * 1024u)
#define HOST_JSON_DEPTH 32u
#define HOST_FIXTURE_MAX (256u * 1024u * 1024u)
#define HOST_DEFAULT_EVENT_LOG 1024u
#define HOST_SNAP_MAGIC "SUHSNAP"   /* 7 bytes + 0x00 */
#define HOST_SNAP_VERSION 1u
#define HOST_SNAP_VERSION_OPSTATE 2u   /* a world with operator state (issue 139): + operator state section */
#define HOST_SNAP_VERSION_TABLES 3u    /* a world with several tables (issue 140): per-table state sections */
#define HOST_OPSTATE_MAGIC "SUOPSTAT"  /* 8 bytes + 0x00 */
#define HOST_OPSTATE_MAGIC_BYTES 9u
#define HOST_OPSTATE_VERSION 1u
#define HOST_OPSTATE_HEADER (HOST_OPSTATE_MAGIC_BYTES + 4u + 4u)
#define HOST_SNAP_HEADER (8u + 4u + 32u + 8u + 8u)
#define HOST_INPUT_MAGIC "SUINPUT"  /* 7 bytes + 0x00 */
#define HOST_INPUT_VERSION 1u
#define HOST_INPUT_HEADER (8u + 4u + 8u + 4u)
#define HOST_INPUT_ENTRY 16u


/* ---------- small helpers ---------- */

static uint32_t set_err(SuHostError *err, uint32_t code, uint32_t index, uint64_t tick, const char *fmt, ...) {
    if (err != NULL) {
        va_list ap;
        err->code = code;
        err->index = index;
        err->tick = tick;
        va_start(ap, fmt);
        (void)vsnprintf(err->message, sizeof err->message, fmt, ap);
        va_end(ap);
    }
    return code;
}

static int table_of_slot(const SuHostWorld *w, uint32_t slot, uint32_t *table, uint32_t *local);

static void clear_err(SuHostError *err) {
    if (err != NULL) {
        memset(err, 0, sizeof *err);
        err->index = UINT32_MAX;
        err->tick = UINT64_MAX;
    }
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

static const SuJsonValue *member(const SuJsonValue *obj, const char *key) {
    return obj != NULL && obj->type == SU_JSON_OBJECT ? su_json_member(obj, key) : NULL;
}

static int get_u32(const SuJsonValue *obj, const char *key, uint32_t *out) {
    uint64_t v = 0;
    if (!su_json_get_uint(member(obj, key), &v) || v > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* ---------- capsules ---------- */

/* Schedule facts of one capsule: version, layout generation, phase and ordering edges. The strings point
 * into doc. */
typedef struct CapsuleFacts {
    SuJsonDoc doc;
    uint32_t schema_version;
    uint32_t layout_generation;
    const char *phase;
    const char *after[SU_SCHED_MAX_SYSTEMS];
    uint32_t after_count;
    const char *before[SU_SCHED_MAX_SYSTEMS];
    uint32_t before_count;
} CapsuleFacts;

/* schedule.after / schedule.before (issue 144): absent is empty; otherwise an array of at most SU_SCHED_MAX_SYSTEMS
 * strings. Anything else is refused (SU_E_TYPE, or SU_E_RESOURCE_LIMIT for too many items) with the reason in why,
 * instead of being dropped. */
static uint32_t edge_items(const SuJsonValue *arr, const char *key, const char **out, uint32_t *n, char *why,
                           size_t why_cap) {
    size_t k;
    *n = 0;
    if (arr == NULL) {
        return SU_OK;
    }
    if (arr->type != SU_JSON_ARRAY) {
        snprintf(why, why_cap, "schedule.%s is not an array", key);
        return SU_E_TYPE;
    }
    if (arr->u.array.count > SU_SCHED_MAX_SYSTEMS) {
        snprintf(why, why_cap, "schedule.%s lists %llu items; at most %u", key, (unsigned long long)arr->u.array.count,
                 (unsigned)SU_SCHED_MAX_SYSTEMS);
        return SU_E_RESOURCE_LIMIT;
    }
    for (k = 0; k < arr->u.array.count; k++) {
        const SuJsonValue *e = arr->u.array.items[k];
        if (e->type != SU_JSON_STRING) {
            snprintf(why, why_cap, "schedule.%s item %u is not a string", key, (unsigned)k);
            return SU_E_TYPE;
        }
        out[(*n)++] = e->u.string.data;
    }
    return SU_OK;
}

static uint32_t capsule_facts(const SuHostCapsule *c, CapsuleFacts *f, char *why, size_t why_cap) {
    SuJsonLimits lim;
    SuJsonError jerr;
    const SuJsonValue *schema;
    const SuJsonValue *sch;
    uint32_t st;

    memset(f, 0, sizeof *f);
    f->phase = "";
    snprintf(why, why_cap, "schema version, layout generation or schedule block");
    lim.max_bytes = c->capsule_len;
    lim.max_depth = HOST_JSON_DEPTH;
    st = su_json_parse(c->capsule_json, c->capsule_len, &lim, &f->doc, &jerr);
    if (st != SU_OK) {
        return st;
    }
    schema = member(f->doc.root, "schema");
    if (!get_u32(schema, "version", &f->schema_version) ||
        !get_u32(schema, "layout_generation", &f->layout_generation)) {
        return SU_E_TYPE;
    }
    sch = member(f->doc.root, "schedule");
    if (sch == NULL) {
        return SU_OK;
    }
    if (sch->type != SU_JSON_OBJECT) {
        return SU_E_TYPE;
    }
    if (member(sch, "phase") != NULL && member(sch, "phase")->type == SU_JSON_STRING) {
        f->phase = member(sch, "phase")->u.string.data;
    }
    st = edge_items(member(sch, "after"), "after", f->after, &f->after_count, why, why_cap);
    if (st == SU_OK) {
        st = edge_items(member(sch, "before"), "before", f->before, &f->before_count, why, why_cap);
    }
    return st;
}

/* Effect analysis over the whole schedule, as world.create runs it (worker.c check_system_effects_ovr):
 * every system declared parallel, `after` edges must name earlier systems of the capsule order. */
static uint32_t check_effects(const SuSchedule *s, const CapsuleFacts *facts, SuHostError *err) {
    SuEffectInput in[SU_SCHED_MAX_SYSTEMS];
    SuEffectGraph *g = NULL;
    char why[256];
    uint32_t n = s->count;
    uint32_t i;
    uint32_t st;

    if (n < 2u) {
        return SU_OK;
    }
    memset(in, 0, sizeof in);
    for (i = 0; i < n; i++) {
        in[i].system_id = s->sys[i].system_id;
        in[i].mod = s->sys[i].mod;
        in[i].phase = facts[i].phase;
        in[i].declared_parallel = 1u;
        in[i].host_declared = s->sys[i].kind == SU_SYS_KIND_HOST ? 1u : 0u; /* its record's access is the effect */
        in[i].table = s->sys[i].table; /* systems in different tables never share columns (issue 140) */
        in[i].after = facts[i].after;
        in[i].after_count = facts[i].after_count;
        in[i].before = facts[i].before;
        in[i].before_count = facts[i].before_count;
    }
    why[0] = '\0';
    st = su_effects_build(in, n, &g, why, sizeof why);
    su_effects_free(g);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "effect analysis: %s", why);
    }
    for (i = 0; i < n; i++) {
        uint32_t k;
        for (k = 0; k < facts[i].after_count; k++) {
            int j = su_sched_find(s, facts[i].after[k]);
            if (j >= 0 && (uint32_t)j >= i) {
                return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                               "schedule edge %s after %s runs against the capsule order", s->sys[i].system_id,
                               facts[i].after[k]);
            }
        }
    }
    return SU_OK;
}

/* ---------- world creation ---------- */

static uint32_t read_u32_array(const SuJsonValue *arr, uint64_t expect, uint32_t **out) {
    uint32_t *v;
    uint64_t i;
    *out = NULL;
    if (arr == NULL || arr->type != SU_JSON_ARRAY || arr->u.array.count != expect) {
        return SU_E_TYPE;
    }
    v = malloc((size_t)(expect > 0u ? expect : 1u) * sizeof *v);
    if (v == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < expect; i++) {
        uint64_t x = 0;
        if (!su_json_get_uint(arr->u.array.items[i], &x) || x > UINT32_MAX) {
            free(v);
            return SU_E_TYPE;
        }
        v[i] = (uint32_t)x;
    }
    *out = v;
    return SU_OK;
}

/* Field descriptors of one table's union table. The names are borrowed from the schedule. */
static SuWorldFieldDesc *table_fields(const SuSchedule *s, uint32_t table) {
    uint32_t n = 0;
    const SuSchedField *f = su_sched_table_fields(s, table, &n);
    SuWorldFieldDesc *d = malloc((size_t)(n > 0u ? n : 1u) * sizeof *d);
    uint32_t i;
    if (d == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        d[i].id = f[i].id;
        d[i].type = f[i].type;
        /* A declared lane no system binds (access 0) is a read-only column of the world: no staging, written only
         * by the host between ticks. World access is not in the schema digest or the canonical state. */
        d[i].access = f[i].access != 0u ? f[i].access : SU_ACCESS_READ;
        d[i].name = f[i].name;
    }
    return d;
}

/* Opens one table's world: from its fixture (world fixture format, as world.create) or empty with capacity. */
static uint32_t open_table(SuHostWorld *hw, uint32_t table, const uint8_t *fixture, size_t fixture_len,
                           uint64_t max_entities, SuHostError *err) {
    const SuSchedule *s = &hw->sched;
    SuWorldFieldDesc *fd = table_fields(s, table);
    SuWorldDesc wd;
    SuJsonDoc doc;
    SuJsonLimits lim;
    SuJsonError jerr;
    const SuJsonValue *root;
    const SuJsonValue *ids_arr;
    const SuJsonValue *cols;
    const SuJsonValue *sid;
    const SuSchedField *fields = NULL;
    uint32_t field_count = 0;
    uint32_t *ids = NULL;
    uint32_t *vals = NULL;
    uint64_t tick = 1;
    uint64_t rows;
    uint32_t i;
    uint32_t st;

    fields = su_sched_table_fields(s, table, &field_count);
    memset(&doc, 0, sizeof doc);
    if (fd == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    memset(&wd, 0, sizeof wd);
    wd.schema_id = s->tables[table].schema_id;
    wd.schema_version = s->tables[table].schema_version;
    wd.layout_generation = s->tables[table].layout_generation;
    wd.fields = fd;
    wd.field_count = field_count;
    if (fixture == NULL) {
        if (max_entities == 0u) {
            free(fd);
            return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                           "table %u: an empty table needs max_entities >= 1", (unsigned)table);
        }
        st = su_world_create_ex(&wd, max_entities, &hw->tabs[table].w);
        free(fd);
        if (table == 0u) {
            hw->w = hw->tabs[0].w;
        }
        return st == SU_OK ? SU_OK : set_err(err, st, UINT32_MAX, UINT64_MAX, "table %u: world creation failed",
                                             (unsigned)table);
    }
    if (fixture_len > HOST_FIXTURE_MAX) {
        free(fd);
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "table %u: fixture exceeds 256 MiB",
                       (unsigned)table);
    }
    lim.max_bytes = fixture_len;
    lim.max_depth = HOST_JSON_DEPTH;
    st = su_json_parse(fixture, fixture_len, &lim, &doc, &jerr);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "table %u fixture: %s", (unsigned)table, jerr.message);
        goto out;
    }
    root = doc.root;
    sid = member(root, "schema_id");
    if (root == NULL || root->type != SU_JSON_OBJECT || !su_json_get_uint(member(root, "tick"), &tick) ||
        tick != 0u) {
        st = set_err(err, SU_E_TYPE, UINT32_MAX, UINT64_MAX, "table %u: fixture must be an object with tick 0",
                     (unsigned)table);
        goto out;
    }
    if (sid == NULL || sid->type != SU_JSON_STRING || strcmp(sid->u.string.data, s->tables[table].schema_id) != 0) {
        st = set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                     "table %u: fixture schema_id does not match the table's schema", (unsigned)table);
        goto out;
    }
    ids_arr = member(root, "entity_ids");
    if (ids_arr == NULL || ids_arr->type != SU_JSON_ARRAY) {
        st = set_err(err, SU_E_TYPE, UINT32_MAX, UINT64_MAX, "table %u: fixture entity_ids must be an array",
                     (unsigned)table);
        goto out;
    }
    rows = ids_arr->u.array.count;
    if (rows > SU_LIMIT_ROWS) {
        st = set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX,
                     "table %u: fixture row count exceeds the row limit", (unsigned)table);
        goto out;
    }
    st = read_u32_array(ids_arr, rows, &ids);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "table %u: fixture entity_ids must be u32 integers", (unsigned)table);
        goto out;
    }
    cols = member(root, "columns");
    if (cols == NULL || cols->type != SU_JSON_OBJECT) {
        st = set_err(err, SU_E_TYPE, UINT32_MAX, UINT64_MAX, "table %u: fixture columns must be an object",
                     (unsigned)table);
        goto out;
    }
    wd.row_count = rows;
    wd.entity_ids = ids;
    st = su_world_create_ex(&wd, max_entities, &hw->tabs[table].w);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "table %u: world creation failed", (unsigned)table);
        goto out;
    }
    if (table == 0u) {
        hw->w = hw->tabs[0].w;
    }
    for (i = 0; i < field_count; i++) {
        st = read_u32_array(member(cols, fields[i].name), rows, &vals);
        if (st == SU_OK) {
            st = su_world_init_committed(hw->tabs[table].w, fields[i].id, vals, rows);
        }
        free(vals);
        vals = NULL;
        if (st != SU_OK) {
            set_err(err, st, fields[i].id, UINT64_MAX, "table %u: fixture column %s is missing, mis-sized or not u32",
                    (unsigned)table, fields[i].name);
            goto out;
        }
    }
    st = SU_OK;
out:
    if (st != SU_OK) {
        su_world_destroy(hw->tabs[table].w);
        hw->tabs[table].w = NULL;
        if (table == 0u) {
            hw->w = NULL;
        }
    }
    free(ids);
    free(fd);
    su_json_doc_free(&doc);
    return st;
}

static void compute_identity(SuHostWorld *hw) {
    SuSha256 c;
    uint8_t schema[32];
    uint8_t b[8];
    uint32_t i;
    memset(schema, 0, sizeof schema);
    (void)su_world_schema_digest(hw->w, schema);
    su_sha256_init(&c);
    su_sha256_update(&c, (const uint8_t *)"SUHIDENT", 9u); /* includes the terminating 0x00 */
    put_u32(b, 1u);
    su_sha256_update(&c, b, 4u);
    su_sha256_update(&c, schema, 32u);
    put_u32(b, hw->sched.count);
    su_sha256_update(&c, b, 4u);
    for (i = 0; i < hw->sched.count; i++) {
        const SuSysEntry *e = &hw->sched.sys[i];
        put_u32(b, (uint32_t)strlen(e->system_id));
        su_sha256_update(&c, b, 4u);
        su_sha256_update(&c, (const uint8_t *)e->system_id, strlen(e->system_id));
        su_sha256_update(&c, e->mod->semantic_hash, 32u);
    }
    put_u64(b, su_world_row_count(hw->w));
    su_sha256_update(&c, b, 8u);
    put_u64(b, hw->max_struct);
    su_sha256_update(&c, b, 8u);
    put_u64(b, hw->max_events);
    su_sha256_update(&c, b, 8u);
    put_u64(b, hw->log_max);
    su_sha256_update(&c, b, 8u);
    su_sha256_final(&c, hw->identity);
    if (hw->sched.declared) {
        /* Declared schema (issue 144): fold it in, so worlds with and without one never share an identity. */
        const SuSchedule *s = &hw->sched;
        uint8_t prev[32];
        memcpy(prev, hw->identity, 32u);
        su_sha256_init(&c);
        su_sha256_update(&c, (const uint8_t *)"SUHSCHEMA", 10u); /* includes the terminating 0x00 */
        su_sha256_update(&c, prev, 32u);
        put_u32(b, (uint32_t)strlen(s->schema_id));
        su_sha256_update(&c, b, 4u);
        su_sha256_update(&c, (const uint8_t *)s->schema_id, strlen(s->schema_id));
        put_u32(b, s->schema_version);
        su_sha256_update(&c, b, 4u);
        put_u32(b, s->layout_generation);
        su_sha256_update(&c, b, 4u);
        put_u32(b, s->declared_count);
        su_sha256_update(&c, b, 4u);
        for (i = 0; i < s->declared_count; i++) {
            const SuSchedField *f = &s->declared_fields[i];
            put_u32(b, f->id);
            su_sha256_update(&c, b, 4u);
            put_u32(b, f->value_type);
            su_sha256_update(&c, b, 4u);
            put_u32(b, (uint32_t)strlen(f->name));
            su_sha256_update(&c, b, 4u);
            su_sha256_update(&c, (const uint8_t *)f->name, strlen(f->name));
        }
        su_sha256_final(&c, hw->identity);
    }
    if (hw->table_count > 1u) {
        /* Multi-table world (issue 140): fold the table descriptors in, so a snapshot or replay loads only into a
         * world with the same tables, capacities, schemas and order. Single-table worlds never take this path. */
        const SuSchedule *s = &hw->sched;
        uint8_t prev[32];
        uint32_t t;
        memcpy(prev, hw->identity, 32u);
        su_sha256_init(&c);
        su_sha256_update(&c, (const uint8_t *)"SUHTABLES", 10u); /* includes the terminating 0x00 */
        su_sha256_update(&c, prev, 32u);
        put_u32(b, hw->table_count);
        su_sha256_update(&c, b, 4u);
        for (t = 0; t < hw->table_count; t++) {
            const SuSchedTable *tab = &s->tables[t];
            uint8_t sd[32];
            memset(sd, 0, sizeof sd);
            (void)su_world_schema_digest(hw->tabs[t].w, sd);
            su_sha256_update(&c, sd, 32u);
            put_u32(b, (uint32_t)strlen(tab->schema_id));
            su_sha256_update(&c, b, 4u);
            su_sha256_update(&c, (const uint8_t *)tab->schema_id, strlen(tab->schema_id));
            put_u32(b, tab->schema_version);
            su_sha256_update(&c, b, 4u);
            put_u32(b, tab->layout_generation);
            su_sha256_update(&c, b, 4u);
            put_u64(b, hw->tabs[t].rows);
            su_sha256_update(&c, b, 8u);
            put_u32(b, tab->field_count);
            su_sha256_update(&c, b, 4u);
            for (i = 0; i < tab->field_count; i++) {
                const SuSchedField *f = &tab->fields[i];
                put_u32(b, f->id);
                su_sha256_update(&c, b, 4u);
                put_u32(b, f->value_type);
                su_sha256_update(&c, b, 4u);
                put_u32(b, (uint32_t)strlen(f->name));
                su_sha256_update(&c, b, 4u);
                su_sha256_update(&c, (const uint8_t *)f->name, strlen(f->name));
            }
        }
        su_sha256_final(&c, hw->identity);
    }
    /* Cross-table host operators (issue 140): a capsule's target table and the lanes it binds there change what the
     * world computes, so they join the identity. Folded only when some entry has a target set, so a world without
     * one (every world of the earlier slices) keeps its identity byte for byte. */
    {
        uint32_t n = 0;
        uint32_t t;
        for (i = 0; i < hw->sched.count; i++) {
            if (hw->sched.sys[i].target_table != UINT32_MAX) {
                n++;
            }
        }
        if (n > 0u) {
            uint8_t prev[32];
            memcpy(prev, hw->identity, 32u);
            su_sha256_init(&c);
            su_sha256_update(&c, (const uint8_t *)"SUHTARGET", 10u); /* includes the terminating 0x00 */
            su_sha256_update(&c, prev, 32u);
            put_u32(b, n);
            su_sha256_update(&c, b, 4u);
            for (i = 0; i < hw->sched.count; i++) {
                const SuSysEntry *e = &hw->sched.sys[i];
                if (e->target_table == UINT32_MAX) {
                    continue;
                }
                put_u32(b, i);
                su_sha256_update(&c, b, 4u);
                put_u32(b, e->target_table);
                su_sha256_update(&c, b, 4u);
                put_u32(b, e->target_bind_count);
                su_sha256_update(&c, b, 4u);
                for (t = 0; t < e->target_bind_count; t++) {
                    put_u32(b, e->target_bind[t]);
                    su_sha256_update(&c, b, 4u);
                }
            }
            su_sha256_final(&c, hw->identity);
        }
    }
}

/* Declared world schema (issue 144): expands SuHostSchema fields into storage lanes (i64/q32: "x.lo" at J, "x.hi" at
 * J + 1) and declares them on the schedule. */
/* Versioned descriptor copy (the SuHostOperatorDesc convention): struct_size >= v1, only struct_size bytes read,
 * bytes beyond the library's struct must be zero. Returns the offending byte offset + 1, or 0 when the tail is zero. */
static size_t nonzero_tail(const void *p, size_t lib_size, size_t struct_size) {
    size_t k;
    for (k = lib_size; k < struct_size; k++) {
        if (((const unsigned char *)p)[k] != 0u) {
            return k + 1u;
        }
    }
    return 0u;
}

/* The capsule rule for field names (su_lir.c): [A-Za-z_][A-Za-z0-9_]*. */
static int field_name_ok(const char *s) {
    size_t i;
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        int digit = c >= '0' && c <= '9';
        if (i == 0 ? !alpha : !(alpha || digit)) {
            return 0;
        }
    }
    return 1;
}

static int lane_like(const char *s) {
    size_t n = s != NULL ? strlen(s) : 0u;
    return n > 3u && (strcmp(s + n - 3u, ".lo") == 0 || strcmp(s + n - 3u, ".hi") == 0);
}

static uint32_t declare_schema(SuHostWorld *hw, uint32_t table, const SuHostSchema *caller, SuHostError *err) {
    SuHostSchema local;
    const SuHostSchema *sc = &local;
    SuSchedField *lanes;
    uint32_t n = 0, i, j;
    uint32_t st;
    size_t bad;
    if (caller->struct_size < SU_HOST_SCHEMA_V1_SIZE) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "schema: struct_size %u is below %u",
                       (unsigned)caller->struct_size, (unsigned)SU_HOST_SCHEMA_V1_SIZE);
    }
    bad = nonzero_tail(caller, sizeof local, caller->struct_size);
    if (bad != 0u) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "schema: byte %u is a field this library does not know and is not zero", (unsigned)(bad - 1u));
    }
    memset(&local, 0, sizeof local);
    memcpy(&local, caller, caller->struct_size < sizeof local ? caller->struct_size : sizeof local);
    if (sc->fields == NULL || sc->schema_id == NULL || sc->field_count == 0u ||
        sc->field_count > SU_HOST_SCHEMA_MAX_LANES) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "schema: fields, schema_id or field_count (1..%u) is invalid",
                       (unsigned)SU_HOST_SCHEMA_MAX_LANES);
    }
    if (strlen(sc->schema_id) == 0u || strlen(sc->schema_id) > SU_WORLD_NAME_MAX) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "schema: schema_id must be 1..%u bytes",
                       (unsigned)SU_WORLD_NAME_MAX);
    }
    if (sc->schema_version == 0u || sc->layout_generation == 0u) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "schema: schema_version and layout_generation must be >= 1 (got %u and %u)",
                       (unsigned)sc->schema_version, (unsigned)sc->layout_generation);
    }
    lanes = calloc((size_t)sc->field_count * 2u, sizeof *lanes);
    if (lanes == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    for (i = 0; i < sc->field_count; i++) {
        const SuHostSchemaField *f = &sc->fields[i];
        uint32_t t = f->type;
        int wide = t == SU_TYPE_I64 || t == SU_TYPE_Q32;
        size_t len = f->name != NULL ? strlen(f->name) : 0u;
        if (t != SU_TYPE_U32 && t != SU_TYPE_I32 && !wide) {
            free(lanes);
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                           "schema field %u: type %u is not u32, i32, i64 or q32", (unsigned)i, (unsigned)t);
        }
        if (len == 0u || len > SU_WORLD_NAME_MAX - (wide ? 3u : 0u)) {
            free(lanes);
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX, "schema field %u: the name must be 1..%u bytes",
                           (unsigned)i, (unsigned)(SU_WORLD_NAME_MAX - (wide ? 3u : 0u)));
        }
        if (!field_name_ok(f->name)) {
            free(lanes);
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                           lane_like(f->name)
                               ? "schema field %u: \"%s\" is a lane name; declare the i64/q32 field itself"
                               : "schema field %u: name \"%s\" must match [A-Za-z_][A-Za-z0-9_]*",
                           (unsigned)i, f->name);
        }
        if (f->id == 0u) {
            free(lanes);
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX, "schema field %u (%s): field IDs are >= 1",
                           (unsigned)i, f->name);
        }
        if (wide && f->id == UINT32_MAX) {
            free(lanes);
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                           "schema field %u (%s): a 64-bit field needs id + 1 for its high lane", (unsigned)i, f->name);
        }
        for (j = 0; j < (wide ? 2u : 1u); j++) {
            SuSchedField *l = &lanes[n++];
            l->id = f->id + j;
            l->type = SU_TYPE_U32;
            l->value_type = t;
            l->access = 0u;
            snprintf(l->name, sizeof l->name, "%s%s", f->name, !wide ? "" : j == 0u ? ".lo" : ".hi");
        }
    }
    if (n > SU_HOST_SCHEMA_MAX_LANES) {
        free(lanes);
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "schema: %u lanes; the limit is %u",
                       (unsigned)n, (unsigned)SU_HOST_SCHEMA_MAX_LANES);
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < i; j++) {
            if (lanes[j].id == lanes[i].id || strcmp(lanes[j].name, lanes[i].name) == 0) {
                uint32_t dup_id = lanes[j].id == lanes[i].id;
                char a[SU_WORLD_NAME_MAX + 1u], b[SU_WORLD_NAME_MAX + 1u];
                uint32_t fi = 0, k, at = 0;
                snprintf(a, sizeof a, "%s", lanes[j].name);
                snprintf(b, sizeof b, "%s", lanes[i].name);
                /* Field index of lane i, for err->index. */
                for (k = 0; k < sc->field_count; k++) {
                    uint32_t w = (sc->fields[k].type == SU_TYPE_I64 || sc->fields[k].type == SU_TYPE_Q32) ? 2u : 1u;
                    if (i < at + w) {
                        fi = k;
                        break;
                    }
                    at += w;
                }
                free(lanes);
                return set_err(err, SU_E_INVALID_ARGUMENT, fi, UINT64_MAX,
                               dup_id ? "schema field %u: lane \"%s\" has the id of lane \"%s\""
                                      : "schema field %u: lane name \"%s\" is also the name of lane \"%s\"",
                               (unsigned)fi, b, a);
            }
        }
    }
    st = su_sched_declare_table(&hw->sched, table, sc->schema_id, sc->schema_version, sc->layout_generation, lanes, n);
    free(lanes);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "schema could not be declared");
    }
    return SU_OK;
}

/* A capsule's schema facts against the declared schema: a specific message instead of the generic join failure. */
static uint32_t check_declared_facts(const SuHostWorld *hw, uint32_t table, const char *query, uint32_t sv,
                                     uint32_t gen, uint32_t i, SuHostError *err) {
    const SuSchedule *s = &hw->sched;
    const SuSchedTable *t = &s->tables[table];
    if (!t->declared || (strcmp(query, t->schema_id) == 0 && sv == t->schema_version &&
                         gen == t->layout_generation)) {
        return SU_OK;
    }
    if (table == 0u) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, i, UINT64_MAX,
                       "capsule %u: schema %s version %u generation %u is not the declared world schema %s version %u "
                       "generation %u",
                       (unsigned)i, query, (unsigned)sv, (unsigned)gen, t->schema_id, (unsigned)t->schema_version,
                       (unsigned)t->layout_generation);
    }
    return set_err(err, SU_E_SCHEMA_MISMATCH, i, UINT64_MAX,
                   "capsule %u: schema %s version %u generation %u is not the declared schema %s version %u "
                   "generation %u of table %u",
                   (unsigned)i, query, (unsigned)sv, (unsigned)gen, t->schema_id, (unsigned)t->schema_version,
                   (unsigned)t->layout_generation, (unsigned)table);
}

/* Capsule index i is a host operator record (lir_json NULL): find the operator its system_id names (built in, or
 * registered with su_host_register_operator), check the record against it (su_host_load_bound_bytes_named: exact
 * or schema-relative binding; a registered operator's record names its operator_name) and append the host
 * entry. */
static uint32_t add_host_record(SuHostWorld *hw, uint32_t table, const SuHostCapsule *c, const CapsuleFacts *f,
                                uint32_t i, SuHostError *err) {
    const SuJsonValue *sid = member(f->doc.root, "system_id");
    const HostOperator *op = sid != NULL && sid->type == SU_JSON_STRING ? su_host_operator_find(sid->u.string.data)
                                                                         : NULL;
    SuLirModule *decl = NULL;
    uint32_t bind[SU_LIMIT_FIELDS];
    uint32_t sv = 0;
    uint32_t gen = 0;
    int bound = 0;
    char msg[256];
    uint32_t st;

    if (op == NULL) {
        return set_err(err, SU_E_CAPABILITY_DENIED, i, UINT64_MAX,
                       "capsule %u: no host operator for system %s is built into or registered with this library",
                       (unsigned)i,
                       sid != NULL && sid->type == SU_JSON_STRING ? sid->u.string.data : "(none)");
    }
    msg[0] = '\0';
    st = su_host_load_bound_bytes_named(c->capsule_json, c->capsule_len, NULL, op->desc, op->record_name, &decl, &sv,
                                        &gen, bind, &bound, msg, sizeof msg);
    if (st != SU_OK) {
        return set_err(err, st, i, UINT64_MAX, "capsule %u: host record refused: %s", (unsigned)i, msg);
    }
    st = check_declared_facts(hw, table, decl->query, sv, gen, i, err);
    if (st != SU_OK) {
        su_host_decl_free(decl);
        return st;
    }
    st = su_sched_add_host_bound_table(&hw->sched, table, op->desc, decl, bound ? bind : NULL, sv, gen, 0u, 1u, 0u);
    if (st != SU_OK) {
        su_host_decl_free(decl);
        return set_err(err, st, i, UINT64_MAX,
                       "capsule %u: host system cannot join the schedule (schema or duplicate system id)", (unsigned)i);
    }
    return SU_OK;
}

/* "u32", "i32", "i64" or "q32" (a lane's value type, for messages). */
static const char *vtype_name(uint32_t t) {
    return t == SU_TYPE_Q32 ? "q32" : t == SU_TYPE_I64 ? "i64" : t == SU_TYPE_I32 ? "i32" : "u32";
}

/* 1 when field id is readable in the table's world: a target set reads the other table's working columns through
 * the world's read invocation, which carries the READ fields only. A lane some system writes and no one reads is
 * write-only, so it cannot be a target lane. */
static int world_field_readable(const SuWorld *w, uint32_t id) {
    uint32_t n = su_world_field_count(w);
    uint32_t i;
    for (i = 0; i < n; i++) {
        SuWorldFieldDesc fd;
        uint32_t def = 0;
        if (su_world_field_at(w, i, &fd, &def) == SU_OK && fd.id == id) {
            return (fd.access & SU_ACCESS_READ) != 0u;
        }
    }
    return 0;
}

/* Cross-table host operator (issue 140): resolves the target lanes of host entry i against table tgt's DECLARED
 * schema and binds them. A target lane is matched by name, and its value type must equal the descriptor's: the
 * operator keeps its own IDs (run->target_columns reports them), the world knows which columns they are. */
static uint32_t bind_target(SuHostWorld *hw, uint32_t i, uint32_t tgt, SuHostError *err) {
    const SuSysEntry *e = &hw->sched.sys[i];
    const SuHostOpDesc *d = e->host;
    const SuSchedTable *tab = &hw->sched.tables[tgt];
    uint32_t bind[SU_LIMIT_FIELDS];
    uint32_t k, j;

    if (!tab->declared) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, i, UINT64_MAX,
                       "capsule %u (%s): target table %u declares no schema, so its target lanes cannot be resolved",
                       (unsigned)i, e->system_id, (unsigned)tgt);
    }
    for (k = 0; k < d->target_field_count; k++) {
        const SuHostField *f = &d->target_fields[k];
        uint32_t want = f->type == 0u ? SU_TYPE_U32 : f->type;
        int found = 0;
        for (j = 0; j < tab->declared_count; j++) {
            const SuSchedField *l = &tab->declared_fields[j];
            uint32_t have = l->value_type == 0u ? SU_TYPE_U32 : l->value_type;
            if (strcmp(l->name, f->name) != 0) {
                continue;
            }
            if (have != want) {
                return set_err(err, SU_E_SCHEMA_MISMATCH, i, UINT64_MAX,
                               "capsule %u (%s): target lane %s of table %u is %s, the operator reads it as %s",
                               (unsigned)i, e->system_id, f->name, (unsigned)tgt, vtype_name(have), vtype_name(want));
            }
            if (!world_field_readable(hw->tabs[tgt].w, l->id)) {
                return set_err(err, SU_E_SCHEMA_MISMATCH, i, UINT64_MAX,
                               "capsule %u (%s): target lane %s of table %u is write-only, so it cannot be read "
                               "as a target",
                               (unsigned)i, e->system_id, f->name, (unsigned)tgt);
            }
            bind[k] = l->id;
            found = 1;
            break;
        }
        if (!found) {
            return set_err(err, SU_E_SCHEMA_MISMATCH, i, UINT64_MAX,
                           "capsule %u (%s): target lane %s is not a lane of table %u's declared schema",
                           (unsigned)i, e->system_id, f->name, (unsigned)tgt);
        }
    }
    return su_sched_set_host_target(&hw->sched, i, tgt, hw->tabs[e->table].base, hw->tabs[tgt].base,
                                    hw->tabs[tgt].rows, bind);
}

_Static_assert(SU_HOST_MAX_CAPSULES <= SU_SCHED_MAX_SYSTEMS, "host worlds fit the schedule");
_Static_assert(SU_HOST_MAX_CAPSULES <= 64u, "opdata_mask has one bit per schedule entry");
_Static_assert(SU_HOST_SCHEMA_MAX_LANES == SU_LIMIT_FIELDS, "declared schemas use the field limit");
_Static_assert(SU_HOST_WORLD_DESC_V1_SIZE == offsetof(SuHostWorldDesc, schema), "v1 descriptor ends at seed");
_Static_assert(sizeof(void *) != 8u || SU_HOST_WORLD_DESC_V1_SIZE == 80u, "first-release SuHostWorldDesc is 80 bytes");
_Static_assert(sizeof(void *) != 8u || SU_HOST_WORLD_DESC_V2_SIZE == 88u, "v2 SuHostWorldDesc (schema) is 88 bytes");
_Static_assert(sizeof(void *) != 8u || SU_HOST_WORLD_DESC_V3_SIZE == 96u, "v3 SuHostWorldDesc is 96 bytes");
_Static_assert(sizeof(void *) != 8u || SU_HOST_WORLD_DESC_V4_SIZE == 120u, "v4 SuHostWorldDesc (tables) is 120 bytes");
_Static_assert(sizeof(void *) != 8u || sizeof(SuHostWorldDesc) == 128u, "v5 SuHostWorldDesc (targets) is 128 bytes");
_Static_assert(sizeof(void *) != 8u || sizeof(SuHostTableDesc) == 40u, "SuHostTableDesc is 40 bytes");
_Static_assert(SU_HOST_SCHEMA_V1_SIZE == (sizeof(void *) == 8u ? 32u : 24u), "first-release SuHostSchema size");
_Static_assert(sizeof(SuHostSchema) == SU_HOST_SCHEMA_V1_SIZE, "SuHostSchema has no padding after the v1 fields");

static uint32_t input_storage_init(SuHostWorld *w); /* inputs, below */

/* Operator-state budget (host.h, SuHostWorldDesc.state_budget_bytes; issue 153): the exact reservations of the
 * stateful operators, summed in schedule order, before anything of the world's state, columns or operator scratch is
 * allocated. Runs before su_sched_finalize, while schedule entry i is still capsule i. Allocates nothing. */
static uint32_t admit_state_budget(const SuHostWorld *hw, uint64_t budget, SuHostError *err) {
    uint64_t total = 0;
    uint32_t i;
    if (budget == 0u) {
        budget = SU_HOST_STATE_BUDGET_DEFAULT;
    }
    for (i = 0; i < hw->sched.count; i++) {
        uint64_t r;
        if (hw->sched.sys[i].kind != SU_SYS_KIND_HOST) {
            continue;
        }
        r = su_host_operator_state_reservation(su_host_operator_find(hw->sched.sys[i].system_id));
        if (r == 0u) {
            continue;
        }
        /* At most SU_HOST_MAX_CAPSULES reservations of under 2^33 bytes each: the sum cannot wrap. */
        total += r;
        if (total > budget) {
            return set_err(err, SU_E_RESOURCE_LIMIT, i, UINT64_MAX,
                           "capsule %u: the operator state of %s (%llu bytes) brings the world to %llu bytes, over "
                           "its operator-state budget of %llu bytes",
                           (unsigned)i, hw->sched.sys[i].system_id, (unsigned long long)r,
                           (unsigned long long)total, (unsigned long long)budget);
        }
    }
    return SU_OK;
}

uint32_t su_host_world_create(const SuHostWorldDesc *desc_in, SuHostWorld **out, SuHostError *err) {
    SuHostWorld *hw = NULL;
    SuHostWorldDesc dv;
    const SuHostWorldDesc *desc = &dv;
    CapsuleFacts *facts = NULL;
    char fwhy[160];
    uint32_t nfacts = 0;
    uint32_t i;
    uint32_t st;
    uint64_t world_slots = 0; /* every table's slots together: the world handle space (issue 140) */

    clear_err(err);
    if (out == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "out is NULL");
    }
    *out = NULL;
    /* Versioning (host.h): the v1 or v2 size, or the library's size or larger with a zero tail; only struct_size bytes
     * are read, and the fields an older size does not cover are zero (no schema, the default state budget). A size
     * ending inside a field is refused. */
    if (desc_in == NULL || desc_in->struct_size < SU_HOST_WORLD_DESC_V1_SIZE ||
        (desc_in->struct_size != SU_HOST_WORLD_DESC_V1_SIZE && desc_in->struct_size != SU_HOST_WORLD_DESC_V2_SIZE &&
         desc_in->struct_size != SU_HOST_WORLD_DESC_V3_SIZE && desc_in->struct_size != SU_HOST_WORLD_DESC_V4_SIZE &&
         desc_in->struct_size < sizeof *desc_in)) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "descriptor: struct_size, capsules (1..%u), reserved0 or fixture is invalid",
                       (unsigned)SU_HOST_MAX_CAPSULES);
    }
    {
        size_t bad = nonzero_tail(desc_in, sizeof dv, desc_in->struct_size);
        if (bad != 0u) {
            return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                           "descriptor: byte %u is a field this library does not know and is not zero",
                           (unsigned)(bad - 1u));
        }
    }
    memset(&dv, 0, sizeof dv);
    memcpy(&dv, desc_in, desc_in->struct_size < sizeof dv ? desc_in->struct_size : sizeof dv);
    if (desc->capsules != NULL && desc->capsule_count > SU_HOST_MAX_CAPSULES) {
        return set_err(err, SU_E_RESOURCE_LIMIT, SU_HOST_MAX_CAPSULES, UINT64_MAX,
                       "%u capsules: a world schedules at most %u systems (capsule %u does not fit)",
                       (unsigned)desc->capsule_count, (unsigned)SU_HOST_MAX_CAPSULES, (unsigned)SU_HOST_MAX_CAPSULES);
    }
    if (desc->capsules == NULL || desc->capsule_count == 0u || desc->reserved0 != 0u ||
        (desc->fixture_json == NULL && desc->fixture_len != 0u)) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "descriptor: struct_size, capsules (1..%u), reserved0 or fixture is invalid",
                       (unsigned)SU_HOST_MAX_CAPSULES);
    }
    if (desc->table_count > SU_HOST_MAX_TABLES) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX,
                       "%u tables: a world has at most %u (issue 140)", (unsigned)desc->table_count,
                       (unsigned)SU_HOST_MAX_TABLES);
    }
    if (desc->table_count > 1u) {
        uint32_t t;
        if (desc->table_reserved != 0u || desc->tables == NULL || desc->capsule_table == NULL ||
            desc->schema != NULL || desc->fixture_json != NULL || desc->fixture_len != 0u) {
            return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                           "descriptor: a world with tables needs tables and capsule_table, and no schema, fixture "
                           "or reserved0 byte");
        }
        for (t = 0; t < desc->table_count; t++) {
            const SuHostTableDesc *td = &desc->tables[t];
            if (td->struct_size < SU_HOST_TABLE_DESC_V1_SIZE || td->reserved != 0u || td->schema == NULL ||
                (td->fixture_json == NULL && td->fixture_len != 0u)) {
                return set_err(err, SU_E_INVALID_ARGUMENT, t, UINT64_MAX,
                               "table %u: struct_size, reserved, schema or fixture is invalid", (unsigned)t);
            }
        }
        for (i = 0; i < desc->capsule_count; i++) {
            if (desc->capsule_table[i] >= desc->table_count) {
                return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                               "capsule %u: table %u does not exist (%u tables)", (unsigned)i,
                               (unsigned)desc->capsule_table[i], (unsigned)desc->table_count);
            }
            if (desc->capsule_target_table != NULL && desc->capsule_target_table[i] != SU_HOST_TABLE_NONE) {
                if (desc->capsule_target_table[i] >= desc->table_count) {
                    return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                                   "capsule %u: target table %u does not exist (%u tables)", (unsigned)i,
                                   (unsigned)desc->capsule_target_table[i], (unsigned)desc->table_count);
                }
                if (desc->capsule_target_table[i] == desc->capsule_table[i]) {
                    return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                                   "capsule %u: the target table must differ from the capsule's own table",
                                   (unsigned)i);
                }
            }
        }
    } else if (desc->tables != NULL || desc->capsule_table != NULL || desc->table_reserved != 0u) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "descriptor: a single-table world takes no tables, capsule_table or table_reserved byte");
    }
    if (desc->capsule_target_table != NULL) {
        for (i = 0; i < desc->capsule_count; i++) {
            if (desc->capsule_target_table[i] != SU_HOST_TABLE_NONE && desc->table_count <= 1u) {
                return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                               "capsule %u: a target set needs a world with several tables", (unsigned)i);
            }
        }
    }
    if (desc->tick_rate_hz == 0u || desc->tick_rate_hz > SU_HOST_TICK_RATE_MAX) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "tick_rate_hz must be 1..%u",
                       (unsigned)SU_HOST_TICK_RATE_MAX);
    }
    hw = calloc(1, sizeof *hw);
    if (hw == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    su_sched_init(&hw->sched);
    st = su_sched_set_max_systems(&hw->sched, SU_HOST_MAX_CAPSULES);
    facts = calloc(desc->capsule_count, sizeof *facts);
    if (st != SU_OK || facts == NULL) {
        free(facts);
        su_host_world_destroy(hw);
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    if (desc->table_count > 1u) {
        uint32_t t;
        hw->table_count = desc->table_count;
        for (t = 0; t < desc->table_count; t++) {
            st = declare_schema(hw, t, desc->tables[t].schema, err);
            if (st != SU_OK) {
                goto fail;
            }
        }
    } else {
        hw->table_count = 1u;
        if (desc->schema != NULL) {
            st = declare_schema(hw, 0u, desc->schema, err);
            if (st != SU_OK) {
                goto fail;
            }
        }
    }
    hw->hz = desc->tick_rate_hz;
    hw->seed = desc->seed;
    hw->max_struct = desc->max_struct_per_tick != 0u ? desc->max_struct_per_tick : SU_CMDQ_DEFAULT_PER_TICK_CAP;
    hw->max_events = desc->max_events_per_tick != 0u ? desc->max_events_per_tick : SU_CMDQ_DEFAULT_PER_TICK_CAP;
    hw->log_max = desc->event_log_max != 0u ? desc->event_log_max : HOST_DEFAULT_EVENT_LOG;

    for (i = 0; i < desc->capsule_count; i++) {
        const SuHostCapsule *c = &desc->capsules[i];
        uint32_t table = desc->capsule_table != NULL ? desc->capsule_table[i] : 0u;
        SuLirModule *m;
        SuDiag diag;
        if (c->capsule_json != NULL && c->lir_json == NULL && c->lir_len == 0u &&
            c->capsule_len <= HOST_CAPSULE_JSON_MAX) {
            st = capsule_facts(c, &facts[i], fwhy, sizeof fwhy);
            nfacts = i + 1u;
            if (st != SU_OK) {
                set_err(err, st, i, UINT64_MAX, "capsule %u: %s", (unsigned)i, fwhy);
                goto fail;
            }
            st = add_host_record(hw, table, c, &facts[i], i, err);
            if (st != SU_OK) {
                goto fail;
            }
            continue;
        }
        if (c->capsule_json == NULL || c->lir_json == NULL || c->capsule_len > HOST_CAPSULE_JSON_MAX ||
            c->lir_len > SU_LIMIT_IR_SOURCE_BYTES) {
            st = set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX, "capsule %u: missing or oversized bytes",
                         (unsigned)i);
            goto fail;
        }
        st = capsule_facts(c, &facts[i], fwhy, sizeof fwhy);
        nfacts = i + 1u;
        if (st != SU_OK) {
            set_err(err, st, i, UINT64_MAX, "capsule %u: %s", (unsigned)i, fwhy);
            goto fail;
        }
        m = calloc(1, sizeof *m);
        if (m == NULL) {
            st = set_err(err, SU_E_RESOURCE_LIMIT, i, UINT64_MAX, "allocation failed");
            goto fail;
        }
        su_diag_init(&diag);
        st = su_lir_verify(c->capsule_json, c->capsule_len, c->lir_json, c->lir_len, m, &diag);
        if (st != SU_OK) {
            free(m);
            set_err(err, st, i, UINT64_MAX, "capsule %u: verification failed at %s: %s", (unsigned)i,
                    diag.json_pointer, diag.message);
            goto fail;
        }
        st = check_declared_facts(hw, table, m->query, facts[i].schema_version, facts[i].layout_generation, i, err);
        if (st != SU_OK) {
            su_lir_module_free(m);
            free(m);
            goto fail;
        }
        st = su_sched_add_table(&hw->sched, table, m, facts[i].schema_version, facts[i].layout_generation, 0u, 1u,
                                0u);
        if (st != SU_OK) {
            su_lir_module_free(m);
            free(m);
            set_err(err, st, i, UINT64_MAX, "capsule %u: cannot join the schedule (schema or duplicate system id)",
                    (unsigned)i);
            goto fail;
        }
    }
    st = admit_state_budget(hw, desc->state_budget_bytes, err);
    if (st != SU_OK) {
        goto fail;
    }
    st = su_sched_finalize(&hw->sched);
    if (st != SU_OK) {
        char why[512];
        uint32_t sys = UINT32_MAX;
        su_sched_explain_mismatch_ex(&hw->sched, why, sizeof why, &sys);
        if (hw->sched.declared || hw->table_count > 1u) {
            set_err(err, st, sys, UINT64_MAX, "a system does not bind the declared world schema%s%s",
                    why[0] != '\0' ? ": " : "", why);
        } else {
            set_err(err, st, UINT32_MAX, UINT64_MAX, "systems disagree on a field's id, name or type%s%s",
                    why[0] != '\0' ? ": " : "", why);
        }
        goto fail;
    }
    st = check_effects(&hw->sched, facts, err);
    if (st != SU_OK) {
        goto fail;
    }
    st = su_sched_enable_entities(&hw->sched, hw->max_struct, hw->max_events, hw->log_max);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "entity runtime caps are out of range");
        goto fail;
    }
    {
        uint64_t max_rows = 0;
        uint64_t base = 0;
        uint32_t t;
        for (t = 0; t < hw->table_count; t++) {
            const uint8_t *fx = desc->table_count > 1u ? desc->tables[t].fixture_json : desc->fixture_json;
            size_t fx_len = desc->table_count > 1u ? desc->tables[t].fixture_len : desc->fixture_len;
            uint64_t cap = desc->table_count > 1u ? desc->tables[t].max_entities : desc->max_entities;
            st = open_table(hw, t, fx, fx_len, cap, err);
            if (st != SU_OK) {
                goto fail;
            }
            hw->tabs[t].rows = su_world_row_count(hw->tabs[t].w);
            hw->tabs[t].base = base;
            if (hw->tabs[t].rows > SU_LIMIT_ROWS - base) {
                st = set_err(err, SU_E_RESOURCE_LIMIT, t, UINT64_MAX,
                             "the tables' capacities exceed the row limit of %llu", (unsigned long long)SU_LIMIT_ROWS);
                goto fail;
            }
            base += hw->tabs[t].rows;
            world_slots = base;
            if (hw->tabs[t].rows > max_rows) {
                max_rows = hw->tabs[t].rows;
            }
        }
        st = su_ent_budget(&hw->sched, max_rows, hw->max_struct, hw->max_events);
        if (st != SU_OK) {
            set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX,
                    "entity tick buffers exceed the resource budget; lower the capacity or the systems");
            goto fail;
        }
    }
    hw->w = hw->tabs[0].w;
    /* Host operators get their own scratch, sized for this world's capacity (never allocated in a tick). */
    for (i = 0; i < hw->sched.count; i++) {
        const HostOperator *op;
        void *inst = NULL;
        if (hw->sched.sys[i].kind != SU_SYS_KIND_HOST) {
            continue;
        }
        op = su_host_operator_find(hw->sched.sys[i].system_id);
        {
            /* An operator bound to a target table (issue 140) holds both tables' rows in its scratch: the cross-table
             * query indexes movers and targets in one row space. */
            uint32_t own = hw->sched.sys[i].table;
            uint64_t inst_rows = hw->tabs[own].rows;
            if (hw->table_count > 1u && desc->capsule_target_table != NULL &&
                desc->capsule_target_table[i] != SU_HOST_TABLE_NONE) {
                inst_rows += hw->tabs[desc->capsule_target_table[i]].rows;
            }
            st = op != NULL ? op->create(op, inst_rows, &inst) : SU_E_INTERNAL;
        }
        if (st == SU_OK) {
            st = su_sched_set_host_instance(&hw->sched, i, inst, op->destroy);
            if (st != SU_OK) {
                op->destroy(inst);
            }
        }
        if (st == SU_OK) {
            hw->ops[i] = op;
            if (op->set_base != NULL) {
                op->set_base(hw->sched.sys[i].host_inst, hw->tabs[hw->sched.sys[i].table].base, world_slots);
            }
        }
        if (st == SU_OK) {
            /* A host operator that appends events (issue 172) runs through the entity tick, so its records join the
             * log with the IR systems' events and count toward the world's per-tick event cap. */
            uint32_t ev_cap = 0u;
            SuHostEventDrain ev_drain = NULL;
            su_host_operator_event_config(op, &ev_cap, &ev_drain);
            if (ev_cap != 0u) {
                st = su_sched_set_host_events(&hw->sched, i, ev_cap, ev_drain);
            }
        }
        if (st == SU_OK && op->identity != NULL) {
            /* Registered operators: flags and this world's scratch size join the system's semantic hash (and so
             * su_host_system_info, the world identity and replay headers). */
            SuSysEntry *e = &hw->sched.sys[i];
            op->identity(e->host_inst, e->mod->semantic_hash);
            su_sha256_hex(e->mod->semantic_hash, e->mod->semantic_hash_hex);
            snprintf(e->semantic_hash, sizeof e->semantic_hash, "%s", e->mod->semantic_hash_hex);
        }
        if (st != SU_OK) {
            set_err(err, st, i, UINT64_MAX, "host operator %s: no scratch for %llu rows%s", hw->sched.sys[i].system_id,
                    (unsigned long long)hw->tabs[hw->sched.sys[i].table].rows,
                    st == SU_E_RESOURCE_LIMIT ? " (capacity above the operator's row limit, or out of memory)" : "");
            goto fail;
        }
    }
    if (hw->table_count > 1u) {
        /* Cross-table host operators (issue 140): a capsule whose operator declares target lanes names its target
         * table, whose declared schema must carry every target lane by name. Bound after the tables exist (the
         * target's rows and base are needed) and before the identity is computed (it covers the binding). */
        for (i = 0; i < hw->sched.count; i++) {
            const SuSysEntry *e = &hw->sched.sys[i];
            uint32_t tgt;
            tgt = desc->capsule_target_table != NULL ? desc->capsule_target_table[i] : SU_HOST_TABLE_NONE;
            if (tgt != SU_HOST_TABLE_NONE &&
                (e->kind != SU_SYS_KIND_HOST || e->host == NULL || e->host->target_field_count == 0u)) {
                st = set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                             "capsule %u (%s): this system takes no target set (a target table needs a host operator "
                             "that declares target lanes)",
                             (unsigned)i, e->system_id);
                goto fail;
            }
            if (e->kind != SU_SYS_KIND_HOST || e->host == NULL || e->host->target_field_count == 0u) {
                continue;
            }
            if (tgt == SU_HOST_TABLE_NONE) {
                continue; /* no target set bound: the operator runs over its own table only */
            }
            st = bind_target(hw, i, tgt, err);
            if (st != SU_OK) {
                goto fail;
            }
        }
    } else if (desc->capsule_target_table != NULL) {
        for (i = 0; i < hw->sched.count; i++) {
            if (desc->capsule_target_table[i] != SU_HOST_TABLE_NONE) {
                st = set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                             "capsule %u: a target set needs a world with several tables", (unsigned)i);
                goto fail;
            }
        }
    }
    /* Operator state (issue 139): owned by the instances; the world keeps the pointers for snapshots and steps. */
    for (i = 0; i < hw->sched.count; i++) {
        if (hw->ops[i] != NULL && hw->ops[i]->state != NULL) {
            hw->opstate[i] = hw->ops[i]->state(hw->sched.sys[i].host_inst);
            hw->opstate_count += hw->opstate[i] != NULL ? 1u : 0u;
        }
    }
    st = input_storage_init(hw);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "no input application storage for %llu rows",
                (unsigned long long)su_world_row_count(hw->w));
        goto fail;
    }
    compute_identity(hw);
    for (i = 0; i < nfacts; i++) {
        su_json_doc_free(&facts[i].doc);
    }
    free(facts);
    *out = hw;
    return SU_OK;
fail:
    for (i = 0; i < nfacts; i++) {
        su_json_doc_free(&facts[i].doc);
    }
    free(facts);
    su_host_world_destroy(hw);
    return st;
}

void su_host_world_destroy(SuHostWorld *w) {
    uint32_t i;
    if (w == NULL) {
        return;
    }
    for (i = 0; i < w->rec_count; i++) {
        free(w->recs[i].writes);
    }
    free(w->recs);
    free(w->in_cells);
    free(w->in_vals);
    for (i = 0; i < SU_HOST_MAX_TABLES; i++) {
        free(w->in_col[i]);
        su_ents_destroy(w->rb_ents[i]);
        su_world_destroy(w->tabs[i].w);
    }
    su_evlog_destroy(w->rb_log);
    su_sched_free(&w->sched);
    free(w);
}

/* ---------- identity and schema ---------- */

uint32_t su_host_api_version(void) {
    return SU_HOST_API_VERSION;
}

const char *su_host_status_name(uint32_t code) {
    return su_status_name(code);
}

const char *su_host_schema_id(const SuHostWorld *w) {
    return w != NULL ? w->sched.schema_id : NULL;
}

uint64_t su_host_tick(const SuHostWorld *w) {
    return w != NULL ? su_world_tick(w->w) : 0u;
}

uint64_t su_host_capacity(const SuHostWorld *w) {
    return w != NULL ? su_world_row_count(w->w) : 0u;
}

uint32_t su_host_tick_rate(const SuHostWorld *w) {
    return w != NULL ? w->hz : 0u;
}

uint64_t su_host_seed(const SuHostWorld *w) {
    return w != NULL ? w->seed : 0u;
}

uint32_t su_host_set_tick_rate(SuHostWorld *w, uint32_t hz) {
    if (w == NULL || hz == 0u || hz > SU_HOST_TICK_RATE_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    w->hz = hz;
    return SU_OK;
}

uint32_t su_host_field_count(const SuHostWorld *w) {
    return w != NULL ? w->sched.tables[0].field_count : 0u;
}

uint32_t su_host_field_count_in(const SuHostWorld *w, uint32_t table, uint32_t *count) {
    if (w == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count != NULL) {
        *count = w->sched.tables[table].field_count;
    }
    return SU_OK;
}

uint32_t su_host_field_info(const SuHostWorld *w, uint32_t index, SuHostFieldInfo *out) {
    return su_host_field_info_in(w, 0u, index, out);
}

uint32_t su_host_field_info_in(const SuHostWorld *w, uint32_t table, uint32_t index, SuHostFieldInfo *out) {
    const SuSchedField *f;
    if (w == NULL || out == NULL || table >= w->table_count || index >= w->sched.tables[table].field_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    f = &w->sched.tables[table].fields[index];
    out->id = f->id;
    out->type = f->type;
    out->access = f->access;
    out->name = f->name;
    return SU_OK;
}

uint32_t su_host_field_id(const SuHostWorld *w, const char *name, uint32_t *id) {
    return su_host_field_id_in(w, 0u, name, id);
}

uint32_t su_host_field_id_in(const SuHostWorld *w, uint32_t table, const char *name, uint32_t *id) {
    const SuSchedField *f;
    uint32_t i;
    if (w == NULL || name == NULL || id == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    f = w->sched.tables[table].fields;
    for (i = 0; i < w->sched.tables[table].field_count; i++) {
        if (strcmp(f[i].name, name) == 0) {
            *id = f[i].id;
            return SU_OK;
        }
    }
    return SU_E_SCHEMA_MISMATCH;
}

/* Index of field id in one table's field table (ascending ids), or -1. */
static long field_index_in(const SuHostWorld *w, uint32_t table, uint32_t id) {
    uint32_t lo = 0;
    uint32_t hi;
    if (w == NULL || table >= w->table_count) {
        return -1;
    }
    hi = w->sched.tables[table].field_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->sched.tables[table].fields[mid].id == id) {
            return (long)mid;
        }
        if (w->sched.tables[table].fields[mid].id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return -1;
}

uint32_t su_host_system_count(const SuHostWorld *w) {
    return w != NULL ? w->sched.count : 0u;
}

uint32_t su_host_system_info(const SuHostWorld *w, uint32_t index, const char **system_id,
                             const char **semantic_hash_hex) {
    if (w == NULL || index >= w->sched.count) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (system_id != NULL) {
        *system_id = w->sched.sys[index].system_id;
    }
    if (semantic_hash_hex != NULL) {
        *semantic_hash_hex = w->sched.sys[index].mod->semantic_hash_hex;
    }
    return SU_OK;
}

uint32_t su_host_world_identity(const SuHostWorld *w, uint8_t out[32]) {
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memcpy(out, w->identity, 32u);
    return SU_OK;
}

/* Operator data (2026-10-20): every host entry named system_id (NULL: every host entry whose operator takes data)
 * receives the bytes; one parse is shared by the entries of one call. All-or-nothing: the entries are checked
 * before any receives data. The world identity becomes SHA-256("SUOPDATA\0" || previous identity || u32 entry
 * count || per entry: u32 index || SHA-256(data)), so snapshots and replays of a world without (or with other) data
 * are refused. */
uint32_t su_host_set_operator_data(SuHostWorld *w, const char *system_id, const uint8_t *data, size_t len,
                                   SuHostError *err) {
    uint32_t i, n = 0, st = SU_OK;
    uint64_t mask = 0;
    void *shared = NULL;
    const HostOperator *first_op = NULL;
    char why[200];
    clear_err(err);
    if (w == NULL || data == NULL || len == 0u) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "world, data or length is NULL/0");
    }
    if (w->stepped) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "operator data must be set before the world's first step");
    }
    for (i = 0; i < w->sched.count; i++) {
        const SuSysEntry *e = &w->sched.sys[i];
        const HostOperator *op;
        if (e->kind != SU_SYS_KIND_HOST || (system_id != NULL && strcmp(e->system_id, system_id) != 0)) {
            continue;
        }
        op = su_host_operator_find(e->system_id);
        if (op == NULL || op->set_data == NULL) {
            if (system_id != NULL) {
                return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX, "host operator %s takes no data",
                               e->system_id);
            }
            continue;
        }
        if ((w->opdata_mask >> i) & 1u) { /* uint64_t: one bit per entry, SU_HOST_MAX_CAPSULES <= 64 */
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX, "host operator %s already has data",
                           e->system_id);
        }
        /* One family per call: the same data callback, and for registered operators the same registration (they
         * share one callback but not one parser). */
        if (first_op != NULL && (op->set_data != first_op->set_data || op->reg != first_op->reg)) {
            return set_err(err, SU_E_INVALID_ARGUMENT, i, UINT64_MAX,
                           "operators of different families in one call; name the system");
        }
        first_op = op;
        mask |= UINT64_C(1) << i;
        n++;
    }
    if (n == 0u) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "no host operator %s%stakes data",
                       system_id != NULL ? system_id : "", system_id != NULL ? " " : "");
    }
    why[0] = '\0';
    for (i = 0; i < w->sched.count && st == SU_OK; i++) {
        if ((mask >> i) & 1u) {
            st = first_op->set_data(first_op, w->sched.sys[i].host_inst, data, len, &shared, why, sizeof why);
            if (st != SU_OK) {
                set_err(err, st, i, UINT64_MAX, "operator data refused by %s: %s", w->sched.sys[i].system_id,
                        why[0] != '\0' ? why : su_host_status_name(st));
            }
        }
    }
    if (shared != NULL && first_op->data_release != NULL) {
        first_op->data_release(shared);
    }
    if (st != SU_OK) {
        /* An entry that already took the data keeps it only on success: recreate is not possible here, so a
         * partial failure is only reachable by out-of-memory after the parse; report it as is. */
        return st;
    }
    {
        SuSha256 c;
        uint8_t h[32], b[4];
        su_sha256_init(&c);
        su_sha256_update(&c, data, len);
        su_sha256_final(&c, h);
        su_sha256_init(&c);
        su_sha256_update(&c, (const uint8_t *)"SUOPDATA", 9u);
        su_sha256_update(&c, w->identity, 32u);
        put_u32(b, n);
        su_sha256_update(&c, b, 4u);
        for (i = 0; i < w->sched.count; i++) {
            if ((mask >> i) & 1u) {
                put_u32(b, i);
                su_sha256_update(&c, b, 4u);
                su_sha256_update(&c, h, 32u);
            }
        }
        su_sha256_final(&c, w->identity);
    }
    w->opdata_mask |= mask;
    return SU_OK;
}

/* ---------- entities ---------- */

static uint32_t log_record(SuHostWorld *w, uint32_t kind, uint32_t slot, uint32_t a, uint32_t b) {
    SuEventLog *log = su_ent_rt_log(w->sched.ent);
    SuEvent ev;
    if (log == NULL) {
        return SU_E_INTERNAL;
    }
    memset(&ev, 0, sizeof ev);
    ev.event_seq = su_evlog_next_seq(log);
    ev.tick = su_world_tick(w->w);
    ev.kind = kind;
    ev.system_index = SU_SYSTEM_INDEX_HOST;
    ev.slot = slot;
    ev.a = a;
    ev.b = b;
    return su_evlog_append(log, &ev, 1u);
}

uint32_t su_host_spawn_in(SuHostWorld *w, uint32_t table, const SuHostValue *values, uint32_t count,
                          uint32_t external_id, uint64_t *entity, SuHostError *err) {
    SuWorldSpawnValue sv[SU_LIMIT_FIELDS];
    SuEntity h = SU_ENTITY_INVALID;
    uint64_t world_slot;
    uint32_t i;
    uint32_t st;

    clear_err(err);
    if (w == NULL || entity == NULL || (values == NULL && count > 0u) || count > SU_LIMIT_FIELDS ||
        table >= w->table_count) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    for (i = 0; i < count; i++) {
        sv[i].field_id = values[i].field_id;
        sv[i].value = values[i].value;
    }
    st = su_world_spawn(w->tabs[table].w, sv, count, external_id, &h);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "spawn refused; world unchanged");
    }
    /* The world handle carries the table through the slot range (host.h, "tables"). */
    world_slot = w->tabs[table].base + (uint64_t)SU_ENTITY_SLOT(h);
    h = SU_ENTITY_MAKE((uint32_t)world_slot, SU_ENTITY_GEN(h));
    st = log_record(w, SU_EVENT_SPAWNED, (uint32_t)world_slot, (uint32_t)world_slot, SU_ENTITY_GEN(h));
    if (st != SU_OK) {
        return set_err(err, SU_E_INTERNAL, UINT32_MAX, UINT64_MAX, "spawn record could not be appended");
    }
    *entity = h;
    return SU_OK;
}

uint32_t su_host_spawn(SuHostWorld *w, const SuHostValue *values, uint32_t count, uint32_t external_id,
                       uint64_t *entity, SuHostError *err) {
    return su_host_spawn_in(w, 0u, values, count, external_id, entity, err);
}

uint32_t su_host_despawn_in(SuHostWorld *w, uint32_t table, uint64_t entity, SuHostError *err) {
    uint32_t st;
    uint32_t handle_table = 0u;
    uint32_t local = 0u;
    SuEntity local_handle;
    uint64_t world_slot;
    clear_err(err);
    if (w == NULL || table >= w->table_count) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "world or table is invalid");
    }
    if (!table_of_slot(w, SU_ENTITY_SLOT(entity), &handle_table, &local) || handle_table != table) {
        return set_err(err, SU_E_STALE_HANDLE, UINT32_MAX, UINT64_MAX, "entity handle is stale");
    }
    local_handle = SU_ENTITY_MAKE(local, SU_ENTITY_GEN(entity));
    st = su_world_despawn(w->tabs[table].w, local_handle);
    if (st != SU_OK) {
        return set_err(err, st == SU_E_INVALID_ARGUMENT ? st : SU_E_STALE_HANDLE, UINT32_MAX, UINT64_MAX,
                       "entity handle is stale");
    }
    {
        uint32_t k;
        for (k = 0; k < w->sched.count; k++) {
            if (w->opstate[k] != NULL) {
                su_hstate_note_despawn(w->opstate[k]); /* DROP_DEAD: the next published tick purges */
            }
        }
    }
    world_slot = SU_ENTITY_SLOT(entity);
    st = log_record(w, SU_EVENT_DESPAWNED, (uint32_t)world_slot, (uint32_t)world_slot, SU_ENTITY_GEN(entity));
    return st == SU_OK ? SU_OK
                       : set_err(err, SU_E_INTERNAL, UINT32_MAX, UINT64_MAX, "despawn record could not be appended");
}

uint32_t su_host_despawn(SuHostWorld *w, uint64_t entity, SuHostError *err) {
    uint32_t table = 0u;
    clear_err(err);
    if (w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "world is NULL");
    }
    if (!table_of_slot(w, SU_ENTITY_SLOT(entity), &table, NULL)) {
        return set_err(err, SU_E_STALE_HANDLE, UINT32_MAX, UINT64_MAX, "entity handle is stale");
    }
    return su_host_despawn_in(w, table, entity, err);
}

uint32_t su_host_entity_at_in(const SuHostWorld *w, uint32_t table, uint32_t slot, uint64_t *entity) {
    SuEntity h = SU_ENTITY_INVALID;
    const SuEnts *e;
    if (w == NULL || entity == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = su_world_entities_committed(w->tabs[table].w);
    if (slot >= su_ents_capacity(e) || !su_ents_slot_alive(e, slot) || su_ents_handle_for_slot(e, slot, &h) != SU_OK) {
        return SU_E_STALE_HANDLE;
    }
    *entity = SU_ENTITY_MAKE((uint32_t)(w->tabs[table].base + slot), SU_ENTITY_GEN(h));
    return SU_OK;
}

uint32_t su_host_entity_at(const SuHostWorld *w, uint32_t slot, uint64_t *entity) {
    return su_host_entity_at_in(w, 0u, slot, entity);
}

uint32_t su_host_alive_in(const SuHostWorld *w, uint32_t table, uint32_t base, uint32_t count, uint8_t *alive) {
    if (w == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_world_alive_view(w->tabs[table].w, base, count, alive);
}

uint32_t su_host_alive(const SuHostWorld *w, uint32_t base, uint32_t count, uint8_t *alive) {
    return su_host_alive_in(w, 0u, base, count, alive);
}

/* ---------- tables (issue 140) ---------- */

uint32_t su_host_table_count(const SuHostWorld *w) {
    return w != NULL ? w->table_count : 0u;
}

uint32_t su_host_table_capacity(const SuHostWorld *w, uint32_t table, uint64_t *rows) {
    if (w == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (rows != NULL) {
        *rows = w->tabs[table].rows;
    }
    return SU_OK;
}

uint32_t su_host_table_schema_id(const SuHostWorld *w, uint32_t table, const char **schema_id) {
    if (w == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (schema_id != NULL) {
        *schema_id = w->sched.tables[table].schema_id;
    }
    return SU_OK;
}

uint32_t su_host_table_base(const SuHostWorld *w, uint32_t table, uint64_t *base) {
    if (w == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (base != NULL) {
        *base = w->tabs[table].base;
    }
    return SU_OK;
}

uint32_t su_host_table_of_handle(const SuHostWorld *w, uint64_t entity, uint32_t *table) {
    uint32_t t = 0u;
    if (w == NULL || table == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!table_of_slot(w, SU_ENTITY_SLOT(entity), &t, NULL)) {
        return SU_E_STALE_HANDLE;
    }
    *table = t;
    return SU_OK;
}

/* ---------- inputs ---------- */

static int input_cmp(const SuHostInput *a, const SuHostInput *b) {
    uint32_t sa = SU_ENTITY_SLOT(a->entity);
    uint32_t sb = SU_ENTITY_SLOT(b->entity);
    if (sa != sb) {
        return sa < sb ? -1 : 1;
    }
    if (a->field_id != b->field_id) {
        return a->field_id < b->field_id ? -1 : 1;
    }
    return 0;
}

static int input_qcmp(const void *a, const void *b) {
    return input_cmp(a, b);
}

static HostRecord *find_record(const SuHostWorld *w, uint64_t tick) {
    uint32_t i;
    for (i = 0; i < w->rec_count; i++) {
        if (w->recs[i].tick == tick) {
            return &w->recs[i];
        }
        if (w->recs[i].tick > tick) {
            break;
        }
    }
    return NULL;
}

static void drop_record(SuHostWorld *w, HostRecord *r) {
    uint32_t i = (uint32_t)(r - w->recs);
    free(r->writes);
    memmove(&w->recs[i], &w->recs[i + 1u], (size_t)(w->rec_count - i - 1u) * sizeof *w->recs);
    w->rec_count--;
}

/* The table a world slot belongs to, and its local slot. 0 when the slot is outside every table. */
static int table_of_slot(const SuHostWorld *w, uint32_t slot, uint32_t *table, uint32_t *local) {
    uint32_t t;
    for (t = 0; t < w->table_count; t++) {
        if ((uint64_t)slot >= w->tabs[t].base && (uint64_t)slot < w->tabs[t].base + w->tabs[t].rows) {
            if (table != NULL) {
                *table = t;
            }
            if (local != NULL) {
                *local = slot - (uint32_t)w->tabs[t].base;
            }
            return 1;
        }
    }
    return 0;
}

/* Merges count writes into tick's record. Nothing is queued on failure. */
static uint32_t queue_writes(SuHostWorld *w, uint64_t tick, const SuHostInput *in, uint32_t count, SuHostError *err) {
    HostRecord *r;
    SuHostInput *merged;
    uint32_t have;
    uint32_t i;
    uint64_t now = su_world_tick(w->w);

    if (tick < now || tick - now >= SU_HOST_INPUT_WINDOW) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, tick,
                       "input tick %llu is not in [%llu, %llu)", (unsigned long long)tick, (unsigned long long)now,
                       (unsigned long long)(now + SU_HOST_INPUT_WINDOW));
    }
    for (i = 0; i < count; i++) {
        uint32_t table = 0u;
        if (in[i].entity == SU_HOST_ENTITY_NONE ||
            !table_of_slot(w, SU_ENTITY_SLOT(in[i].entity), &table, NULL)) {
            return set_err(err, SU_E_STALE_HANDLE, i, tick, "write %u names no slot of this world", (unsigned)i);
        }
        if (field_index_in(w, table, in[i].field_id) < 0) {
            return set_err(err, SU_E_SCHEMA_MISMATCH, i, tick, "write %u names unknown field %u", (unsigned)i,
                           (unsigned)in[i].field_id);
        }
    }
    r = find_record(w, tick);
    have = r != NULL ? r->count : 0u;
    if (count > SU_HOST_INPUT_MAX_WRITES - have) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, tick, "more than %u writes for one tick",
                       (unsigned)SU_HOST_INPUT_MAX_WRITES);
    }
    merged = malloc((size_t)(have + count > 0u ? have + count : 1u) * sizeof *merged);
    if (merged == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, tick, "allocation failed");
    }
    if (have > 0u) {
        memcpy(merged, r->writes, (size_t)have * sizeof *merged);
    }
    if (count > 0u) {
        memcpy(merged + have, in, (size_t)count * sizeof *merged);
    }
    qsort(merged, have + count, sizeof *merged, input_qcmp);
    for (i = 1; i < have + count; i++) {
        if (input_cmp(&merged[i - 1u], &merged[i]) == 0) {
            uint32_t slot = SU_ENTITY_SLOT(merged[i].entity);
            uint32_t field = merged[i].field_id;
            free(merged);
            return set_err(err, SU_E_INVALID_ARGUMENT, field, tick, "slot %u field %u is written twice for tick %llu",
                           (unsigned)slot, (unsigned)field, (unsigned long long)tick);
        }
    }
    if (r == NULL) {
        uint32_t at = 0;
        if (w->rec_count == w->rec_cap) {
            uint32_t ncap = w->rec_cap != 0u ? w->rec_cap * 2u : 16u;
            HostRecord *g = realloc(w->recs, (size_t)ncap * sizeof *g);
            if (g == NULL) {
                free(merged);
                return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, tick, "allocation failed");
            }
            w->recs = g;
            w->rec_cap = ncap;
        }
        while (at < w->rec_count && w->recs[at].tick < tick) {
            at++;
        }
        memmove(&w->recs[at + 1u], &w->recs[at], (size_t)(w->rec_count - at) * sizeof *w->recs);
        w->rec_count++;
        r = &w->recs[at];
        memset(r, 0, sizeof *r);
        r->tick = tick;
    } else {
        free(r->writes);
    }
    r->writes = merged;
    r->count = have + count;
    r->cap = have + count;
    return SU_OK;
}

uint32_t su_host_input_push(SuHostWorld *w, uint64_t tick, const SuHostInput *writes, uint32_t count,
                            SuHostError *err) {
    clear_err(err);
    if (w == NULL || (writes == NULL && count > 0u)) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, tick, "invalid arguments");
    }
    return queue_writes(w, tick, writes, count, err);
}

void su_host_input_clear(SuHostWorld *w, uint64_t from_tick) {
    if (w == NULL) {
        return;
    }
    while (w->rec_count > 0u && w->recs[w->rec_count - 1u].tick >= from_tick) {
        free(w->recs[w->rec_count - 1u].writes);
        w->rec_count--;
    }
}

uint32_t su_host_input_encode(const SuHostWorld *w, uint64_t tick, uint8_t *buf, size_t cap, size_t *written) {
    const HostRecord *r;
    uint32_t n;
    uint32_t i;
    size_t need;
    if (w == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    r = find_record(w, tick);
    n = r != NULL ? r->count : 0u;
    need = HOST_INPUT_HEADER + (size_t)n * HOST_INPUT_ENTRY;
    *written = need;
    if (buf == NULL) {
        return SU_OK;
    }
    if (cap < need) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(buf, HOST_INPUT_MAGIC, 8u);
    put_u32(buf + 8, HOST_INPUT_VERSION);
    put_u64(buf + 12, tick);
    put_u32(buf + 20, n);
    for (i = 0; i < n; i++) {
        uint8_t *p = buf + HOST_INPUT_HEADER + (size_t)i * HOST_INPUT_ENTRY;
        put_u64(p, r->writes[i].entity);
        put_u32(p + 8, r->writes[i].field_id);
        put_u32(p + 12, r->writes[i].value);
    }
    return SU_OK;
}

uint32_t su_host_input_push_encoded(SuHostWorld *w, const uint8_t *buf, size_t len, SuHostError *err) {
    SuHostInput *in;
    uint64_t tick;
    uint32_t n;
    uint32_t i;
    uint32_t st;

    clear_err(err);
    if (w == NULL || buf == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    if (len < HOST_INPUT_HEADER || memcmp(buf, HOST_INPUT_MAGIC, 8u) != 0 || rd_u32(buf + 8) != HOST_INPUT_VERSION) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "not an input record (magic or version)");
    }
    tick = rd_u64(buf + 12);
    n = rd_u32(buf + 20);
    if (n > SU_HOST_INPUT_MAX_WRITES || len != HOST_INPUT_HEADER + (size_t)n * HOST_INPUT_ENTRY) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, tick, "input record length does not match its count");
    }
    in = malloc((size_t)(n > 0u ? n : 1u) * sizeof *in);
    if (in == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, tick, "allocation failed");
    }
    for (i = 0; i < n; i++) {
        const uint8_t *p = buf + HOST_INPUT_HEADER + (size_t)i * HOST_INPUT_ENTRY;
        in[i].entity = rd_u64(p);
        in[i].field_id = rd_u32(p + 8);
        in[i].value = rd_u32(p + 12);
        if (i > 0u && input_cmp(&in[i - 1u], &in[i]) >= 0) {
            free(in);
            return set_err(err, SU_E_PARSE, i, tick, "input record writes are not in canonical order");
        }
    }
    st = queue_writes(w, tick, in, n, err);
    free(in);
    return st;
}

/* One field's saved cells, for undoing the input writes of a failed tick. */
typedef struct UndoCell {
    uint32_t table;      /* table the slot belongs to (issue 140) */
    uint32_t slot;       /* the table's local slot */
    uint32_t field_id;
    uint32_t old;
} UndoCell;

/* Allocates the input application storage of a world (issue 149; ENGINE_CONSTITUTION C11): su_host_step then
 * applies and undoes input records without allocating. A record holds unique (slot, field) writes, at most
 * SU_HOST_INPUT_MAX_WRITES (queue_writes refuses more), so it never has more than min(SU_HOST_INPUT_MAX_WRITES,
 * capacity x field count) writes. */
static uint32_t input_storage_init(SuHostWorld *w) {
    uint64_t total_cells = 0;
    uint64_t max = 0;
    uint32_t t;
    for (t = 0; t < w->table_count; t++) {
        uint64_t rows = w->tabs[t].rows;
        uint64_t cells = rows * (uint64_t)w->sched.tables[t].field_count; /* <= 2^32 x 2^10: no overflow */
        total_cells += cells;
        w->in_col_rows[t] = rows;
        w->in_col[t] = rows <= SIZE_MAX / sizeof *w->in_col[t]
                           ? malloc((size_t)(rows > 0u ? rows : 1u) * sizeof *w->in_col[t])
                           : NULL;
        if (w->in_col[t] == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        if (rows > w->in_rows) {
            w->in_rows = rows;
        }
    }
    max = total_cells < SU_HOST_INPUT_MAX_WRITES ? total_cells : SU_HOST_INPUT_MAX_WRITES;
    w->in_max = (uint32_t)max;
    w->in_cells = malloc((size_t)(max > 0u ? max : 1u) * sizeof *w->in_cells);
    w->in_vals = malloc((size_t)(max > 0u ? max : 1u) * sizeof *w->in_vals);
    if (w->in_cells == NULL || w->in_vals == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    return SU_OK;
}

/* Writes cells (sorted by world slot, field) into the committed roots, one column copy per touched (table, field),
 * through the table's column buffer (w->in_col[table], w->in_col_rows[table] cells). values[i] is written to
 * (cells[i].table, cells[i].slot, cells[i].field_id); slot is the table's local slot. */
static uint32_t write_cells(SuHostWorld *w, const UndoCell *cells, const uint32_t *values, uint32_t n) {
    uint32_t i;
    uint32_t st = SU_OK;
    /* (table, field) pairs in ascending order of first appearance; each pair is written once. */
    for (i = 0; i < n && st == SU_OK; i++) {
        const uint32_t *col = NULL;
        uint64_t rows = 0;
        uint32_t k;
        uint32_t *buf = w->in_col[cells[i].table];
        int seen = 0;
        for (k = 0; k < i; k++) {
            if (cells[k].field_id == cells[i].field_id && cells[k].table == cells[i].table) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        st = su_world_read_committed(w->tabs[cells[i].table].w, cells[i].field_id, &col, &rows);
        if (st != SU_OK) {
            break;
        }
        if (rows > w->in_col_rows[cells[i].table]) {
            st = SU_E_RESOURCE_LIMIT; /* not reached: the capacity is fixed at creation */
            break;
        }
        memcpy(buf, col, (size_t)rows * sizeof *buf);
        for (k = i; k < n; k++) {
            if (cells[k].field_id == cells[i].field_id && cells[k].table == cells[i].table) {
                buf[cells[k].slot] = values[k];
            }
        }
        st = su_world_put_committed(w->tabs[cells[i].table].w, cells[i].field_id, buf, rows);
    }
    return st;
}

/* Applies tick's record to the committed roots. On SU_OK *undo_n cells of w->in_cells hold the old values (valid
 * until the next apply_record). A dead or stale entity is SU_E_STALE_HANDLE, and a record larger than the
 * world's input storage SU_E_RESOURCE_LIMIT; in both cases nothing is written. Allocates nothing. */
static uint32_t apply_record(SuHostWorld *w, const HostRecord *r, uint32_t *undo_n, SuHostError *err) {
    UndoCell *cells = w->in_cells;
    uint32_t *vals = w->in_vals;
    uint32_t i;
    uint32_t st;

    *undo_n = 0;
    if (r->count == 0u) {
        return SU_OK;
    }
    if (r->count > w->in_max) {
        /* Not reached through the API: queue_writes keeps a record within SU_HOST_INPUT_MAX_WRITES unique cells. */
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, r->tick,
                       "input for tick %llu has %u writes, more than the world's input storage (%u)",
                       (unsigned long long)r->tick, (unsigned)r->count, (unsigned)w->in_max);
    }
    for (i = 0; i < r->count; i++) {
        const uint32_t *col = NULL;
        uint64_t rows = 0;
        uint32_t table = 0u;
        uint32_t local = 0u;
        SuEntity local_handle;
        if (!table_of_slot(w, SU_ENTITY_SLOT(r->writes[i].entity), &table, &local)) {
            return set_err(err, SU_E_STALE_HANDLE, UINT32_MAX, r->tick,
                           "input for tick %llu names a dead or stale entity (slot %u)", (unsigned long long)r->tick,
                           (unsigned)SU_ENTITY_SLOT(r->writes[i].entity));
        }
        local_handle = SU_ENTITY_MAKE(local, SU_ENTITY_GEN(r->writes[i].entity));
        if (su_ents_validate(su_world_entities_committed(w->tabs[table].w), local_handle) != SU_OK) {
            return set_err(err, SU_E_STALE_HANDLE, UINT32_MAX, r->tick,
                           "input for tick %llu names a dead or stale entity (slot %u)", (unsigned long long)r->tick,
                           (unsigned)SU_ENTITY_SLOT(r->writes[i].entity));
        }
        (void)su_world_read_committed(w->tabs[table].w, r->writes[i].field_id, &col, &rows);
        cells[i].table = table;
        cells[i].slot = local;
        cells[i].field_id = r->writes[i].field_id;
        cells[i].old = col != NULL ? col[local] : 0u;
        vals[i] = r->writes[i].value;
    }
    st = write_cells(w, cells, vals, r->count);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, r->tick, "input could not be applied");
    }
    *undo_n = r->count;
    return SU_OK;
}

/* Restores the n cells apply_record saved in w->in_cells. Allocates nothing. */
static void undo_record(SuHostWorld *w, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        w->in_vals[i] = w->in_cells[i].old;
    }
    (void)write_cells(w, w->in_cells, w->in_vals, n);
}

/* ---------- stepping ---------- */

/* 1 when a despawn may have been applied by the tick whose events are [seq0, next_seq) of the log: a
 * SU_EVENT_DESPAWNED record among them (an IR event of that kind counts too, harmlessly), or events evicted before
 * they could be read. Reads at most the tick's events, without allocating. */
static int tick_despawned(const SuEventLog *log, uint64_t seq0) {
    uint64_t next = su_evlog_next_seq(log);
    uint64_t cur = seq0;
    if (next == seq0) {
        return 0;
    }
    if (su_evlog_oldest_seq(log) > seq0) {
        return 1;
    }
    while (cur < next) {
        SuEvent ev[32];
        uint64_t n = 0, nc = 0, skipped = 0, dropped = 0, i;
        if (su_evlog_read(log, cur, 32u, ev, &n, &nc, &skipped, &dropped) != SU_OK || n == 0u || skipped != 0u) {
            return 1;
        }
        for (i = 0; i < n; i++) {
            if (ev[i].kind == SU_EVENT_DESPAWNED) {
                return 1;
            }
        }
        cur = nc;
    }
    return 0;
}

uint32_t su_host_step(SuHostWorld *w, uint64_t ticks, uint64_t *done, SuHostError *err) {
    uint64_t n = 0;
    uint32_t st = SU_OK;

    clear_err(err);
    if (done != NULL) {
        *done = 0;
    }
    if (w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "world is NULL");
    }
    while (n < ticks) {
        uint64_t t = su_world_tick(w->w);
        HostRecord *r = find_record(w, t);
        uint32_t undo_n = 0;
        uint64_t one = 0;
        uint32_t failed = UINT32_MAX;
        uint64_t seq0 = su_evlog_next_seq(su_ent_rt_log(w->sched.ent));

        /* A registered operator's failure reason belongs to one tick attempt. */
        {
            uint32_t k;
            for (k = 0; k < w->sched.count; k++) {
                if (w->ops[k] != NULL && w->ops[k]->clear_error != NULL) {
                    w->ops[k]->clear_error(w->sched.sys[k].host_inst);
                }
            }
        }
        /* Records for ticks already behind the world can no longer apply. */
        while (w->rec_count > 0u && w->recs[0].tick < t) {
            drop_record(w, &w->recs[0]);
            r = find_record(w, t);
        }
        if (r != NULL) {
            st = apply_record(w, r, &undo_n, err);
            if (st != SU_OK) {
                break;
            }
        }
        {
            SuWorld *ws[SU_HOST_MAX_TABLES];
            uint32_t k;
            for (k = 0; k < w->table_count; k++) {
                ws[k] = w->tabs[k].w;
            }
            st = su_sched_run_ticks_tables(ws, w->table_count, &w->sched, 1u, NULL, NULL, NULL, &one, &failed);
        }
        if (st != SU_OK || one != 1u) {
            uint32_t k;
            for (k = 0; k < w->sched.count; k++) {
                if (w->opstate[k] != NULL) {
                    su_hstate_abort(w->opstate[k]); /* operator state changes of the failed tick are dropped */
                }
            }
            if (undo_n > 0u) {
                undo_record(w, undo_n);
            }
            if (st == SU_OK) {
                st = SU_E_INTERNAL;
            }
            {
                /* A registered operator says why its run failed (an undeclared write, its own code). */
                const char *detail = "";
                if (failed != UINT32_MAX && failed < w->sched.count &&
                    w->sched.sys[failed].kind == SU_SYS_KIND_HOST) {
                    const HostOperator *op = w->ops[failed];
                    if (op != NULL && op->last_error != NULL) {
                        detail = op->last_error(w->sched.sys[failed].host_inst);
                    }
                }
                set_err(err, st, failed, t, "tick %llu failed%s%s; committed state unchanged%s%s",
                        (unsigned long long)t, failed != UINT32_MAX ? " in system " : "",
                        failed != UINT32_MAX ? w->sched.sys[failed].system_id : "", detail[0] != '\0' ? ": " : "",
                        detail);
            }
            break;
        }
        {
            /* The tick published: its operator state changes become committed state (with SU_HOST_STATE_DROP_DEAD,
             * after erasing the entries of entities the committed entity table no longer holds alive; an entry
             * keyed by another table's handle is checked against that table, issue 140). */
            SuHStateTables tabs;
            uint32_t k;
            int died = -1; /* computed once, only for a world with operator state */
            memset(&tabs, 0, sizeof tabs);
            tabs.count = w->table_count;
            for (k = 0; k < w->table_count; k++) {
                tabs.ents[k] = su_world_entities_committed(w->tabs[k].w);
                tabs.base[k] = w->tabs[k].base;
                tabs.rows[k] = w->tabs[k].rows;
            }
            for (k = 0; k < w->sched.count; k++) {
                if (w->opstate[k] != NULL) {
                    if (died < 0) {
                        died = tick_despawned(su_ent_rt_log(w->sched.ent), seq0);
                    }
                    if (died) {
                        su_hstate_note_despawn(w->opstate[k]);
                    }
                    su_hstate_commit(w->opstate[k], &tabs);
                }
            }
        }
        if (r != NULL) {
            drop_record(w, find_record(w, t));
        }
        n++;
        w->stepped = 1u;
    }
    if (done != NULL) {
        *done = n;
    }
    return st;
}

/* ---------- reads ---------- */

uint32_t su_host_column_in(const SuHostWorld *w, uint32_t table, uint32_t field_id, const uint32_t **data,
                           uint64_t *rows) {
    if (w == NULL || data == NULL || rows == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_world_read_committed(w->tabs[table].w, field_id, data, rows);
}

uint32_t su_host_column(const SuHostWorld *w, uint32_t field_id, const uint32_t **data, uint64_t *rows) {
    return su_host_column_in(w, 0u, field_id, data, rows);
}

uint32_t su_host_read_in(const SuHostWorld *w, uint32_t table, uint32_t field_id, uint64_t first, uint64_t count,
                         uint32_t *out) {
    const uint32_t *col = NULL;
    uint64_t rows = 0;
    uint32_t st;
    if (w == NULL || (out == NULL && count > 0u) || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_world_read_committed(w->tabs[table].w, field_id, &col, &rows);
    if (st != SU_OK) {
        return st;
    }
    if (first > rows || count > rows - first) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (count > 0u) {
        memcpy(out, col + first, (size_t)count * sizeof *out);
    }
    return SU_OK;
}

uint32_t su_host_read(const SuHostWorld *w, uint32_t field_id, uint64_t first, uint64_t count, uint32_t *out) {
    return su_host_read_in(w, 0u, field_id, first, count, out);
}

/* ---------- snapshots ---------- */

/* Operator state section (issue 139): "SUOPSTAT" 0x00 | u32 version | u32 system count | per system with state, in
 * schedule order: u32 schedule index | its encoding (su_hstate_encode). */
static size_t opstate_size(const SuHostWorld *w) {
    size_t n = HOST_OPSTATE_HEADER;
    uint32_t i;
    for (i = 0; i < w->sched.count; i++) {
        if (w->opstate[i] != NULL) {
            n += 4u + su_hstate_encoded_size(w->opstate[i]);
        }
    }
    return n;
}

static void opstate_write(const SuHostWorld *w, uint8_t *p) {
    uint32_t i;
    memcpy(p, HOST_OPSTATE_MAGIC, HOST_OPSTATE_MAGIC_BYTES);
    put_u32(p + HOST_OPSTATE_MAGIC_BYTES, HOST_OPSTATE_VERSION);
    put_u32(p + HOST_OPSTATE_MAGIC_BYTES + 4u, w->opstate_count);
    p += HOST_OPSTATE_HEADER;
    for (i = 0; i < w->sched.count; i++) {
        if (w->opstate[i] != NULL) {
            put_u32(p, i);
            su_hstate_encode(w->opstate[i], p + 4u);
            p += 4u + su_hstate_encoded_size(w->opstate[i]);
        }
    }
}

/* Parses the section strictly into every state's working side (the committed states are untouched). On failure the
 * loaded sides are dropped and err says why (SU_E_PARSE). */
static uint32_t opstate_load(SuHostWorld *w, const uint8_t *p, size_t len, SuHostError *err) {
    size_t pos = HOST_OPSTATE_HEADER;
    uint32_t i;
    char why[160];
    uint32_t st = SU_OK;
    if (len < HOST_OPSTATE_HEADER || memcmp(p, HOST_OPSTATE_MAGIC, HOST_OPSTATE_MAGIC_BYTES) != 0 ||
        rd_u32(p + HOST_OPSTATE_MAGIC_BYTES) != HOST_OPSTATE_VERSION ||
        rd_u32(p + HOST_OPSTATE_MAGIC_BYTES + 4u) != w->opstate_count) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX,
                       "operator state section: magic, version or system count is wrong");
    }
    for (i = 0; i < w->sched.count && st == SU_OK; i++) {
        size_t used = 0;
        if (w->opstate[i] == NULL) {
            continue;
        }
        if (len - pos < 4u || rd_u32(p + pos) != i) {
            st = set_err(err, SU_E_PARSE, i, UINT64_MAX, "operator state section: system index %u expected",
                         (unsigned)i);
            break;
        }
        pos += 4u;
        why[0] = '\0';
        st = su_hstate_load(w->opstate[i], p + pos, len - pos, &used, why, sizeof why);
        if (st != SU_OK) {
            set_err(err, st, i, UINT64_MAX, "operator state of system %u (%s): %s", (unsigned)i,
                    w->sched.sys[i].system_id, why);
            break;
        }
        pos += used;
    }
    if (st == SU_OK && pos != len) {
        st = set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "operator state section has trailing bytes");
    }
    if (st != SU_OK) {
        for (i = 0; i < w->sched.count; i++) {
            if (w->opstate[i] != NULL) {
                su_hstate_abort(w->opstate[i]);
            }
        }
    }
    return st;
}

/* Exchanges every state's committed and loaded sides (a second call undoes the first). */
static void opstate_swap(SuHostWorld *w) {
    uint32_t i;
    for (i = 0; i < w->sched.count; i++) {
        if (w->opstate[i] != NULL) {
            su_hstate_swap(w->opstate[i]);
        }
    }
}

/* Snapshot layout: the parts of a snapshot, in file order, for write, restore and in-place restore. A single-table
 * world keeps versions 1 and 2 (state | log [| operator state]); a multi-table world uses version 3
 * (u32 table count | per table: u64 state length | state | u64 log length | log [| operator state]). */
typedef struct SnapLayout {
    uint32_t version;
    uint32_t table_count;
    uint64_t state_len[SU_HOST_MAX_TABLES];
    size_t state_off[SU_HOST_MAX_TABLES];
    uint64_t log_len;
    size_t log_off;
    size_t op_off;
    size_t op_len;
} SnapLayout;

/* Splits a snapshot for w: header checks, then the per-table states, the log and the operator state part (version 2
 * and 3 only; op_off and op_len 0 otherwise). Every part must be in range and the parts must cover len exactly. */
static uint32_t snapshot_split(const SuHostWorld *w, const uint8_t *buf, size_t len, SnapLayout *L, SuHostError *err) {
    uint32_t version;
    uint32_t want;
    size_t pos = HOST_SNAP_HEADER;
    size_t rest;
    uint32_t t;

    memset(L, 0, sizeof *L);
    want = w->table_count > 1u ? HOST_SNAP_VERSION_TABLES
                               : (w->opstate_count > 0u ? HOST_SNAP_VERSION_OPSTATE : HOST_SNAP_VERSION);
    if (len < HOST_SNAP_HEADER + 8u || memcmp(buf, HOST_SNAP_MAGIC, 8u) != 0 ||
        ((version = rd_u32(buf + 8)) != HOST_SNAP_VERSION && version != HOST_SNAP_VERSION_OPSTATE &&
         version != HOST_SNAP_VERSION_TABLES)) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "not a host snapshot (magic or version)");
    }
    if (memcmp(buf + 12, w->identity, 32u) != 0) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                       "snapshot belongs to a different world identity (schema, systems, tables, capacity or caps)");
    }
    if (version != want) {
        if (w->table_count > 1u) {
            return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX,
                           "snapshot version %u does not match the world (version 3 exactly when it has tables)",
                           (unsigned)version);
        }
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX,
                       "snapshot version %u does not match the world (version 2 exactly when it has operator state)",
                       (unsigned)version);
    }
    L->version = version;
    L->table_count = 1u;
    if (version != HOST_SNAP_VERSION_TABLES) {
        /* The state length is the last word of the fixed header (versions 1 and 2), as released. */
        L->state_len[0] = rd_u64(buf + HOST_SNAP_HEADER - 8u);
        pos = HOST_SNAP_HEADER;
        if (L->state_len[0] > (uint64_t)(len - pos)) {
            return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot state length is out of range");
        }
        L->state_off[0] = pos;
        pos += (size_t)L->state_len[0];
    } else {
        /* Version 3: the table count takes the header word that versions 1 and 2 use for the state length. */
        pos = HOST_SNAP_HEADER - 8u;
        if (len - pos < 4u) {
            return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot table count is out of range");
        }
        L->table_count = rd_u32(buf + pos);
        pos += 4u;
        if (L->table_count != w->table_count) {
            return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                           "snapshot has %u tables, the world has %u", (unsigned)L->table_count,
                           (unsigned)w->table_count);
        }
        for (t = 0; t < L->table_count; t++) {
            if (len - pos < 8u) {
                return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot table %u length is out of range",
                               (unsigned)t);
            }
            L->state_len[t] = rd_u64(buf + pos);
            pos += 8u;
            if (L->state_len[t] > (uint64_t)(len - pos)) {
                return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot table %u length is out of range",
                               (unsigned)t);
            }
            L->state_off[t] = pos;
            pos += (size_t)L->state_len[t];
        }
    }
    if (len - pos < 8u) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot length does not match its parts");
    }
    L->log_len = rd_u64(buf + pos);
    pos += 8u;
    if (L->log_len > (uint64_t)(len - pos)) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot length does not match its parts");
    }
    L->log_off = pos;
    pos += (size_t)L->log_len;
    rest = len - pos;
    if (w->opstate_count == 0u) {
        if (rest != 0u) {
            return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot length does not match its parts");
        }
        return SU_OK;
    }
    if (rest < 8u || rd_u64(buf + pos) != (uint64_t)(rest - 8u)) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot length does not match its parts");
    }
    L->op_off = pos + 8u;
    L->op_len = rest - 8u;
    return SU_OK;
}

static uint32_t snapshot_parts(const SuHostWorld *w, size_t *state_len, uint64_t *log_len, size_t *total) {
    size_t sum = 0;
    size_t per_table = 0;
    size_t head;
    uint32_t t;
    uint32_t st;
    *state_len = 0;
    for (t = 0; t < w->table_count; t++) {
        size_t sl = 0;
        st = su_world_canonical_size(w->tabs[t].w, &sl);
        if (st != SU_OK) {
            return st;
        }
        if (sl > SIZE_MAX - sum - 8u) {
            return SU_E_RESOURCE_LIMIT;
        }
        sum += sl;
        per_table += 8u;
    }
    if (t == 1u) {
        *state_len = sum;
    }
    st = su_evlog_dump(su_ent_rt_log(w->sched.ent), NULL, 0u, log_len);
    if (st != SU_OK) {
        return st;
    }
    /* Versions 1 and 2 spend the header's last word on the state length; version 3 replaces it with a u32 table
     * count, so its per-table parts start 4 bytes after that word. */
    head = w->table_count > 1u ? (HOST_SNAP_HEADER - 8u + 4u) : HOST_SNAP_HEADER;
    if (w->table_count > 1u) {
        if (*log_len > (uint64_t)(SIZE_MAX - head - per_table - sum - 8u)) {
            return SU_E_RESOURCE_LIMIT;
        }
        *total = head + per_table + sum + 8u + (size_t)*log_len;
    } else {
        if (*log_len > (uint64_t)(SIZE_MAX - head - sum - 8u)) {
            return SU_E_RESOURCE_LIMIT;
        }
        *total = head + sum + 8u + (size_t)*log_len;
    }
    if (w->opstate_count > 0u) {
        size_t op = opstate_size(w);
        if (op > SIZE_MAX - 8u - *total) {
            return SU_E_RESOURCE_LIMIT;
        }
        *total += 8u + op;
    }
    return SU_OK;
}

uint32_t su_host_snapshot_size(const SuHostWorld *w, size_t *size) {
    size_t state_len = 0;
    uint64_t log_len = 0;
    if (w == NULL || size == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return snapshot_parts(w, &state_len, &log_len, size);
}

uint32_t su_host_snapshot_write(const SuHostWorld *w, uint8_t *buf, size_t cap, size_t *written) {
    size_t state_len = 0;
    size_t got = 0;
    uint64_t log_len = 0;
    uint64_t log_got = 0;
    size_t total = 0;
    size_t off;
    uint32_t t;
    uint32_t st;

    if (w == NULL || written == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = snapshot_parts(w, &state_len, &log_len, &total);
    if (st != SU_OK) {
        return st;
    }
    *written = total;
    if (buf == NULL || cap < total) {
        return buf == NULL ? SU_E_INVALID_ARGUMENT : SU_E_RESOURCE_LIMIT;
    }
    memcpy(buf, HOST_SNAP_MAGIC, 8u);
    put_u32(buf + 8, w->table_count > 1u ? HOST_SNAP_VERSION_TABLES
                                         : (w->opstate_count > 0u ? HOST_SNAP_VERSION_OPSTATE : HOST_SNAP_VERSION));
    memcpy(buf + 12, w->identity, 32u);
    put_u64(buf + 44, su_world_tick(w->w));
    if (w->table_count > 1u) {
        put_u32(buf + 52, w->table_count);
        off = 56u;
        for (t = 0; t < w->table_count; t++) {
            size_t sl = 0;
            st = su_world_canonical_size(w->tabs[t].w, &sl);
            if (st != SU_OK) {
                return st;
            }
            put_u64(buf + off, (uint64_t)sl);
            off += 8u;
            st = su_world_canonical_write(w->tabs[t].w, buf + off, sl, &got);
            if (st != SU_OK || got != sl) {
                return st != SU_OK ? st : SU_E_INTERNAL;
            }
            off += sl;
        }
    } else {
        put_u64(buf + 52, (uint64_t)state_len);
        off = HOST_SNAP_HEADER;
        st = su_world_canonical_write(w->tabs[0].w, buf + off, state_len, &got);
        if (st != SU_OK || got != state_len) {
            return st != SU_OK ? st : SU_E_INTERNAL;
        }
        off += state_len;
    }
    put_u64(buf + off, log_len);
    off += 8u;
    st = su_evlog_dump(su_ent_rt_log(w->sched.ent), buf + off, log_len, &log_got);
    if (st != SU_OK || log_got != log_len) {
        return st != SU_OK ? st : SU_E_INTERNAL;
    }
    if (w->opstate_count > 0u) {
        off += (size_t)log_len;
        put_u64(buf + off, (uint64_t)(total - off - 8u));
        opstate_write(w, buf + off + 8u);
    }
    return SU_OK;
}

uint32_t su_host_snapshot_restore(SuHostWorld *w, const uint8_t *buf, size_t len, SuHostError *err) {
    SuWorldFieldDesc *fd = NULL;
    SuWorldDesc wd;
    SuWorld *nw[SU_HOST_MAX_TABLES];
    SuWorld *old_w[SU_HOST_MAX_TABLES];
    SuEventLog *log = NULL;
    SuEvent *evs = NULL;
    SuEntRt *nrt = NULL;
    SuEntRt *old_rt;
    SnapLayout L;
    uint64_t n = 0;
    uint64_t got = 0;
    uint64_t ms = 0;
    uint64_t me = 0;
    uint64_t lm = 0;
    uint8_t *check = NULL;
    size_t check_len = 0;
    int op_loaded = 0;
    uint32_t t;
    uint32_t st;

    memset(nw, 0, sizeof nw);
    clear_err(err);
    if (w == NULL || buf == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    st = snapshot_split(w, buf, len, &L, err);
    if (st != SU_OK) {
        return st;
    }
    for (t = 0; t < w->table_count; t++) {
        fd = table_fields(&w->sched, t);
        if (fd == NULL) {
            st = set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
            goto fail;
        }
        memset(&wd, 0, sizeof wd);
        wd.schema_id = w->sched.tables[t].schema_id;
        wd.schema_version = w->sched.tables[t].schema_version;
        wd.layout_generation = w->sched.tables[t].layout_generation;
        wd.fields = fd;
        wd.field_count = w->sched.tables[t].field_count;
        st = su_world_restore_canonical(&wd, buf + L.state_off[t], (size_t)L.state_len[t], &nw[t]);
        free(fd);
        fd = NULL;
        if (st != SU_OK) {
            set_err(err, st, UINT32_MAX, UINT64_MAX, "world state does not restore (table %u)", (unsigned)t);
            goto fail;
        }
        if (su_world_row_count(nw[t]) != w->tabs[t].rows || su_world_tick(nw[t]) != rd_u64(buf + 44)) {
            st = set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                         "snapshot capacity or tick disagrees (table %u)", (unsigned)t);
            goto fail;
        }
    }
    st = su_evlog_parse(buf + L.log_off, L.log_len, &log);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "event log does not parse");
        goto fail;
    }
    (void)su_ent_rt_caps(w->sched.ent, &ms, &me, &lm);
    if (su_evlog_capacity(log) != lm) {
        st = set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX, "event log capacity disagrees");
        goto fail;
    }
    n = su_evlog_count(log);
    evs = malloc((size_t)(n > 0u ? n : 1u) * sizeof *evs);
    if (evs == NULL || su_evlog_copy_retained(log, evs, n, &got) != SU_OK || got != n) {
        st = set_err(err, evs == NULL ? SU_E_RESOURCE_LIMIT : SU_E_INTERNAL, UINT32_MAX, UINT64_MAX,
                     "event log could not be copied");
        goto fail;
    }
    st = su_ent_rt_create_restored(ms, me, lm, su_evlog_next_seq(log), su_evlog_dropped(log), evs, n, &nrt);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "event log does not restore");
        goto fail;
    }
    if (w->opstate_count > 0u) {
        /* Operator state into the states' free sides (the committed states are untouched until the swap). */
        st = opstate_load(w, buf + L.op_off, L.op_len, err);
        if (st != SU_OK) {
            goto fail;
        }
        op_loaded = 1;
    }
    /* Swap in the new state, then require the bytes to be canonical: the restored state must encode back to
     * exactly the input. Otherwise the old state is put back. */
    old_rt = w->sched.ent;
    for (t = 0; t < w->table_count; t++) {
        old_w[t] = w->tabs[t].w;
        w->tabs[t].w = nw[t];
        nw[t] = NULL;
    }
    w->w = w->tabs[0].w;
    w->sched.ent = nrt;
    nrt = NULL;
    if (op_loaded) {
        opstate_swap(w);
    }
    check = malloc(len);
    if (check == NULL || su_host_snapshot_write(w, check, len, &check_len) != SU_OK || check_len != len ||
        memcmp(check, buf, len) != 0) {
        for (t = 0; t < w->table_count; t++) {
            nw[t] = w->tabs[t].w;
            w->tabs[t].w = old_w[t];
        }
        w->w = w->tabs[0].w;
        nrt = w->sched.ent;
        w->sched.ent = old_rt;
        if (op_loaded) {
            opstate_swap(w);
        }
        st = set_err(err, check == NULL ? SU_E_RESOURCE_LIMIT : SU_E_TYPE, UINT32_MAX, UINT64_MAX,
                     "snapshot bytes are not canonical");
        goto fail;
    }
    for (t = 0; t < w->table_count; t++) {
        su_world_destroy(old_w[t]);
    }
    su_ent_rt_destroy(old_rt);
    st = SU_OK;
fail:
    free(check);
    free(evs);
    su_evlog_destroy(log);
    su_ent_rt_destroy(nrt);
    for (t = 0; t < w->table_count; t++) {
        su_world_destroy(nw[t]);
    }
    return st;
}

uint32_t su_host_operator_state(const SuHostWorld *w, uint32_t index, const SuHostState **state) {
    if (state != NULL) {
        *state = NULL;
    }
    if (w == NULL || state == NULL || index >= w->sched.count || w->opstate[index] == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *state = w->opstate[index];
    return SU_OK;
}

uint32_t su_host_operator_counters(const SuHostWorld *w, uint32_t index, SuHostOperatorCounters *out) {
    uint32_t size;
    if (w == NULL || out == NULL || out->struct_size < sizeof *out || out->reserved != 0u ||
        index >= w->sched.count || w->sched.sys[index].kind != SU_SYS_KIND_HOST || w->ops[index] == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (w->ops[index]->counters == NULL) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    size = out->struct_size;
    memset(out, 0, sizeof *out);
    out->struct_size = size;
    w->ops[index]->counters(w->sched.sys[index].host_inst, out);
    return SU_OK;
}

uint32_t su_host_digest(const SuHostWorld *w, uint8_t out[32]) {
    size_t size = 0;
    size_t got = 0;
    uint8_t *buf;
    uint32_t st;
    SuSha256 c;
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_host_snapshot_size(w, &size);
    if (st != SU_OK) {
        return st;
    }
    buf = malloc(size);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_host_snapshot_write(w, buf, size, &got);
    if (st == SU_OK) {
        su_sha256_init(&c);
        su_sha256_update(&c, buf, got);
        su_sha256_final(&c, out);
    }
    free(buf);
    return st;
}

void su_host_hex(const uint8_t digest[32], char out[65]) {
    su_sha256_hex(digest, out);
}

/* ---------- events ---------- */

_Static_assert(sizeof(SuHostEvent) == sizeof(SuEvent), "SuHostEvent mirrors SuEvent");
_Static_assert(offsetof(SuHostEvent, kind) == offsetof(SuEvent, kind), "SuHostEvent.kind");
_Static_assert(offsetof(SuHostEvent, b) == offsetof(SuEvent, b), "SuHostEvent.b");
_Static_assert(SU_HOST_EVENT_SYSTEM_HOST == SU_SYSTEM_INDEX_HOST, "host system index");
_Static_assert(SU_HOST_EVENT_REJECTED == SU_EVENT_REJECTED, "rejected kind");
_Static_assert(SU_HOST_EVENT_SPAWNED == SU_EVENT_SPAWNED, "spawned kind");
_Static_assert(SU_HOST_EVENT_DESPAWNED == SU_EVENT_DESPAWNED, "despawned kind");

static void copy_event(SuHostEvent *dst, const SuEvent *src) {
    dst->event_seq = src->event_seq;
    dst->tick = src->tick;
    dst->kind = src->kind;
    dst->system_index = src->system_index;
    dst->slot = src->slot;
    dst->seq = src->seq;
    dst->a = src->a;
    dst->b = src->b;
}

/* One retained event by sequence number (oldest_seq <= seq < next_seq). */
static int event_at(const SuEventLog *log, uint64_t seq, SuEvent *ev) {
    uint64_t one = 0;
    uint64_t next = 0;
    uint64_t skipped = 0;
    uint64_t dropped = 0;
    return su_evlog_read(log, seq, 1u, ev, &one, &next, &skipped, &dropped) == SU_OK && one == 1u && skipped == 0u;
}

uint64_t su_host_event_next_seq(const SuHostWorld *w) {
    return w != NULL ? su_evlog_next_seq(su_ent_rt_log(w->sched.ent)) : 0u;
}

uint32_t su_host_events_read(const SuHostWorld *w, uint64_t cursor, SuHostEvent *out, uint32_t max,
                             uint32_t *count, uint64_t *next_cursor, uint64_t *skipped) {
    const SuEventLog *log;
    uint64_t oldest;
    uint64_t next;
    uint64_t pos;
    uint32_t n = 0;

    if (w == NULL || count == NULL || next_cursor == NULL || (out == NULL && max > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    log = su_ent_rt_log(w->sched.ent);
    oldest = su_evlog_oldest_seq(log);
    next = su_evlog_next_seq(log);
    if (cursor > next) {
        return SU_E_INVALID_ARGUMENT;
    }
    pos = cursor < oldest ? oldest : cursor;
    if (skipped != NULL) {
        *skipped = pos - cursor;
    }
    while (n < max && pos < next) {
        SuEvent ev;
        if (!event_at(log, pos, &ev)) {
            return SU_E_INTERNAL;
        }
        copy_event(&out[n++], &ev);
        pos++;
    }
    *count = n;
    *next_cursor = pos;
    return SU_OK;
}

uint32_t su_host_events_for_tick(const SuHostWorld *w, uint64_t tick, SuHostEvent *out, uint32_t max,
                                 uint32_t *count, uint32_t *total, uint32_t *complete) {
    const SuEventLog *log;
    uint64_t oldest;
    uint64_t next;
    uint64_t seq;
    uint32_t n = 0;
    uint32_t matched = 0;
    int earlier = 0;

    if (w == NULL || count == NULL || (out == NULL && max > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    log = su_ent_rt_log(w->sched.ent);
    oldest = su_evlog_oldest_seq(log);
    next = su_evlog_next_seq(log);
    for (seq = oldest; seq < next; seq++) {
        SuEvent ev;
        if (!event_at(log, seq, &ev)) {
            return SU_E_INTERNAL;
        }
        if (seq == oldest) {
            earlier = ev.tick < tick;
        }
        if (ev.tick == tick) {
            if (n < max) {
                copy_event(&out[n++], &ev);
            }
            if (matched < UINT32_MAX) {
                matched++;
            }
        }
    }
    *count = n;
    if (total != NULL) {
        *total = matched;
    }
    if (complete != NULL) {
        *complete = (su_evlog_dropped(log) == 0u || earlier) ? 1u : 0u;
    }
    return SU_OK;
}

/* ---------- rollback-grade state operations ---------- */

static uint32_t rollback_scratch(SuHostWorld *w) {
    uint64_t ms = 0;
    uint64_t me = 0;
    uint64_t lm = 0;
    uint32_t t;
    for (t = 0; t < w->table_count; t++) {
        if (w->rb_ents[t] == NULL) {
            SuEntsDesc d;
            memset(&d, 0, sizeof d);
            d.max_entities = w->tabs[t].rows;
            if (su_ents_create(&d, &w->rb_ents[t]) != SU_OK) {
                return SU_E_RESOURCE_LIMIT;
            }
        }
    }
    if (w->rb_log == NULL) {
        (void)su_ent_rt_caps(w->sched.ent, &ms, &me, &lm);
        if (su_evlog_create(lm, 0u, &w->rb_log) != SU_OK) {
            return SU_E_RESOURCE_LIMIT;
        }
    }
    return SU_OK;
}

uint32_t su_host_snapshot_restore_inplace(SuHostWorld *w, const uint8_t *buf, size_t len, SuHostError *err) {
    SnapLayout L;
    uint32_t t;
    uint32_t st;

    clear_err(err);
    if (w == NULL || buf == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    st = snapshot_split(w, buf, len, &L, err);
    if (st != SU_OK) {
        return st;
    }
    st = rollback_scratch(w);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "allocation of the rollback scratch failed");
    }
    /* 1. Validate each table's state into its scratch entity table (the world is untouched). */
    for (t = 0; t < w->table_count; t++) {
        st = su_world_restore_canonical_check(w->tabs[t].w, buf + L.state_off[t], (size_t)L.state_len[t],
                                              w->rb_ents[t]);
        if (st != SU_OK) {
            return set_err(err, st, UINT32_MAX, UINT64_MAX, "world state does not restore (table %u)", (unsigned)t);
        }
        if (rd_u64(buf + L.state_off[t] + 13u) != rd_u64(buf + 44)) {
            return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                           "snapshot capacity or tick disagrees (table %u)", (unsigned)t);
        }
    }
    /* 2. Event log into the scratch log (validated, then filled; the live log is untouched). */
    st = su_evlog_parse_into(w->rb_log, buf + L.log_off, L.log_len);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX,
                       st == SU_E_SCHEMA_MISMATCH ? "event log capacity disagrees" : "event log does not parse");
    }
    /* 2b. Operator state into the states' free sides (allocated with the world; the committed states are untouched). */
    if (w->opstate_count > 0u) {
        st = opstate_load(w, buf + L.op_off, L.op_len, err);
        if (st != SU_OK) {
            return st;
        }
    }
    /* 3. Commit: none of these steps can fail after the checks above. */
    for (t = 0; t < w->table_count; t++) {
        su_world_restore_canonical_commit(w->tabs[t].w, buf + L.state_off[t], w->rb_ents[t]);
    }
    (void)su_evlog_swap(su_ent_rt_log(w->sched.ent), w->rb_log);
    if (w->opstate_count > 0u) {
        opstate_swap(w);
    }
    return SU_OK;
}

uint32_t su_host_digest_fast(const SuHostWorld *w, uint64_t *out) {
    size_t size = 0;
    size_t got = 0;
    uint8_t *buf;
    uint32_t st;
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_host_snapshot_size(w, &size);
    if (st != SU_OK) {
        return st;
    }
    buf = malloc(size);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_host_snapshot_write(w, buf, size, &got);
    if (st == SU_OK) {
        *out = su_xxh64(buf, got, 0u);
    }
    free(buf);
    return st;
}

/* ---------- IR 0.3 typed lanes (numeric-3d) ---------- */

/* Value type and lane of the field at index i of table's field table. 64-bit fields are adjacent lane pairs
 * (id, id + 1) in the ascending table, so pairs are counted from the start. */
static void lane_of(const SuHostWorld *w, uint32_t table, uint32_t i, uint32_t *vt, uint32_t *lane) {
    const SuSchedField *f = w->sched.tables[table].fields;
    uint32_t k = 0;
    *vt = f[i].value_type == 0u ? SU_TYPE_U32 : f[i].value_type;
    while (k < i) {
        uint32_t t = f[k].value_type;
        k += (t == SU_TYPE_I64 || t == SU_TYPE_Q32) ? 2u : 1u;
    }
    *lane = k == i ? 0u : 1u;
}

/* Index of the lane-0 entry of a 64-bit (i64 or q32) field, or -1. */
static long wide_index(const SuHostWorld *w, uint32_t table, uint32_t field_id) {
    long i = field_index_in(w, table, field_id);
    uint32_t vt, lane;
    if (i < 0) {
        return -1;
    }
    lane_of(w, table, (uint32_t)i, &vt, &lane);
    if ((vt != SU_TYPE_I64 && vt != SU_TYPE_Q32) || lane != 0u ||
        (uint32_t)i + 1u >= w->sched.tables[table].field_count) {
        return -2;
    }
    return i;
}

uint32_t su_host_field_value_type_in(const SuHostWorld *w, uint32_t table, uint32_t field_id, uint32_t *value_type,
                                     uint32_t *lane) {
    long i;
    if (w == NULL || value_type == NULL || lane == NULL || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = field_index_in(w, table, field_id);
    if (i < 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    lane_of(w, table, (uint32_t)i, value_type, lane);
    return SU_OK;
}

uint32_t su_host_field_value_type(const SuHostWorld *w, uint32_t field_id, uint32_t *value_type, uint32_t *lane) {
    return su_host_field_value_type_in(w, 0u, field_id, value_type, lane);
}

uint32_t su_host_read_i64_in(const SuHostWorld *w, uint32_t table, uint32_t field_id, uint64_t first, uint64_t count,
                             int64_t *out) {
    const uint32_t *lo = NULL, *hi = NULL;
    uint64_t rows = 0, k;
    long i;
    uint32_t st;
    if (w == NULL || (out == NULL && count > 0u) || table >= w->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = wide_index(w, table, field_id);
    if (i == -1) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (i < 0) {
        return SU_E_TYPE;
    }
    st = su_world_read_committed(w->tabs[table].w, field_id, &lo, &rows);
    if (st == SU_OK) {
        st = su_world_read_committed(w->tabs[table].w, field_id + 1u, &hi, &rows);
    }
    if (st != SU_OK) {
        return st;
    }
    if (first > rows || count > rows - first) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (k = 0; k < count; k++) {
        uint64_t u = ((uint64_t)hi[first + k] << 32) | (uint64_t)lo[first + k];
        out[k] = u <= (uint64_t)INT64_MAX ? (int64_t)u : -(int64_t)(~u) - 1;
    }
    return SU_OK;
}

uint32_t su_host_read_i64(const SuHostWorld *w, uint32_t field_id, uint64_t first, uint64_t count, int64_t *out) {
    return su_host_read_i64_in(w, 0u, field_id, first, count, out);
}

uint32_t su_host_input_push_i64(SuHostWorld *w, uint64_t tick, uint64_t entity, uint32_t field_id, int64_t value,
                                SuHostError *err) {
    SuHostInput two[2];
    uint32_t table = 0u;
    long i;
    clear_err(err);
    if (w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, tick, "invalid arguments");
    }
    if (!table_of_slot(w, SU_ENTITY_SLOT(entity), &table, NULL)) {
        return set_err(err, SU_E_STALE_HANDLE, field_id, tick, "the entity names no slot of this world");
    }
    i = wide_index(w, table, field_id);
    if (i == -1) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, field_id, tick, "field %u is not in the schema", (unsigned)field_id);
    }
    if (i < 0) {
        return set_err(err, SU_E_TYPE, field_id, tick, "field %u is not the low lane of an i64 or q32 field",
                       (unsigned)field_id);
    }
    two[0].entity = entity;
    two[0].field_id = field_id;
    two[0].value = (uint32_t)((uint64_t)value & 0xFFFFFFFFu);
    two[1].entity = entity;
    two[1].field_id = field_id + 1u;
    two[1].value = (uint32_t)((uint64_t)value >> 32);
    return su_host_input_push(w, tick, two, 2u, err);
}

uint32_t su_host_memory(const SuHostWorld *w, SuHostMemory *out) {
    SuHostMemory m;
    uint32_t t;
    size_t sz;
    if (w == NULL || out == NULL || out->struct_size < SU_HOST_MEMORY_V1_SIZE) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&m, 0, sizeof m);
    m.struct_size = (uint32_t)sizeof m;
    m.table_count = w->table_count;
    for (t = 0; t < w->table_count; t++) {
        SuWorldMemory wm;
        if (su_world_memory(w->tabs[t].w, &wm) != SU_OK) {
            return SU_E_INVALID_ARGUMENT;
        }
        m.table_total[t] = wm.total;
        m.tables_total += wm.total;
    }
    /* The input application buffers of su_host_step: the undo cells and their values, and one column buffer per
     * table (issue 149 storage, allocated at creation). */
    m.input_storage = (uint64_t)w->in_max * (uint64_t)(sizeof(struct UndoCell) + sizeof(uint32_t));
    for (t = 0; t < w->table_count; t++) {
        m.input_storage += w->in_col_rows[t] * (uint64_t)sizeof(uint32_t);
    }
    /* Queued input records (issue 140: the earlier claim left them out): the record array, which grows with the
     * queue, and each record's write array (cap entries, freed when the record is consumed). */
    m.queued_inputs = (uint64_t)w->rec_cap * (uint64_t)sizeof(HostRecord);
    for (t = 0; t < w->rec_count; t++) {
        m.queued_inputs += (uint64_t)w->recs[t].cap * (uint64_t)sizeof(SuHostInput);
    }
    /* su_host_snapshot_restore_inplace's scratch (allocated by its first call, never freed before destruction):
     * one entity table per table and the event log. */
    for (t = 0; t < SU_HOST_MAX_TABLES; t++) {
        m.restore_scratch += su_ents_memory_bytes(w->rb_ents[t]);
    }
    m.restore_scratch += su_evlog_memory_bytes(w->rb_log);
    /* The host world struct (it contains the schedule struct and the fixed per-entry arrays). The schedule's
     * heap allocations (modules, VM contexts, operator instances and their data, the event log) are not part of the
     * world root. */
    m.metadata = (uint64_t)sizeof *w;
    m.total = m.tables_total + m.input_storage + m.queued_inputs + m.restore_scratch + m.metadata;
    /* Never report a size larger than the caller's struct: a v1 caller gets the fields it covers and its own size
     * back (host.h, SU_HOST_MEMORY_V1_SIZE), a current caller sizeof(SuHostMemory). */
    sz = out->struct_size < sizeof m ? (size_t)out->struct_size : sizeof m;
    memcpy(out, &m, sz);
    out->struct_size = (uint32_t)sz;
    return SU_OK;
}

/* ---------- execution tiers (2026-10-18) ---------- */

uint32_t su_host_native_available(void) {
#ifdef SU_HAVE_NATIVE
    return 1u;
#else
    return 0u;
#endif
}

uint32_t su_host_system_tier(const SuHostWorld *w, uint32_t index, uint32_t *tier) {
    const SuSysEntry *e;
    if (w == NULL || tier == NULL || index >= w->sched.count) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = &w->sched.sys[index];
    if (e->kind == SU_SYS_KIND_HOST) {
        *tier = SU_HOST_TIER_OPERATOR;
        return SU_OK;
    }
#ifdef SU_HAVE_NATIVE
    if (e->native_seq != 0u) {
        *tier = SU_HOST_TIER_NATIVE;
        return SU_OK;
    }
#endif
    *tier = e->vm_tier != 0u ? e->vm_tier : su_vm_default_tier();
    return SU_OK;
}

_Static_assert(SU_HOST_TIER_REFERENCE == SU_VM_TIER_REFERENCE, "host tier mirrors the VM tier");
_Static_assert(SU_HOST_TIER_PLANNED == SU_VM_TIER_PLANNED, "host tier mirrors the VM tier");

uint32_t su_host_system_set_tier(SuHostWorld *w, uint32_t index, uint32_t tier, SuHostError *err) {
    SuSysEntry *e;
    uint32_t st;
    clear_err(err);
    if (w == NULL || index >= w->sched.count) {
        return set_err(err, SU_E_INVALID_ARGUMENT, index, UINT64_MAX, "invalid world or system index");
    }
    e = &w->sched.sys[index];
    if (e->kind == SU_SYS_KIND_HOST) {
        return set_err(err, SU_E_INVALID_ARGUMENT, index, UINT64_MAX, "system %s is a host operator", e->system_id);
    }
    if (tier == SU_HOST_TIER_REFERENCE || tier == SU_HOST_TIER_PLANNED) {
        if (e->ctx != NULL) {
            st = su_vm_context_set_tier(e->ctx, tier);
            if (st != SU_OK) {
                return set_err(err, st, index, UINT64_MAX, "VM tier could not be set");
            }
        }
        e->vm_tier = tier; /* the entity runtime's per-call contexts follow on the next step (cache key) */
#ifdef SU_HAVE_NATIVE
        if (e->native_seq != 0u) {
            su_nv_release(e->native_seq);
            e->native_seq = 0u;
        }
#endif
        return SU_OK;
    }
    if (tier != SU_HOST_TIER_NATIVE) {
        return set_err(err, SU_E_INVALID_ARGUMENT, index, UINT64_MAX, "unknown tier %u", (unsigned)tier);
    }
#ifdef SU_HAVE_NATIVE
    {
        uint64_t seq = 0;
        uint64_t rows = 0;
        uint64_t bad = 0;
        uint32_t cases = 0;
        char msg[192];
        if (e->native_seq != 0u) {
            return SU_OK;
        }
        if (e->mod->uses_entity_ops) {
            return set_err(err, SU_E_CAPABILITY_DENIED, index, UINT64_MAX,
                           "system %s uses entity operations; it stays on the VM", e->system_id);
        }
        msg[0] = '\0';
        st = su_nv_prepare(e->mod, w->sched.tables[e->table].layout_generation, SU_NCAP_X86_64_SCALAR, &seq, msg,
                           sizeof msg);
        if (st != SU_OK) {
            return set_err(err, SU_E_CAPABILITY_DENIED, index, UINT64_MAX, "native backend refused %s: %s (%s)",
                           e->system_id, msg, su_status_name(st));
        }
        st = su_nv_admit(seq, 20261018u, &cases, &rows, &bad);
        if (st == SU_OK) {
            st = su_nv_install(seq);
        }
        if (st != SU_OK) {
            su_nv_release(seq);
            return set_err(err, st, index, UINT64_MAX,
                           "native kernel of %s not admitted: %llu mismatching words over %u cases", e->system_id,
                           (unsigned long long)bad, (unsigned)cases);
        }
        e->native_seq = seq;
        return SU_OK;
    }
#else
    return set_err(err, SU_E_CAPABILITY_DENIED, index, UINT64_MAX,
                   "this library was built without the native backend");
#endif
}
