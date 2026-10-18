/* In-process host API (include/sutekh/host.h): worlds driven directly through the schedule, without the
 * supervisor. The pieces are the ones the worker uses for world.create and world.step (su_lir_verify,
 * su_sched_*, su_effects_build, su_world_*, the entity runtime); this file adds memory-based loading, the
 * per-tick input records, the binary snapshot and the digest. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/host.h"
#include "host/su_host_internal.h"

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

static uint32_t string_items(const SuJsonValue *arr, const char **out, uint32_t cap) {
    uint32_t n = 0;
    size_t k;
    if (arr == NULL || arr->type != SU_JSON_ARRAY) {
        return 0;
    }
    for (k = 0; k < arr->u.array.count && n < cap; k++) {
        const SuJsonValue *e = arr->u.array.items[k];
        if (e->type == SU_JSON_STRING) {
            out[n++] = e->u.string.data;
        }
    }
    return n;
}

static uint32_t capsule_facts(const SuHostCapsule *c, CapsuleFacts *f) {
    SuJsonLimits lim;
    SuJsonError jerr;
    const SuJsonValue *schema;
    const SuJsonValue *sch;
    uint32_t st;

    memset(f, 0, sizeof *f);
    f->phase = "";
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
    f->after_count = string_items(member(sch, "after"), f->after, SU_SCHED_MAX_SYSTEMS);
    f->before_count = string_items(member(sch, "before"), f->before, SU_SCHED_MAX_SYSTEMS);
    return SU_OK;
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

/* Field descriptors of the schedule's union table. The names are borrowed from the schedule. */
static SuWorldFieldDesc *schedule_fields(const SuSchedule *s) {
    SuWorldFieldDesc *d = malloc((size_t)(s->field_count > 0u ? s->field_count : 1u) * sizeof *d);
    uint32_t i;
    if (d == NULL) {
        return NULL;
    }
    for (i = 0; i < s->field_count; i++) {
        d[i].id = s->fields[i].id;
        d[i].type = s->fields[i].type;
        d[i].access = s->fields[i].access;
        d[i].name = s->fields[i].name;
    }
    return d;
}

/* Opens the world: from the fixture (world fixture format, as world.create) or empty with capacity. */
static uint32_t open_world(SuHostWorld *hw, const SuHostWorldDesc *desc, SuHostError *err) {
    const SuSchedule *s = &hw->sched;
    SuWorldFieldDesc *fd = schedule_fields(s);
    SuWorldDesc wd;
    SuJsonDoc doc;
    SuJsonLimits lim;
    SuJsonError jerr;
    const SuJsonValue *root;
    const SuJsonValue *ids_arr;
    const SuJsonValue *cols;
    const SuJsonValue *sid;
    uint32_t *ids = NULL;
    uint32_t *vals = NULL;
    uint64_t tick = 1;
    uint64_t rows;
    uint32_t i;
    uint32_t st;

    memset(&doc, 0, sizeof doc);
    if (fd == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    memset(&wd, 0, sizeof wd);
    wd.schema_id = s->schema_id;
    wd.schema_version = s->schema_version;
    wd.layout_generation = s->layout_generation;
    wd.fields = fd;
    wd.field_count = s->field_count;
    if (desc->fixture_json == NULL) {
        if (desc->max_entities == 0u) {
            free(fd);
            return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                           "an empty world needs max_entities >= 1");
        }
        st = su_world_create_ex(&wd, desc->max_entities, &hw->w);
        free(fd);
        return st == SU_OK ? SU_OK : set_err(err, st, UINT32_MAX, UINT64_MAX, "world creation failed");
    }
    if (desc->fixture_len > HOST_FIXTURE_MAX) {
        free(fd);
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "fixture exceeds 256 MiB");
    }
    lim.max_bytes = desc->fixture_len;
    lim.max_depth = HOST_JSON_DEPTH;
    st = su_json_parse(desc->fixture_json, desc->fixture_len, &lim, &doc, &jerr);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "fixture: %s", jerr.message);
        goto out;
    }
    root = doc.root;
    sid = member(root, "schema_id");
    if (root == NULL || root->type != SU_JSON_OBJECT || !su_json_get_uint(member(root, "tick"), &tick) ||
        tick != 0u) {
        st = set_err(err, SU_E_TYPE, UINT32_MAX, UINT64_MAX, "fixture must be an object with tick 0");
        goto out;
    }
    if (sid == NULL || sid->type != SU_JSON_STRING || strcmp(sid->u.string.data, s->schema_id) != 0) {
        st = set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                     "fixture schema_id does not match the capsules' schema");
        goto out;
    }
    ids_arr = member(root, "entity_ids");
    if (ids_arr == NULL || ids_arr->type != SU_JSON_ARRAY) {
        st = set_err(err, SU_E_TYPE, UINT32_MAX, UINT64_MAX, "fixture entity_ids must be an array");
        goto out;
    }
    rows = ids_arr->u.array.count;
    if (rows > SU_LIMIT_ROWS) {
        st = set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "fixture row count exceeds the row limit");
        goto out;
    }
    st = read_u32_array(ids_arr, rows, &ids);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "fixture entity_ids must be u32 integers");
        goto out;
    }
    cols = member(root, "columns");
    if (cols == NULL || cols->type != SU_JSON_OBJECT) {
        st = set_err(err, SU_E_TYPE, UINT32_MAX, UINT64_MAX, "fixture columns must be an object");
        goto out;
    }
    wd.row_count = rows;
    wd.entity_ids = ids;
    st = su_world_create_ex(&wd, desc->max_entities, &hw->w);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "world creation failed");
        goto out;
    }
    for (i = 0; i < s->field_count; i++) {
        st = read_u32_array(member(cols, s->fields[i].name), rows, &vals);
        if (st == SU_OK) {
            st = su_world_init_committed(hw->w, s->fields[i].id, vals, rows);
        }
        free(vals);
        vals = NULL;
        if (st != SU_OK) {
            set_err(err, st, s->fields[i].id, UINT64_MAX, "fixture column %s is missing, mis-sized or not u32",
                    s->fields[i].name);
            goto out;
        }
    }
    st = SU_OK;
