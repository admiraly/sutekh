#define _POSIX_C_SOURCE 200809L

#include "runtime/schedule.h"

#ifdef SU_HAVE_NATIVE
#include "runtime/native_versions.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "runtime/jobs/su_jobs.h"
#include "runtime/rts/host_decl.h"

void su_sched_init(SuSchedule *s) {
    memset(s, 0, sizeof *s);
}

void su_sched_entry_destroy(SuSysEntry *e) {
#ifdef SU_HAVE_NATIVE
    if (e->native_seq != 0u) {
        su_nv_release(e->native_seq);
    }
#endif
    su_vm_context_destroy(e->ctx);
    if (e->mod != NULL) {
        su_lir_module_free(e->mod);
        free(e->mod);
    }
    free(e->host_bind);
    free(e->target_bind);
    free(e->target_alive);
    free(e->target_gen);
    if (e->host_inst != NULL && e->host_inst_free != NULL) {
        e->host_inst_free(e->host_inst);
    }
    memset(e, 0, sizeof *e);
}

uint32_t su_sched_set_host_target(SuSchedule *s, uint32_t idx, uint32_t target_table, uint64_t own_base,
                                  uint64_t target_base, uint64_t target_rows, const uint32_t *bind) {
    SuSysEntry *e;
    uint32_t n;
    uint32_t *copy;
    if (s == NULL || idx >= s->count || s->sys[idx].kind != SU_SYS_KIND_HOST || bind == NULL ||
        target_table >= su_sched_table_count(s) || s->sys[idx].host == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = &s->sys[idx];
    n = e->host->target_field_count;
    if (n == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    copy = malloc((size_t)n * sizeof *copy);
    if (copy == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(copy, bind, (size_t)n * sizeof *copy);
    if (target_rows > 0u) {
        /* Allocated once, here (never in a tick). Fresh blocks, so a failed second call leaves the entry intact. */
        uint8_t *alive = malloc((size_t)target_rows);
        uint32_t *gen = malloc((size_t)target_rows * sizeof *gen);
        if (alive == NULL || gen == NULL) {
            free(alive);
            free(gen);
            free(copy);
            return SU_E_RESOURCE_LIMIT;
        }
        free(e->target_alive);
        free(e->target_gen);
        e->target_alive = alive;
        e->target_gen = gen;
    } else {
        free(e->target_alive);
        free(e->target_gen);
        e->target_alive = NULL;
        e->target_gen = NULL;
    }
    free(e->target_bind);
    e->target_bind = copy;
    e->target_bind_count = n;
    e->target_table = target_table;
    e->own_base = own_base;
    e->target_base = target_base;
    e->target_rows = target_rows;
    return SU_OK;
}

uint32_t su_sched_set_host_instance(SuSchedule *s, uint32_t idx, void *inst, void (*free_fn)(void *inst)) {
    SuSysEntry *e;
    if (s == NULL || idx >= s->count || inst == NULL || s->sys[idx].kind != SU_SYS_KIND_HOST) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = &s->sys[idx];
    if (e->host_inst != NULL && e->host_inst != inst && e->host_inst_free != NULL) {
        e->host_inst_free(e->host_inst);
    }
    e->host_inst = inst;
    e->host_inst_free = free_fn;
    return SU_OK;
}

uint32_t su_sched_set_host_events(SuSchedule *s, uint32_t idx, uint32_t cap, SuHostEventDrain drain) {
    SuSysEntry *e;
    if (s == NULL || idx >= s->count || cap == 0u || drain == NULL || s->sys[idx].kind != SU_SYS_KIND_HOST) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = &s->sys[idx];
    e->host_event_cap = cap;
    e->host_event_drain = drain;
    return SU_OK;
}

void su_sched_free(SuSchedule *s) {
    uint32_t i;
    for (i = 0; i < s->count; i++) {
        su_sched_entry_destroy(&s->sys[i]);
    }
    /* tables[0].fields/declared_fields are the s->fields/s->declared_fields aliases: freed once, here. */
    for (i = 0; i < SU_SCHED_MAX_TABLES; i++) {
        free(s->tables[i].fields);
        free(s->tables[i].declared_fields);
    }
    su_ent_rt_destroy(s->ent);
    memset(s, 0, sizeof *s);
}

/* Mirrors table 0 into the SuSchedule fields of the same names, so every caller written before issue 140 reads
 * table 0 exactly as before. The aliases are borrowed: the storage belongs to tables[0]. */
static void table0_sync(SuSchedule *s) {
    s->fields = s->tables[0].fields;
    s->field_count = s->tables[0].field_count;
    s->declared = s->tables[0].declared;
    s->declared_count = s->tables[0].declared_count;
    s->declared_fields = s->tables[0].declared_fields;
    snprintf(s->schema_id, sizeof s->schema_id, "%s", s->tables[0].schema_id);
    s->schema_version = s->tables[0].schema_version;
    s->layout_generation = s->tables[0].layout_generation;
}

uint32_t su_sched_table_count(const SuSchedule *s) {
    return s != NULL && s->table_count != 0u ? s->table_count : 1u;
}

const SuSchedField *su_sched_table_fields(const SuSchedule *s, uint32_t table, uint32_t *count) {
    if (s == NULL || table >= su_sched_table_count(s)) {
        if (count != NULL) {
            *count = 0u;
        }
        return NULL;
    }
    if (count != NULL) {
        *count = s->tables[table].field_count;
    }
    return s->tables[table].fields;
}

uint32_t su_sched_max_systems(const SuSchedule *s) {
    return s->max_systems != 0u ? s->max_systems : SU_SCHED_DEFAULT_SYSTEMS;
}

uint32_t su_sched_set_max_systems(SuSchedule *s, uint32_t max_systems) {
    if (s == NULL || max_systems == 0u || max_systems > SU_SCHED_MAX_SYSTEMS || max_systems < s->count) {
        return SU_E_INVALID_ARGUMENT;
    }
    s->max_systems = max_systems;
    return SU_OK;
}

static int name_ok(const char *name) {
    size_t n = 0;
    if (name == NULL) {
        return 0;
    }
    while (n <= SU_WORLD_NAME_MAX && name[n] != '\0') {
        n++;
    }
    return n >= 1u && n <= SU_WORLD_NAME_MAX;
}

uint32_t su_sched_declare_table(SuSchedule *s, uint32_t table, const char *schema_id, uint32_t schema_version,
                                uint32_t gen, const SuSchedField *lanes, uint32_t n) {
    SuSchedField *tab;
    uint32_t i, j;
    if (s == NULL || table >= SU_SCHED_MAX_TABLES || table > s->table_count || s->tables[table].declared ||
        lanes == NULL || n == 0u || n > SU_LIMIT_FIELDS || !name_ok(schema_id) || schema_version == 0u ||
        gen == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < s->count; i++) {
        if (s->sys[i].table == table) {
            return SU_E_INVALID_ARGUMENT; /* the table already has systems: its schema is fixed */
        }
    }
    for (i = 0; i < n; i++) {
        uint32_t vt = lanes[i].value_type;
        if (lanes[i].type != SU_TYPE_U32 || !name_ok(lanes[i].name) ||
            (vt != SU_TYPE_U32 && vt != SU_TYPE_I32 && vt != SU_TYPE_I64 && vt != SU_TYPE_Q32)) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (j = 0; j < i; j++) {
            if (lanes[j].id == lanes[i].id || strcmp(lanes[j].name, lanes[i].name) == 0) {
                return SU_E_INVALID_ARGUMENT;
            }
        }
    }
    tab = malloc((size_t)n * sizeof *tab);
    if (tab == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < n; i++) {
        SuSchedField key = lanes[i];
        key.access = 0u;
        j = i;
        while (j > 0 && tab[j - 1].id > key.id) {
            tab[j] = tab[j - 1];
            j--;
        }
        tab[j] = key;
    }
    snprintf(s->tables[table].schema_id, sizeof s->tables[table].schema_id, "%s", schema_id);
    s->tables[table].schema_version = schema_version;
    s->tables[table].layout_generation = gen;
    s->tables[table].declared = 1u;
    s->tables[table].declared_count = n;
    s->tables[table].declared_fields = tab;
    if (table >= s->table_count) {
        s->table_count = table + 1u;
    }
    table0_sync(s);
    return SU_OK;
}

uint32_t su_sched_declare_schema(SuSchedule *s, const char *schema_id, uint32_t schema_version, uint32_t gen,
                                 const SuSchedField *lanes, uint32_t n) {
    return su_sched_declare_table(s, 0u, schema_id, schema_version, gen, lanes, n);
}

uint32_t su_sched_enable_entities(SuSchedule *s, uint64_t max_struct_tick, uint64_t max_events_tick,
                                  uint64_t event_log_max) {
    SuEntRt *rt = NULL;
    uint32_t st;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    st = su_ent_rt_create(max_struct_tick, max_events_tick, event_log_max, &rt);
    if (st != SU_OK) {
        return st;
    }
    su_ent_rt_destroy(s->ent);
    s->ent = rt;
    return SU_OK;
}

uint32_t su_sched_adopt_entities(SuSchedule *s, SuEntRt *rt) {
    if (s == NULL || rt == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_ent_rt_destroy(s->ent);
    s->ent = rt;
    return SU_OK;
}

int su_sched_find(const SuSchedule *s, const char *system_id) {
    uint32_t i;
    for (i = 0; i < s->count; i++) {
        if (strcmp(s->sys[i].system_id, system_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void fill_entry_ids(SuSysEntry *e, uint64_t txid, uint64_t system_rev, uint64_t tick) {
    e->txid = txid;
    e->system_rev = system_rev;
    e->installed_tick = tick;
}

uint32_t su_sched_add_table(SuSchedule *s, uint32_t table, SuLirModule *mod, uint32_t schema_version, uint32_t gen,
                            uint64_t txid, uint64_t system_rev, uint64_t tick) {
    SuSysEntry *e;
    SuSchedTable *t;
    uint32_t st;

    if (mod == NULL || s == NULL || table >= SU_SCHED_MAX_TABLES) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (s->count >= su_sched_max_systems(s)) {
        return SU_E_RESOURCE_LIMIT;
    }
    t = &s->tables[table];
    if (table > s->table_count) {
        return SU_E_INVALID_ARGUMENT; /* tables are created in order: declare or add table table-1 first */
    }
    if (!t->declared && t->field_count == 0u && t->schema_id[0] == '\0') {
        snprintf(t->schema_id, sizeof t->schema_id, "%s", mod->query);
        t->schema_version = schema_version;
        t->layout_generation = gen;
        if (table >= s->table_count) {
            s->table_count = table + 1u;
        }
        table0_sync(s);
    } else if (strcmp(t->schema_id, mod->query) != 0 || t->schema_version != schema_version ||
               t->layout_generation != gen) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (su_sched_find(s, mod->system_id) >= 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = &s->sys[s->count];
    memset(e, 0, sizeof *e);
    snprintf(e->system_id, sizeof e->system_id, "%s", mod->system_id);
    snprintf(e->source_hash, sizeof e->source_hash, "%s", mod->source_hash_hex);
    snprintf(e->capsule_hash, sizeof e->capsule_hash, "%s", mod->capsule_hash_hex);
    snprintf(e->semantic_hash, sizeof e->semantic_hash, "%s", mod->semantic_hash_hex);
    e->table = table;
    e->target_table = UINT32_MAX; /* no target set unless su_sched_set_host_target names one */
    st = su_vm_context_create(mod, gen, &e->ctx);
    if (st != SU_OK) {
        memset(e, 0, sizeof *e);
        return SU_E_RESOURCE_LIMIT;
    }
    e->mod = mod;
    fill_entry_ids(e, txid, system_rev, tick);
    s->count++;
    return SU_OK;
}

uint32_t su_sched_add(SuSchedule *s, SuLirModule *mod, uint32_t schema_version, uint32_t gen,
                      uint64_t txid, uint64_t system_rev, uint64_t tick) {
    return su_sched_add_table(s, 0u, mod, schema_version, gen, txid, system_rev, tick);
}

/* Host entry (M3-04a). The declaration must describe d exactly; no VM context is created. */
static int decl_matches_host(const SuLirModule *decl, const SuHostOpDesc *d) {
    uint32_t i;
    if (decl->insn_count != 0u || decl->uses_entity_ops != 0u || decl->field_count != d->field_count ||
        strcmp(decl->system_id, d->system_id) != 0 || strcmp(decl->query, d->schema_id) != 0) {
        return 0;
    }
    for (i = 0; i < d->field_count; i++) {
        const SuLirField *f = &decl->fields[i];
        if (f->id != d->fields[i].id || f->type != SU_TYPE_U32 || SU_LIR_VALUE_TYPE(f) != d->fields[i].type ||
            f->access != d->fields[i].access || strcmp(f->name, d->fields[i].name) != 0) {
            return 0;
        }
    }
    return 1;
}

/* decl carries, for every descriptor field j, one lane with ID bind[j] and the descriptor's type and access. */
static int decl_matches_bound(const SuLirModule *decl, const SuHostOpDesc *d, const uint32_t *bind) {
    uint32_t i, j;
    if (decl->insn_count != 0u || decl->uses_entity_ops != 0u || decl->field_count != d->field_count ||
        strcmp(decl->system_id, d->system_id) != 0) {
        return 0;
    }
    for (j = 0; j < d->field_count; j++) {
        const SuLirField *f = NULL;
        for (i = 0; i < decl->field_count; i++) {
            if (decl->fields[i].id == bind[j]) {
                f = &decl->fields[i];
                break;
            }
        }
        if (f == NULL || f->type != SU_TYPE_U32 || SU_LIR_VALUE_TYPE(f) != d->fields[j].type ||
            f->access != d->fields[j].access) {
            return 0;
        }
        for (i = 0; i < j; i++) {
            if (bind[i] == bind[j]) {
                return 0;
            }
        }
    }
    return 1;
}

uint32_t su_sched_add_host_bound_table(SuSchedule *s, uint32_t table, const SuHostOpDesc *d, SuLirModule *decl,
                                       const uint32_t *bind, uint32_t schema_version, uint32_t gen, uint64_t txid,
                                       uint64_t system_rev, uint64_t tick) {
    SuSysEntry *e;
    SuSchedTable *t;
    uint32_t *copy;
    if (bind == NULL) {
        return su_sched_add_host_table(s, table, d, decl, schema_version, gen, txid, system_rev, tick);
    }
    if (s == NULL || decl == NULL || table >= SU_SCHED_MAX_TABLES || su_host_desc_validate(d) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (decl->insn_count != 0u || decl->fields == NULL || decl->system_id == NULL || decl->query == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!decl_matches_bound(decl, d, bind)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (s->count >= su_sched_max_systems(s)) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (table > s->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    t = &s->tables[table];
    if (!t->declared && t->schema_id[0] == '\0') {
        snprintf(t->schema_id, sizeof t->schema_id, "%s", decl->query);
        t->schema_version = schema_version;
        t->layout_generation = gen;
        if (table >= s->table_count) {
            s->table_count = table + 1u;
        }
        table0_sync(s);
    } else if (strcmp(t->schema_id, decl->query) != 0 || t->schema_version != schema_version ||
               t->layout_generation != gen) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (su_sched_find(s, d->system_id) >= 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    copy = malloc((size_t)d->field_count * sizeof *copy);
    if (copy == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(copy, bind, (size_t)d->field_count * sizeof *copy);
    e = &s->sys[s->count];
    memset(e, 0, sizeof *e);
    snprintf(e->system_id, sizeof e->system_id, "%s", d->system_id);
    snprintf(e->source_hash, sizeof e->source_hash, "%s", decl->source_hash_hex);
    snprintf(e->capsule_hash, sizeof e->capsule_hash, "%s", decl->capsule_hash_hex);
    snprintf(e->semantic_hash, sizeof e->semantic_hash, "%s", decl->semantic_hash_hex);
    e->kind = SU_SYS_KIND_HOST;
    e->table = table;
    e->target_table = UINT32_MAX; /* no target set unless su_sched_set_host_target names one */
    e->host = d;
    e->host_bind = copy;
    e->mod = decl;
    e->ctx = NULL;
    fill_entry_ids(e, txid, system_rev, tick);
    s->count++;
    return SU_OK;
}

uint32_t su_sched_add_host_bound(SuSchedule *s, const SuHostOpDesc *d, SuLirModule *decl, const uint32_t *bind,
                                 uint32_t schema_version, uint32_t gen, uint64_t txid, uint64_t system_rev,
                                 uint64_t tick) {
    return su_sched_add_host_bound_table(s, 0u, d, decl, bind, schema_version, gen, txid, system_rev, tick);
}

uint32_t su_sched_add_host_table(SuSchedule *s, uint32_t table, const SuHostOpDesc *d, SuLirModule *decl,
                                 uint32_t schema_version, uint32_t gen, uint64_t txid, uint64_t system_rev,
                                 uint64_t tick) {
    SuSysEntry *e;
    SuSchedTable *t;
    if (s == NULL || decl == NULL || table >= SU_SCHED_MAX_TABLES || su_host_desc_validate(d) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (decl->insn_count != 0u || decl->fields == NULL || decl->system_id == NULL || decl->query == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!decl_matches_host(decl, d)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (s->count >= su_sched_max_systems(s)) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (table > s->table_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    t = &s->tables[table];
    if (!t->declared && t->schema_id[0] == '\0') {
        snprintf(t->schema_id, sizeof t->schema_id, "%s", d->schema_id);
        t->schema_version = schema_version;
        t->layout_generation = gen;
        if (table >= s->table_count) {
            s->table_count = table + 1u;
        }
        table0_sync(s);
    } else if (strcmp(t->schema_id, d->schema_id) != 0 || t->schema_version != schema_version ||
               t->layout_generation != gen) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (su_sched_find(s, d->system_id) >= 0) {
        return SU_E_INVALID_ARGUMENT;
    }
    e = &s->sys[s->count];
    memset(e, 0, sizeof *e);
    snprintf(e->system_id, sizeof e->system_id, "%s", d->system_id);
    snprintf(e->source_hash, sizeof e->source_hash, "%s", decl->source_hash_hex);
    snprintf(e->capsule_hash, sizeof e->capsule_hash, "%s", decl->capsule_hash_hex);
    snprintf(e->semantic_hash, sizeof e->semantic_hash, "%s", decl->semantic_hash_hex);
    e->kind = SU_SYS_KIND_HOST;
    e->table = table;
    e->target_table = UINT32_MAX; /* no target set unless su_sched_set_host_target names one */
    e->host = d;
    e->mod = decl;
    e->ctx = NULL;
    fill_entry_ids(e, txid, system_rev, tick);
    s->count++;
    return SU_OK;
}

uint32_t su_sched_add_host(SuSchedule *s, const SuHostOpDesc *d, SuLirModule *decl, uint32_t schema_version,
                           uint32_t gen, uint64_t txid, uint64_t system_rev, uint64_t tick) {
    return su_sched_add_host_table(s, 0u, d, decl, schema_version, gen, txid, system_rev, tick);
}

/* Adds one field to the union table, or checks it against an existing row. */
static uint32_t union_add(SuSchedField **tab, uint32_t *n, uint32_t *cap, const SuLirField *f, int closed) {
    uint32_t i;
    for (i = 0; i < *n; i++) {
        SuSchedField *x = &(*tab)[i];
        if (x->id == f->id) {
            if (x->type != f->type || x->value_type != SU_LIR_VALUE_TYPE(f) || strcmp(x->name, f->name) != 0) {
                return SU_E_SCHEMA_MISMATCH;
            }
            x->access |= f->access;
            return SU_OK;
        }
        if (strcmp(x->name, f->name) == 0) {
            return SU_E_SCHEMA_MISMATCH;
        }
    }
    if (closed) {
        return SU_E_SCHEMA_MISMATCH; /* a declared schema: systems bind a subset of its lanes */
    }
    if (*n >= *cap) {
        uint32_t nc = *cap == 0 ? 8u : *cap * 2u;
        SuSchedField *nt;
        if (nc > SU_LIMIT_FIELDS) {
            nc = SU_LIMIT_FIELDS;
        }
        if (*n >= nc) {
            return SU_E_RESOURCE_LIMIT;
        }
        nt = realloc(*tab, (size_t)nc * sizeof *nt);
        if (nt == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        *tab = nt;
        *cap = nc;
    }
    memset(&(*tab)[*n], 0, sizeof(SuSchedField));
    (*tab)[*n].id = f->id;
    (*tab)[*n].type = f->type;
    (*tab)[*n].value_type = SU_LIR_VALUE_TYPE(f);
    (*tab)[*n].access = f->access;
    snprintf((*tab)[*n].name, sizeof (*tab)[*n].name, "%s", f->name);
    (*n)++;
    return SU_OK;
}

static const char *vtype_name(uint32_t t) {
    return t == SU_TYPE_Q32 ? "q32" : t == SU_TYPE_I64 ? "i64" : t == SU_TYPE_I32 ? "i32" : "u32";
}

/* 1 when name ends in ".lo" or ".hi" (a lane of a 64-bit field). */
static int lane_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 3u && (strcmp(name + n - 3u, ".lo") == 0 || strcmp(name + n - 3u, ".hi") == 0);
}

/* A declared schema: the first system field of table t that is not a declared lane, or disagrees with one. */
static int explain_declared(const SuSchedule *s, uint32_t table, char *msg, size_t cap, uint32_t *system) {
    const SuSchedTable *t = &s->tables[table];
    uint32_t a, i, k;
    for (a = 0; a < s->count; a++) {
        const SuLirModule *m = s->sys[a].mod;
        if (s->sys[a].table != table) {
            continue;
        }
        for (i = 0; i < m->field_count; i++) {
            const SuLirField *f = &m->fields[i];
            const SuSchedField *by_id = NULL, *by_name = NULL;
            for (k = 0; k < t->declared_count; k++) {
                if (t->declared_fields[k].id == f->id) {
                    by_id = &t->declared_fields[k];
                }
                if (strcmp(t->declared_fields[k].name, f->name) == 0) {
                    by_name = &t->declared_fields[k];
                }
            }
            if (by_id != NULL && by_id == by_name && by_id->type == f->type &&
                by_id->value_type == SU_LIR_VALUE_TYPE(f)) {
                continue;
            }
            *system = a;
            if (by_id == NULL && by_name == NULL) {
                snprintf(msg, cap, "%s binds field id %u \"%s\" (%s), which the declared world schema does not have",
                         s->sys[a].system_id, (unsigned)f->id, f->name, vtype_name(SU_LIR_VALUE_TYPE(f)));
            } else {
                const SuSchedField *d = by_id != NULL ? by_id : by_name;
                snprintf(msg, cap,
                         "field mismatch: the declared world schema has id %u \"%s\" (%s), %s has id %u \"%s\" (%s)%s",
                         (unsigned)d->id, d->name, vtype_name(d->value_type), s->sys[a].system_id, (unsigned)f->id,
                         f->name, vtype_name(SU_LIR_VALUE_TYPE(f)),
                         lane_suffix(d->name) || lane_suffix(f->name)
                             ? "; a q32/i64 field \"x\" at ID J is lanes \"x.lo\" (J), \"x.hi\" (J+1)"
                             : "");
            }
            return 1;
        }
    }
    return 0;
}

void su_sched_explain_mismatch(const SuSchedule *s, char *msg, size_t cap) {
    uint32_t system;
    su_sched_explain_mismatch_ex(s, msg, cap, &system);
}

void su_sched_explain_mismatch_ex(const SuSchedule *s, char *msg, size_t cap, uint32_t *system) {
    uint32_t table, a, b, i, j;
    *system = UINT32_MAX;
    if (msg == NULL || cap == 0u) {
        return;
    }
    msg[0] = '\0';
    for (table = 0; table < su_sched_table_count(s); table++) {
        if (s->tables[table].declared && explain_declared(s, table, msg, cap, system)) {
            return;
        }
    }
    /* A derived table: two of its systems disagree on a field. Systems of different tables never share columns,
     * so only pairs within one table are compared (for a single-table world that is every pair, as before). */
    for (table = 0; table < su_sched_table_count(s); table++) {
        for (a = 0; a < s->count; a++) {
            const SuLirModule *ma = s->sys[a].mod;
            if (s->sys[a].table != table) {
                continue;
            }
            for (b = a; b < s->count; b++) {
                const SuLirModule *mb = s->sys[b].mod;
                if (s->sys[b].table != table) {
                    continue;
                }
                for (i = 0; i < ma->field_count; i++) {
                    const SuLirField *fa = &ma->fields[i];
                    for (j = (a == b ? i + 1u : 0u); j < mb->field_count; j++) {
                        const SuLirField *fb = &mb->fields[j];
                        int same_id = fa->id == fb->id, same_name = strcmp(fa->name, fb->name) == 0;
                        const char *hint = "";
                        if (same_id == same_name && (!same_id || (fa->type == fb->type &&
                                                                  SU_LIR_VALUE_TYPE(fa) == SU_LIR_VALUE_TYPE(fb)))) {
                            continue;
                        }
                        if (!same_name && (lane_suffix(fa->name) || lane_suffix(fb->name))) {
                            hint = "; a q32/i64 IR field \"x\" at ID J binds lanes \"x.lo\" (J), \"x.hi\" (J+1)";
                        } else if (same_id && same_name) {
                            hint = "; every system binding a lane must agree on its value type";
                        }
                        snprintf(msg, cap,
                                 "field mismatch: %s has id %u \"%s\" (%s), %s has id %u \"%s\" (%s)%s",
                                 s->sys[a].system_id, (unsigned)fa->id, fa->name, vtype_name(SU_LIR_VALUE_TYPE(fa)),
                                 s->sys[b].system_id, (unsigned)fb->id, fb->name, vtype_name(SU_LIR_VALUE_TYPE(fb)),
                                 hint);
                        return;
                    }
                }
            }
        }
    }
}

/* Builds table t's union field table from its declared lanes and its systems' fields. */
static uint32_t finalize_table(SuSchedule *s, uint32_t table) {
    SuSchedTable *t = &s->tables[table];
    SuSchedField *tab = NULL;
    uint32_t n = 0;
    uint32_t cap = 0;
    uint32_t i;
    uint32_t j;
    uint32_t st = SU_OK;

    if (t->declared) {
        tab = malloc((size_t)t->declared_count * sizeof *tab);
        if (tab == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        memcpy(tab, t->declared_fields, (size_t)t->declared_count * sizeof *tab);
        n = cap = t->declared_count;
    }
    for (i = 0; i < s->count && st == SU_OK; i++) {
        const SuLirModule *m;
        if (s->sys[i].table != table) {
            continue;
        }
        m = s->sys[i].mod;
        for (j = 0; j < m->field_count && st == SU_OK; j++) {
            st = union_add(&tab, &n, &cap, &m->fields[j], t->declared != 0u);
        }
    }
    if (st != SU_OK) {
        free(tab);
        return st;
    }
    /* Keep the table ascending by ID (the VM and world rely on sorted fields). */
    for (i = 1; i < n; i++) {
        SuSchedField key = tab[i];
        j = i;
        while (j > 0 && tab[j - 1].id > key.id) {
            tab[j] = tab[j - 1];
            j--;
        }
        tab[j] = key;
    }
    free(t->fields);
    t->fields = tab;
    t->field_count = n;
    return SU_OK;
}

uint32_t su_sched_finalize(SuSchedule *s) {
    uint32_t table;
    uint32_t st;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (s->table_count == 0u) {
        s->table_count = 1u;
    }
    for (table = 0; table < s->table_count; table++) {
        st = finalize_table(s, table);
        if (st != SU_OK) {
            return st;
        }
    }
    table0_sync(s);
    return SU_OK;
}

uint32_t su_sched_check_candidate(const SuSchedule *s, const SuLirModule *m, uint32_t schema_version,
                                  uint32_t gen, char *why, size_t cap) {
    uint32_t i;
    uint32_t table = 0u;
    const SuSchedField *fields;
    uint32_t field_count;
    int idx = su_sched_find(s, m->system_id);
    if (idx >= 0 && s->sys[idx].kind == SU_SYS_KIND_HOST) {
        snprintf(why, cap, "system %s is a host entry and cannot be replaced by a candidate", m->system_id);
        return SU_E_CAPABILITY_DENIED;
    }
    if (idx >= 0) {
        table = s->sys[idx].table;
    }
    if (table >= su_sched_table_count(s)) {
        snprintf(why, cap, "candidate's table %u does not exist", (unsigned)table);
        return SU_E_SCHEMA_MISMATCH;
    }
    fields = su_sched_table_fields(s, table, &field_count);
    if (strcmp(s->tables[table].schema_id, m->query) != 0) {
        snprintf(why, cap, "candidate query %s does not match world schema %s", m->query, s->tables[table].schema_id);
        return SU_E_SCHEMA_MISMATCH;
    }
    if (schema_version != s->tables[table].schema_version) {
        snprintf(why, cap, "candidate schema version %u does not match world %u", (unsigned)schema_version,
                 (unsigned)s->tables[table].schema_version);
        return SU_E_SCHEMA_MISMATCH;
    }
    if (gen != s->tables[table].layout_generation) {
        snprintf(why, cap, "candidate layout_generation %u does not match world %u", (unsigned)gen,
                 (unsigned)s->tables[table].layout_generation);
        return SU_E_SCHEMA_MISMATCH;
    }
    for (i = 0; i < m->field_count; i++) {
        const SuLirField *f = &m->fields[i];
        uint32_t k;
        int found = 0;
        for (k = 0; k < field_count; k++) {
            const SuSchedField *w = &fields[k];
            if (w->id != f->id) {
                continue;
            }
            found = 1;
            if (w->type != f->type || w->value_type != SU_LIR_VALUE_TYPE(f) || strcmp(w->name, f->name) != 0) {
                snprintf(why, cap, "field %u is not the same field as in the world", (unsigned)f->id);
                return SU_E_SCHEMA_MISMATCH;
            }
            if ((f->access & ~w->access) != 0u) {
                snprintf(why, cap, "field %s needs access the world layout does not provide", f->name);
                return SU_E_SCHEMA_MISMATCH;
            }
        }
        if (!found) {
            snprintf(why, cap, "field %s is not part of the world layout", f->name);
            return SU_E_SCHEMA_MISMATCH;
        }
    }
    return SU_OK;
}

/* Host entry (M3-04a, section 3.1). The operator sees only its declared reads and writes: the bindings of
 * other fields are dropped from the views it is given. Staging of a declared write is committed for whole
 * columns, so the operator must store every row of each declared write (copy working to staging first).
 * A failed run commits nothing. ev (optional) receives the events the run appended (issue 172).
 * A cross-table entry (su_sched_set_host_target) also receives its descriptor's target lanes from the target
 * table's working columns and that table's alive/generation views, filled here for this run (issue 140). */
static uint32_t run_host(SuSchedule *s, SuWorld *w, const SuSysEntry *e, SuHostOpEvents *ev) {
    const SuHostOpDesc *d = e->host;
    SuInvocation inv;
    SuOutputs out;
    SuColumnBinding rb[SU_LIMIT_FIELDS];
    SuColumnBinding wb[SU_LIMIT_FIELDS];
    SuHostTargetColumn tcol[SU_LIMIT_FIELDS];
    uint32_t wid[SU_LIMIT_FIELDS];
    uint32_t nr = 0;
    uint32_t nw = 0;
    uint32_t nt = 0;
    uint32_t i;
    uint32_t st;
    SuInvocation full_inv;
    SuOutputs full_out;
    SuInvocation tgt_inv;
    uint32_t cur_in = 0;
    uint32_t cur_out = 0;
    uint32_t cur_tgt = 0;
    void *inst = e->host_inst != NULL ? e->host_inst : d->host_ctx;

    if (ev != NULL) {
        ev->events = NULL;
        ev->count = 0u;
    }

    st = su_world_build_invocation(w, &full_inv);
    if (st != SU_OK) {
        return st;
    }
    st = su_world_build_outputs(w, &full_out);
    if (st != SU_OK) {
        return st;
    }
    if (e->target_table != UINT32_MAX) {
        SuWorld *tw = NULL;
        if (s->tick_worlds == NULL || e->target_table >= s->tick_nw ||
            (tw = s->tick_worlds[e->target_table]) == NULL) {
            return SU_E_INTERNAL; /* a target-bound entry runs only inside a table tick */
        }
        st = su_world_build_read_invocation(tw, &tgt_inv);
        if (st != SU_OK) {
            return st;
        }
        if (e->target_rows > 0u) {
            st = su_world_alive_view(tw, 0u, (uint32_t)e->target_rows, e->target_alive);
            if (st == SU_OK) {
                st = su_world_generation_view(tw, 0u, (uint32_t)e->target_rows, e->target_gen);
            }
            if (st != SU_OK) {
                return st;
            }
        }
        for (i = 0; i < d->target_field_count; i++) {
            const SuHostField *f = &d->target_fields[i];
            const SuColumnBinding *b =
                su_world_find_binding_from(tgt_inv.bindings, tgt_inv.binding_count, e->target_bind[i], &cur_tgt);
            if (b == NULL) {
                return SU_E_SCHEMA_MISMATCH;
            }
            tcol[nt].field_id = f->id;
            tcol[nt].in = (const uint32_t *)b->data;
            nt++;
        }
    }
    for (i = 0; i < d->field_count; i++) {
        const SuHostField *f = &d->fields[i];
        /* Schema-relative binding: the world column of descriptor field i is e->host_bind[i]; the operator sees it
         * under the descriptor's ID f->id (it looks columns up by its own IDs). */
        const uint32_t world_id = e->host_bind != NULL ? e->host_bind[i] : f->id;
        const SuColumnBinding *rbind = su_world_find_binding_from(full_inv.bindings, full_inv.binding_count, world_id, &cur_in);
        if ((f->access & SU_ACCESS_READ) != 0u && rbind != NULL) {
            rb[nr] = *rbind;
            rb[nr++].field_id = f->id;
        }
        if ((f->access & SU_ACCESS_WRITE) != 0u) {
            const SuColumnBinding *wbind = su_world_find_binding_from(full_out.bindings, full_out.binding_count, world_id, &cur_out);
            if (wbind != NULL) {
                /* r4 (A4): staging is committed whole, so every row starts from its working value. Done
                 * when the working column is visible (declared read); a write-only field has no working
                 * view, so its operator stores every row itself (M3-04a deviation, see the handoff). */
                if ((f->access & SU_ACCESS_READ) != 0u && rbind != NULL) {
                    memcpy(wbind->data, rbind->data, (size_t)full_inv.row_count * sizeof(uint32_t));
                }
                wb[nw] = *wbind;
                wb[nw].field_id = f->id;
                wid[nw++] = world_id;
            }
        }
    }
    inv = full_inv;
    inv.bindings = rb;
    inv.binding_count = nr;
    out = full_out;
    out.bindings = wb;
    out.binding_count = nw;
    if (e->target_table != UINT32_MAX) {
        SuHostOpTargets t;
        if (d->run_targets == NULL || d->target_field_count == 0u) {
            return SU_E_CAPABILITY_DENIED;
        }
        t.table = e->target_table;
        t.rows = e->target_rows;
        t.base = e->target_base;
        t.own_base = e->own_base;
        t.column_count = nt;
        t.columns = tcol;
        t.alive = e->target_alive;
        t.generation = e->target_gen;
        st = d->run_targets(w, &inv, &out, &t, inst);
    } else {
        st = d->run(w, &inv, &out, inst);
    }
    if (st != SU_OK) {
        return st;
    }
    st = su_world_commit_staging(w, wid, nw);
    if (st != SU_OK) {
        return st;
    }
    if (ev != NULL && e->host_event_drain != NULL) {
        ev->events = e->host_event_drain(inst, &ev->count);
    }
    return SU_OK;
}

static uint32_t run_one_system(SuWorld *w, SuSchedule *s, uint32_t i, const SuSchedTickOpts *opt,
                               SuHostOpEvents *ev) {
    if (s->sys[i].kind == SU_SYS_KIND_HOST) {
        return run_host(s, w, &s->sys[i], ev);
    }
    /* The entry's own table's layout generation (issue 140): a table's systems are verified against that table's
     * schema, whose generation may differ from table 0's. */
    {
        uint32_t gen = s->tables[s->sys[i].table].layout_generation;
#ifdef SU_HAVE_NATIVE
        if (s->sys[i].native_seq != 0u) {
            if (opt != NULL && opt->workers > 1u && su_world_row_count(w) >= opt->chunk_min_rows) {
                return su_nv_run_system_chunked(s->sys[i].native_seq, w, opt->workers, opt->chunk_fault,
                                                opt->chunk_ctx);
            }
            return su_nv_run_system(s->sys[i].native_seq, w);
        }
#endif
        if (opt != NULL && opt->workers > 1u && su_world_row_count(w) >= opt->chunk_min_rows) {
            SuJobsOptions jo;
            SuJobsReport rep;
            uint32_t st;
            memset(&jo, 0, sizeof jo);
            jo.workers = opt->workers;
            jo.fault = opt->chunk_fault;
            jo.fault_ctx = opt->chunk_ctx;
            st = su_jobs_run_system_chunked(w, s->sys[i].mod, gen, &jo, &rep);
            su_jobs_report_free(&rep);
            return st;
        }
    }
    return su_vm_run_system(w, s->sys[i].ctx);
}

uint32_t su_sched_run_system(SuWorld *w, SuSchedule *s, uint32_t i, const SuSchedTickOpts *opt) {
    if (w == NULL || s == NULL || i >= s->count) {
        return SU_E_INVALID_ARGUMENT;
    }
    return run_one_system(w, s, i, opt, NULL);
}

uint32_t su_sched_run_system_events(SuWorld *w, SuSchedule *s, uint32_t i, const SuSchedTickOpts *opt,
                                    SuHostOpEvents *ev) {
    if (ev != NULL) {
        ev->events = NULL;
        ev->count = 0u;
    }
    if (w == NULL || s == NULL || i >= s->count) {
        return SU_E_INVALID_ARGUMENT;
    }
    return run_one_system(w, s, i, opt, ev);
}

uint32_t su_sched_run_ticks_opt(SuWorld *w, SuSchedule *s, uint64_t ticks, SuSchedFault fault,
                                void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                                uint32_t *failed_index) {
    uint64_t t;
    uint32_t k;
    int has_entity_ops = 0;
    *done = 0;
    *failed_index = UINT32_MAX;
    for (k = 0; k < s->count; k++) {
        /* A host operator that appends events needs the entity tick too (issue 172): its records join the tick's
         * event buffers and are committed to the log with the IR systems' events. */
        if (s->sys[k].mod->uses_entity_ops || s->sys[k].host_event_cap != 0u) {
            has_entity_ops = 1;
        }
    }
    if (has_entity_ops) {
        if (s->ent == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
        return su_ent_run_ticks(w, s, s->ent, ticks, fault, fault_ctx, opt, done, failed_index);
    }
    for (t = 0; t < ticks; t++) {
        uint32_t st = su_world_begin_tick(w);
        uint32_t i;
        if (st != SU_OK) {
            return st;
        }
        for (i = 0; i < s->count; i++) {
            if (fault != NULL && fault(i, fault_ctx) != 0) {
                st = SU_E_CANDIDATE_FAILED;
            } else {
                st = run_one_system(w, s, i, opt, NULL);
            }
            if (st == SU_OK) {
                /* Dead rows are canonically zero after every system's commit (contract B1). */
                st = su_world_zero_dead(w);
            }
            if (st != SU_OK) {
                break;
            }
        }
        if (st != SU_OK) {
            su_world_abort(w);
            *failed_index = i;
            return st;
        }
        st = su_world_publish(w);
        if (st != SU_OK) {
            su_world_abort(w);
            return st;
        }
        *done = t + 1u;
    }
    return SU_OK;
}

uint32_t su_sched_run_ticks(SuWorld *w, SuSchedule *s, uint64_t ticks, SuSchedFault fault,
                            void *fault_ctx, uint64_t *done, uint32_t *failed_index) {
    return su_sched_run_ticks_opt(w, s, ticks, fault, fault_ctx, NULL, done, failed_index);
}

uint32_t su_sched_run_ticks_tables(SuWorld *const *ws, uint32_t nw, SuSchedule *s, uint64_t ticks, SuSchedFault fault,
                                   void *fault_ctx, const SuSchedTickOpts *opt, uint64_t *done,
                                   uint32_t *failed_index) {
    int begun[SU_SCHED_MAX_TABLES];
    int has[SU_SCHED_MAX_TABLES];
    uint64_t t;
    uint32_t i;
    uint32_t st = SU_OK;

    if (done == NULL || failed_index == NULL || ws == NULL || s == NULL || nw == 0u || nw > SU_SCHED_MAX_TABLES ||
        nw < su_sched_table_count(s)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (nw == 1u) {
        return su_sched_run_ticks_opt(ws[0], s, ticks, fault, fault_ctx, opt, done, failed_index);
    }
    for (i = 0; i < s->count; i++) {
        if (s->sys[i].table >= nw) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    /* A system with entity operations, or a host operator that appends events, runs through the table-aware entity
     * tick (issue 140): one chunk cache, entity view and command/event queue per table, one log for the world. */
    for (i = 0; i < s->count; i++) {
        if (s->sys[i].mod->uses_entity_ops || s->sys[i].host_event_cap != 0u) {
            if (s->ent == NULL) {
                return SU_E_INVALID_ARGUMENT;
            }
            return su_ent_run_ticks_tables(ws, nw, s, s->ent, ticks, fault, fault_ctx, opt, done, failed_index);
        }
    }
    *done = 0u;
    *failed_index = UINT32_MAX;
    /* The worlds of the tick, so a host entry bound to a target table can read it (issue 140). */
    s->tick_worlds = ws;
    s->tick_nw = nw;
    memset(has, 0, sizeof has);
    for (i = 0; i < s->count; i++) {
        has[s->sys[i].table] = 1;
    }
    for (t = 0; t < ticks; t++) {
        uint32_t j;
        memset(begun, 0, sizeof begun);
        for (j = 0; j < nw; j++) {
            if (has[j]) {
                st = su_world_begin_tick(ws[j]);
                if (st != SU_OK) {
                    *failed_index = UINT32_MAX;
                    goto fail;
                }
                begun[j] = 1;
            }
        }
        for (i = 0; i < s->count; i++) {
            uint32_t table = s->sys[i].table;
            if (fault != NULL && fault(i, fault_ctx) != 0) {
                st = SU_E_CANDIDATE_FAILED;
            } else {
                st = run_one_system(ws[table], s, i, opt, NULL);
            }
            if (st == SU_OK) {
                /* Dead rows are canonically zero after every system's commit (contract B1), per table. */
                st = su_world_zero_dead(ws[table]);
            }
            if (st != SU_OK) {
                *failed_index = i;
                goto fail;
            }
        }
        for (j = 0; j < nw; j++) {
            if (begun[j]) {
                st = su_world_publish(ws[j]);
                if (st != SU_OK) {
                    *failed_index = UINT32_MAX;
                    goto fail;
                }
                begun[j] = 0;
            }
        }
        /* A table with no scheduled system was not begun or published; it keeps the world's one tick (issue 140). */
        if (s->count > 0u) {
            uint64_t nt = su_world_tick(ws[s->sys[0].table]);
            for (j = 0; j < nw; j++) {
                if (!has[j]) {
                    (void)su_world_set_tick(ws[j], nt);
                }
            }
        }
        *done = t + 1u;
        continue;
    fail:
        for (j = 0; j < nw; j++) {
            if (begun[j]) {
                (void)su_world_abort(ws[j]);
            }
        }
        s->tick_worlds = NULL;
        s->tick_nw = 0u;
        return st;
    }
    s->tick_worlds = NULL;
    s->tick_nw = 0u;
    return SU_OK;
}

void su_sched_swap(SuSchedule *s, uint32_t idx, SuSysEntry *fresh, SuSysEntry *old) {
    *old = s->sys[idx];
    s->sys[idx] = *fresh;
    memset(fresh, 0, sizeof *fresh);
}