out:
    if (st != SU_OK) {
        su_world_destroy(hw->w);
        hw->w = NULL;
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
}

/* Capsule index i is a host operator record (lir_json NULL): find the built-in operator its system_id names,
 * check the record against it (su_host_load_bound_bytes: exact or schema-relative binding) and append the
 * host entry. */
static uint32_t add_host_record(SuHostWorld *hw, const SuHostCapsule *c, const CapsuleFacts *f, uint32_t i,
                                SuHostError *err) {
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
                       "capsule %u: no host operator for system %s is built into this library", (unsigned)i,
                       sid != NULL && sid->type == SU_JSON_STRING ? sid->u.string.data : "(none)");
    }
    msg[0] = '\0';
    st = su_host_load_bound_bytes(c->capsule_json, c->capsule_len, NULL, op->desc, &decl, &sv, &gen, bind, &bound,
                                  msg, sizeof msg);
    if (st != SU_OK) {
        return set_err(err, st, i, UINT64_MAX, "capsule %u: host record refused: %s", (unsigned)i, msg);
    }
    st = su_sched_add_host_bound(&hw->sched, op->desc, decl, bound ? bind : NULL, sv, gen, 0u, 1u, 0u);
    if (st != SU_OK) {
        su_host_decl_free(decl);
        return set_err(err, st, i, UINT64_MAX,
                       "capsule %u: host system cannot join the schedule (schema or duplicate system id)", (unsigned)i);
    }
    return SU_OK;
}

uint32_t su_host_world_create(const SuHostWorldDesc *desc, SuHostWorld **out, SuHostError *err) {
    SuHostWorld *hw = NULL;
    CapsuleFacts facts[SU_SCHED_MAX_SYSTEMS];
    uint32_t nfacts = 0;
    uint32_t i;
    uint32_t st;

    clear_err(err);
    if (out == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "out is NULL");
    }
    *out = NULL;
    if (desc == NULL || desc->struct_size != sizeof *desc || desc->capsules == NULL || desc->capsule_count == 0u ||
        desc->capsule_count > SU_HOST_MAX_CAPSULES || desc->reserved0 != 0u ||
        (desc->fixture_json == NULL && desc->fixture_len != 0u)) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX,
                       "descriptor: struct_size, capsules (1..8), reserved0 or fixture is invalid");
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
    hw->hz = desc->tick_rate_hz;
    hw->seed = desc->seed;
    hw->max_struct = desc->max_struct_per_tick != 0u ? desc->max_struct_per_tick : SU_CMDQ_DEFAULT_PER_TICK_CAP;
    hw->max_events = desc->max_events_per_tick != 0u ? desc->max_events_per_tick : SU_CMDQ_DEFAULT_PER_TICK_CAP;
    hw->log_max = desc->event_log_max != 0u ? desc->event_log_max : HOST_DEFAULT_EVENT_LOG;

    for (i = 0; i < desc->capsule_count; i++) {
        const SuHostCapsule *c = &desc->capsules[i];
        SuLirModule *m;
        SuDiag diag;
        if (c->capsule_json != NULL && c->lir_json == NULL && c->lir_len == 0u &&
            c->capsule_len <= HOST_CAPSULE_JSON_MAX) {
            st = capsule_facts(c, &facts[i]);
            nfacts = i + 1u;
            if (st != SU_OK) {
                set_err(err, st, i, UINT64_MAX, "capsule %u: schema version, layout generation or schedule block",
                        (unsigned)i);
                goto fail;
            }
            st = add_host_record(hw, c, &facts[i], i, err);
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
        st = capsule_facts(c, &facts[i]);
        nfacts = i + 1u;
        if (st != SU_OK) {
            set_err(err, st, i, UINT64_MAX, "capsule %u: schema version, layout generation or schedule block",
                    (unsigned)i);
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
        st = su_sched_add(&hw->sched, m, facts[i].schema_version, facts[i].layout_generation, 0u, 1u, 0u);
        if (st != SU_OK) {
            su_lir_module_free(m);
            free(m);
            set_err(err, st, i, UINT64_MAX, "capsule %u: cannot join the schedule (schema or duplicate system id)",
                    (unsigned)i);
            goto fail;
        }
    }
    st = su_sched_finalize(&hw->sched);
    if (st != SU_OK) {
        set_err(err, st, UINT32_MAX, UINT64_MAX, "systems disagree on a field's id, name or type");
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
    st = open_world(hw, desc, err);
    if (st != SU_OK) {
        goto fail;
    }
    st = su_ent_budget(&hw->sched, su_world_row_count(hw->w), hw->max_struct, hw->max_events);
    if (st != SU_OK) {
        set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX,
                "entity tick buffers exceed the resource budget; lower the capacity or the systems");
        goto fail;
    }
    /* Host operators get their own scratch, sized for this world's capacity (never allocated in a tick). */
    for (i = 0; i < hw->sched.count; i++) {
        const HostOperator *op;
        void *inst = NULL;
        if (hw->sched.sys[i].kind != SU_SYS_KIND_HOST) {
            continue;
        }
        op = su_host_operator_find(hw->sched.sys[i].system_id);
        st = op != NULL ? op->create(su_world_row_count(hw->w), &inst) : SU_E_INTERNAL;
        if (st == SU_OK) {
            st = su_sched_set_host_instance(&hw->sched, i, inst, op->destroy);
            if (st != SU_OK) {
                op->destroy(inst);
            }
        }
        if (st != SU_OK) {
            set_err(err, st, i, UINT64_MAX, "host operator %s: no scratch for %llu rows%s", hw->sched.sys[i].system_id,
                    (unsigned long long)su_world_row_count(hw->w),
                    st == SU_E_RESOURCE_LIMIT ? " (capacity above the operator's row limit, or out of memory)" : "");
            goto fail;
        }
    }
    compute_identity(hw);
    for (i = 0; i < nfacts; i++) {
        su_json_doc_free(&facts[i].doc);
    }
    *out = hw;
    return SU_OK;
fail:
    for (i = 0; i < nfacts; i++) {
        su_json_doc_free(&facts[i].doc);
    }
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
    su_ents_destroy(w->rb_ents);
    su_evlog_destroy(w->rb_log);
    su_world_destroy(w->w);
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
    return w != NULL ? w->sched.field_count : 0u;
}

uint32_t su_host_field_info(const SuHostWorld *w, uint32_t index, SuHostFieldInfo *out) {
    if (w == NULL || out == NULL || index >= w->sched.field_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    out->id = w->sched.fields[index].id;
    out->type = w->sched.fields[index].type;
    out->access = w->sched.fields[index].access;
    out->name = w->sched.fields[index].name;
    return SU_OK;
}

uint32_t su_host_field_id(const SuHostWorld *w, const char *name, uint32_t *id) {
    uint32_t i;
    if (w == NULL || name == NULL || id == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < w->sched.field_count; i++) {
        if (strcmp(w->sched.fields[i].name, name) == 0) {
            *id = w->sched.fields[i].id;
            return SU_OK;
        }
    }
    return SU_E_SCHEMA_MISMATCH;
}

static long field_index(const SuHostWorld *w, uint32_t id) {
    uint32_t lo = 0;
    uint32_t hi = w->sched.field_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->sched.fields[mid].id == id) {
            return (long)mid;
        }
        if (w->sched.fields[mid].id < id) {
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

uint32_t su_host_spawn(SuHostWorld *w, const SuHostValue *values, uint32_t count, uint32_t external_id,
                       uint64_t *entity, SuHostError *err) {
    SuWorldSpawnValue sv[SU_LIMIT_FIELDS];
    SuEntity h = SU_ENTITY_INVALID;
    uint32_t i;
    uint32_t st;

    clear_err(err);
    if (w == NULL || entity == NULL || (values == NULL && count > 0u) || count > SU_LIMIT_FIELDS) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    for (i = 0; i < count; i++) {
        sv[i].field_id = values[i].field_id;
        sv[i].value = values[i].value;
    }
    st = su_world_spawn(w->w, sv, count, external_id, &h);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "spawn refused; world unchanged");
    }
    st = log_record(w, SU_EVENT_SPAWNED, SU_ENTITY_SLOT(h), SU_ENTITY_SLOT(h), SU_ENTITY_GEN(h));
    if (st != SU_OK) {
        return set_err(err, SU_E_INTERNAL, UINT32_MAX, UINT64_MAX, "spawn record could not be appended");
    }
    *entity = h;
    return SU_OK;
}

uint32_t su_host_despawn(SuHostWorld *w, uint64_t entity, SuHostError *err) {
    uint32_t st;
    clear_err(err);
    if (w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "world is NULL");
    }
    st = su_world_despawn(w->w, (SuEntity)entity);
    if (st != SU_OK) {
        return set_err(err, st == SU_E_INVALID_ARGUMENT ? st : SU_E_STALE_HANDLE, UINT32_MAX, UINT64_MAX,
                       "entity handle is stale");
    }
    st = log_record(w, SU_EVENT_DESPAWNED, SU_ENTITY_SLOT(entity), SU_ENTITY_SLOT(entity), SU_ENTITY_GEN(entity));
    return st == SU_OK ? SU_OK
                       : set_err(err, SU_E_INTERNAL, UINT32_MAX, UINT64_MAX, "despawn record could not be appended");
}

uint32_t su_host_entity_at(const SuHostWorld *w, uint32_t slot, uint64_t *entity) {
    SuEntity h = SU_ENTITY_INVALID;
    const SuEnts *e;
    if (w == NULL || entity == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = su_world_entities_committed(w->w);
    if (slot >= su_ents_capacity(e) || !su_ents_slot_alive(e, slot) || su_ents_handle_for_slot(e, slot, &h) != SU_OK) {
        return SU_E_STALE_HANDLE;
    }
    *entity = h;
    return SU_OK;
}

uint32_t su_host_alive(const SuHostWorld *w, uint32_t base, uint32_t count, uint8_t *alive) {
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_world_alive_view(w->w, base, count, alive);
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
        if (field_index(w, in[i].field_id) < 0) {
            return set_err(err, SU_E_SCHEMA_MISMATCH, i, tick, "write %u names unknown field %u", (unsigned)i,
                           (unsigned)in[i].field_id);
        }
        if (in[i].entity == SU_HOST_ENTITY_NONE || SU_ENTITY_SLOT(in[i].entity) >= su_world_row_count(w->w)) {
            return set_err(err, SU_E_STALE_HANDLE, i, tick, "write %u names no slot of this world", (unsigned)i);
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
    uint32_t slot;
    uint32_t field_id;
    uint32_t old;
} UndoCell;

/* Writes cells (sorted by slot, field) into the committed root, one column copy per touched field.
 * values[i] is written to (cells[i].slot, cells[i].field_id). */
static uint32_t write_cells(SuWorld *wd, const UndoCell *cells, const uint32_t *values, uint32_t n) {
    uint64_t cap = su_world_row_count(wd);
    uint32_t *buf = NULL;
    uint32_t i;
    uint32_t st = SU_OK;
    buf = malloc((size_t)(cap > 0u ? cap : 1u) * sizeof *buf);
    if (buf == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    /* Fields in ascending order of first appearance; each field is written once. */
    for (i = 0; i < n && st == SU_OK; i++) {
        const uint32_t *col = NULL;
        uint64_t rows = 0;
        uint32_t k;
        int seen = 0;
        for (k = 0; k < i; k++) {
            if (cells[k].field_id == cells[i].field_id) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        st = su_world_read_committed(wd, cells[i].field_id, &col, &rows);
        if (st != SU_OK) {
            break;
        }
        memcpy(buf, col, (size_t)rows * sizeof *buf);
        for (k = i; k < n; k++) {
            if (cells[k].field_id == cells[i].field_id) {
                buf[cells[k].slot] = values[k];
            }
        }
        st = su_world_put_committed(wd, cells[i].field_id, buf, rows);
    }
    free(buf);
    return st;
}

/* Applies tick's record to the committed root. On SU_OK *undo (malloc'd, *undo_n cells) holds the old
 * values. A dead or stale entity is SU_E_STALE_HANDLE and nothing is written. */
static uint32_t apply_record(SuHostWorld *w, const HostRecord *r, UndoCell **undo, uint32_t *undo_n,
                             SuHostError *err) {
    const SuEnts *ents = su_world_entities_committed(w->w);
    UndoCell *cells;
    uint32_t *vals;
    uint32_t i;
    uint32_t st;

    *undo = NULL;
    *undo_n = 0;
    if (r->count == 0u) {
        return SU_OK;
    }
    cells = malloc((size_t)r->count * sizeof *cells);
    vals = malloc((size_t)r->count * sizeof *vals);
    if (cells == NULL || vals == NULL) {
        free(cells);
        free(vals);
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, r->tick, "allocation failed");
    }
    for (i = 0; i < r->count; i++) {
        const uint32_t *col = NULL;
        uint64_t rows = 0;
        if (su_ents_validate(ents, (SuEntity)r->writes[i].entity) != SU_OK) {
            free(cells);
            free(vals);
            return set_err(err, SU_E_STALE_HANDLE, UINT32_MAX, r->tick,
                           "input for tick %llu names a dead or stale entity (slot %u)", (unsigned long long)r->tick,
                           (unsigned)SU_ENTITY_SLOT(r->writes[i].entity));
        }
        (void)su_world_read_committed(w->w, r->writes[i].field_id, &col, &rows);
        cells[i].slot = SU_ENTITY_SLOT(r->writes[i].entity);
        cells[i].field_id = r->writes[i].field_id;
        cells[i].old = col != NULL ? col[cells[i].slot] : 0u;
        vals[i] = r->writes[i].value;
    }
    st = write_cells(w->w, cells, vals, r->count);
    free(vals);
    if (st != SU_OK) {
        free(cells);
        return set_err(err, st, UINT32_MAX, r->tick, "input could not be applied");
    }
    *undo = cells;
    *undo_n = r->count;
    return SU_OK;
}

static void undo_record(SuHostWorld *w, const UndoCell *cells, uint32_t n) {
    uint32_t *old = malloc((size_t)(n > 0u ? n : 1u) * sizeof *old);
    uint32_t i;
    if (old == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        old[i] = cells[i].old;
    }
    (void)write_cells(w->w, cells, old, n);
    free(old);
}

/* ---------- stepping ---------- */

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
        UndoCell *undo = NULL;
        uint32_t undo_n = 0;
        uint64_t one = 0;
        uint32_t failed = UINT32_MAX;

        /* Records for ticks already behind the world can no longer apply. */
        while (w->rec_count > 0u && w->recs[0].tick < t) {
            drop_record(w, &w->recs[0]);
            r = find_record(w, t);
        }
        if (r != NULL) {
            st = apply_record(w, r, &undo, &undo_n, err);
            if (st != SU_OK) {
                break;
            }
        }
        st = su_sched_run_ticks_opt(w->w, &w->sched, 1u, NULL, NULL, NULL, &one, &failed);
        if (st != SU_OK || one != 1u) {
            if (undo != NULL) {
                undo_record(w, undo, undo_n);
            }
            free(undo);
            if (st == SU_OK) {
                st = SU_E_INTERNAL;
            }
            set_err(err, st, failed, t, "tick %llu failed%s%s; committed state unchanged", (unsigned long long)t,
                    failed != UINT32_MAX ? " in system " : "",
                    failed != UINT32_MAX ? w->sched.sys[failed].system_id : "");
            break;
        }
        free(undo);
        if (r != NULL) {
            drop_record(w, find_record(w, t));
        }
        n++;
    }
    if (done != NULL) {
        *done = n;
    }
    return st;
}

/* ---------- reads ---------- */

uint32_t su_host_column(const SuHostWorld *w, uint32_t field_id, const uint32_t **data, uint64_t *rows) {
    if (w == NULL || data == NULL || rows == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return su_world_read_committed(w->w, field_id, data, rows);
}

uint32_t su_host_read(const SuHostWorld *w, uint32_t field_id, uint64_t first, uint64_t count, uint32_t *out) {
    const uint32_t *col = NULL;
    uint64_t rows = 0;
    uint32_t st;
    if (w == NULL || (out == NULL && count > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_world_read_committed(w->w, field_id, &col, &rows);
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

/* ---------- snapshots ---------- */

static uint32_t snapshot_parts(const SuHostWorld *w, size_t *state_len, uint64_t *log_len, size_t *total) {
    uint32_t st;
    st = su_world_canonical_size(w->w, state_len);
    if (st != SU_OK) {
        return st;
    }
    st = su_evlog_dump(su_ent_rt_log(w->sched.ent), NULL, 0u, log_len);
    if (st != SU_OK) {
        return st;
    }
    if (*log_len > (uint64_t)(SIZE_MAX - HOST_SNAP_HEADER - 8u - *state_len)) {
        return SU_E_RESOURCE_LIMIT;
    }
    *total = HOST_SNAP_HEADER + *state_len + 8u + (size_t)*log_len;
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
    put_u32(buf + 8, HOST_SNAP_VERSION);
    memcpy(buf + 12, w->identity, 32u);
    put_u64(buf + 44, su_world_tick(w->w));
    put_u64(buf + 52, (uint64_t)state_len);
    off = HOST_SNAP_HEADER;
    st = su_world_canonical_write(w->w, buf + off, state_len, &got);
    if (st != SU_OK || got != state_len) {
        return st != SU_OK ? st : SU_E_INTERNAL;
    }
    off += state_len;
    put_u64(buf + off, log_len);
    off += 8u;
    st = su_evlog_dump(su_ent_rt_log(w->sched.ent), buf + off, log_len, &log_got);
    if (st != SU_OK || log_got != log_len) {
        return st != SU_OK ? st : SU_E_INTERNAL;
    }
    return SU_OK;
}

uint32_t su_host_snapshot_restore(SuHostWorld *w, const uint8_t *buf, size_t len, SuHostError *err) {
    SuWorldFieldDesc *fd = NULL;
    SuWorldDesc wd;
    SuWorld *nw = NULL;
    SuWorld *old_w;
    SuEventLog *log = NULL;
    SuEvent *evs = NULL;
    SuEntRt *nrt = NULL;
    SuEntRt *old_rt;
    uint64_t state_len;
    uint64_t log_len;
    uint64_t n = 0;
    uint64_t got = 0;
    uint64_t ms = 0;
    uint64_t me = 0;
    uint64_t lm = 0;
    uint8_t *check = NULL;
    size_t check_len = 0;
    uint32_t st;

    clear_err(err);
    if (w == NULL || buf == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    if (len < HOST_SNAP_HEADER + 8u || memcmp(buf, HOST_SNAP_MAGIC, 8u) != 0 ||
        rd_u32(buf + 8) != HOST_SNAP_VERSION) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "not a host snapshot (magic or version)");
    }
    if (memcmp(buf + 12, w->identity, 32u) != 0) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                       "snapshot belongs to a different world identity (schema, systems, capacity or caps)");
    }
    state_len = rd_u64(buf + 52);
    if (state_len > len - HOST_SNAP_HEADER - 8u) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot state length is out of range");
    }
    log_len = rd_u64(buf + HOST_SNAP_HEADER + state_len);
    if (log_len != len - HOST_SNAP_HEADER - 8u - state_len) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot length does not match its parts");
    }
    fd = schedule_fields(&w->sched);
    if (fd == NULL) {
        return set_err(err, SU_E_RESOURCE_LIMIT, UINT32_MAX, UINT64_MAX, "allocation failed");
    }
    memset(&wd, 0, sizeof wd);
    wd.schema_id = w->sched.schema_id;
    wd.schema_version = w->sched.schema_version;
    wd.layout_generation = w->sched.layout_generation;
    wd.fields = fd;
    wd.field_count = w->sched.field_count;
    st = su_world_restore_canonical(&wd, buf + HOST_SNAP_HEADER, (size_t)state_len, &nw);
    free(fd);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "world state does not restore");
    }
    if (su_world_row_count(nw) != su_world_row_count(w->w) || su_world_tick(nw) != rd_u64(buf + 44)) {
        st = set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX, "snapshot capacity or tick disagrees");
        goto fail;
    }
    st = su_evlog_parse(buf + HOST_SNAP_HEADER + state_len + 8u, log_len, &log);
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
    /* Swap in the new state, then require the bytes to be canonical: the restored state must encode back to
     * exactly the input. Otherwise the old state is put back. */
    old_w = w->w;
    old_rt = w->sched.ent;
    w->w = nw;
    w->sched.ent = nrt;
    nw = NULL;
    nrt = NULL;
    check = malloc(len);
    if (check == NULL || su_host_snapshot_write(w, check, len, &check_len) != SU_OK || check_len != len ||
        memcmp(check, buf, len) != 0) {
        nw = w->w;
        nrt = w->sched.ent;
        w->w = old_w;
        w->sched.ent = old_rt;
        st = set_err(err, check == NULL ? SU_E_RESOURCE_LIMIT : SU_E_TYPE, UINT32_MAX, UINT64_MAX,
                     "snapshot bytes are not canonical");
        goto fail;
    }
    su_world_destroy(old_w);
    su_ent_rt_destroy(old_rt);
    st = SU_OK;
fail:
    free(check);
    free(evs);
    su_evlog_destroy(log);
    su_ent_rt_destroy(nrt);
    su_world_destroy(nw);
    return st;
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
    if (w->rb_ents == NULL) {
        SuEntsDesc d;
        memset(&d, 0, sizeof d);
        d.max_entities = su_world_row_count(w->w);
        if (su_ents_create(&d, &w->rb_ents) != SU_OK) {
            return SU_E_RESOURCE_LIMIT;
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
    uint64_t state_len;
    uint64_t log_len;
    uint32_t st;

    clear_err(err);
    if (w == NULL || buf == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, UINT64_MAX, "invalid arguments");
    }
    if (len < HOST_SNAP_HEADER + 8u || memcmp(buf, HOST_SNAP_MAGIC, 8u) != 0 ||
        rd_u32(buf + 8) != HOST_SNAP_VERSION) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "not a host snapshot (magic or version)");
    }
    if (memcmp(buf + 12, w->identity, 32u) != 0) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX,
                       "snapshot belongs to a different world identity (schema, systems, capacity or caps)");
    }
    state_len = rd_u64(buf + 52);
    if (state_len > len - HOST_SNAP_HEADER - 8u) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot state length is out of range");
    }
    log_len = rd_u64(buf + HOST_SNAP_HEADER + state_len);
    if (log_len != len - HOST_SNAP_HEADER - 8u - state_len) {
        return set_err(err, SU_E_PARSE, UINT32_MAX, UINT64_MAX, "snapshot length does not match its parts");
    }
    st = rollback_scratch(w);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "allocation of the rollback scratch failed");
    }
    /* 1. Validate the world state into the scratch entity table (the world is untouched). */
    st = su_world_restore_canonical_check(w->w, buf + HOST_SNAP_HEADER, (size_t)state_len, w->rb_ents);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX, "world state does not restore");
    }
    if (rd_u64(buf + HOST_SNAP_HEADER + 13u) != rd_u64(buf + 44)) {
        return set_err(err, SU_E_SCHEMA_MISMATCH, UINT32_MAX, UINT64_MAX, "snapshot capacity or tick disagrees");
    }
    /* 2. Event log into the scratch log (validated, then filled; the live log is untouched). */
    st = su_evlog_parse_into(w->rb_log, buf + HOST_SNAP_HEADER + state_len + 8u, log_len);
    if (st != SU_OK) {
        return set_err(err, st, UINT32_MAX, UINT64_MAX,
                       st == SU_E_SCHEMA_MISMATCH ? "event log capacity disagrees" : "event log does not parse");
    }
    /* 3. Commit: neither step can fail after the checks above. */
    su_world_restore_canonical_commit(w->w, buf + HOST_SNAP_HEADER, w->rb_ents);
    (void)su_evlog_swap(su_ent_rt_log(w->sched.ent), w->rb_log);
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

/* Value type and lane of the field at table index i. 64-bit fields are adjacent lane pairs (id, id + 1) in the
 * ascending table, so pairs are counted from the start. */
static void lane_of(const SuHostWorld *w, uint32_t i, uint32_t *vt, uint32_t *lane) {
    uint32_t k = 0;
    *vt = w->sched.fields[i].value_type == 0u ? SU_TYPE_U32 : w->sched.fields[i].value_type;
    while (k < i) {
        uint32_t t = w->sched.fields[k].value_type;
        k += (t == SU_TYPE_I64 || t == SU_TYPE_Q32) ? 2u : 1u;
    }
    *lane = k == i ? 0u : 1u;
}

/* Index of the lane-0 entry of a 64-bit (i64 or q32) field, or -1. */
static long wide_index(const SuHostWorld *w, uint32_t field_id) {
    long i = field_index(w, field_id);
    uint32_t vt, lane;
    if (i < 0) {
        return -1;
    }
    lane_of(w, (uint32_t)i, &vt, &lane);
    if ((vt != SU_TYPE_I64 && vt != SU_TYPE_Q32) || lane != 0u || (uint32_t)i + 1u >= w->sched.field_count) {
        return -2;
    }
    return i;
}

uint32_t su_host_field_value_type(const SuHostWorld *w, uint32_t field_id, uint32_t *value_type, uint32_t *lane) {
    long i;
    if (w == NULL || value_type == NULL || lane == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = field_index(w, field_id);
    if (i < 0) {
        return SU_E_SCHEMA_MISMATCH;
    }
    lane_of(w, (uint32_t)i, value_type, lane);
    return SU_OK;
}

uint32_t su_host_read_i64(const SuHostWorld *w, uint32_t field_id, uint64_t first, uint64_t count, int64_t *out) {
    const uint32_t *lo = NULL, *hi = NULL;
    uint64_t rows = 0, k;
    long i;
    uint32_t st;
    if (w == NULL || (out == NULL && count > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    i = wide_index(w, field_id);
    if (i == -1) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (i < 0) {
        return SU_E_TYPE;
    }
    st = su_world_read_committed(w->w, field_id, &lo, &rows);
    if (st == SU_OK) {
        st = su_world_read_committed(w->w, field_id + 1u, &hi, &rows);
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

uint32_t su_host_input_push_i64(SuHostWorld *w, uint64_t tick, uint64_t entity, uint32_t field_id, int64_t value,
                                SuHostError *err) {
    SuHostInput two[2];
    long i;
    clear_err(err);
    if (w == NULL) {
        return set_err(err, SU_E_INVALID_ARGUMENT, UINT32_MAX, tick, "invalid arguments");
    }
    i = wide_index(w, field_id);
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
        st = su_nv_prepare(e->mod, w->sched.layout_generation, SU_NCAP_X86_64_SCALAR, &seq, msg, sizeof msg);
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
