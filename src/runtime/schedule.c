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
    if (e->host_inst != NULL && e->host_inst_free != NULL) {
        e->host_inst_free(e->host_inst);
    }
    memset(e, 0, sizeof *e);
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

void su_sched_free(SuSchedule *s) {
    uint32_t i;
    for (i = 0; i < s->count; i++) {
        su_sched_entry_destroy(&s->sys[i]);
    }
    free(s->fields);
    su_ent_rt_destroy(s->ent);
    memset(s, 0, sizeof *s);
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

uint32_t su_sched_add(SuSchedule *s, SuLirModule *mod, uint32_t schema_version, uint32_t gen,
                      uint64_t txid, uint64_t system_rev, uint64_t tick) {
    SuSysEntry *e;
    uint32_t st;

    if (mod == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (s->count >= SU_SCHED_MAX_SYSTEMS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (s->count == 0) {
        snprintf(s->schema_id, sizeof s->schema_id, "%s", mod->query);
        s->schema_version = schema_version;
        s->layout_generation = gen;
    } else if (strcmp(s->schema_id, mod->query) != 0 || s->schema_version != schema_version ||
               s->layout_generation != gen) {
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

uint32_t su_sched_add_host_bound(SuSchedule *s, const SuHostOpDesc *d, SuLirModule *decl, const uint32_t *bind,
                                 uint32_t schema_version, uint32_t gen, uint64_t txid, uint64_t system_rev,
                                 uint64_t tick) {
    SuSysEntry *e;
    uint32_t *copy;
    if (bind == NULL) {
        return su_sched_add_host(s, d, decl, schema_version, gen, txid, system_rev, tick);
    }
    if (s == NULL || decl == NULL || su_host_desc_validate(d) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (decl->insn_count != 0u || decl->fields == NULL || decl->system_id == NULL || decl->query == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!decl_matches_bound(decl, d, bind)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (s->count >= SU_SCHED_MAX_SYSTEMS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (s->count == 0) {
        snprintf(s->schema_id, sizeof s->schema_id, "%s", decl->query);
        s->schema_version = schema_version;
        s->layout_generation = gen;
    } else if (strcmp(s->schema_id, decl->query) != 0 || s->schema_version != schema_version ||
               s->layout_generation != gen) {
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
    e->host = d;
    e->host_bind = copy;
    e->mod = decl;
    e->ctx = NULL;
    fill_entry_ids(e, txid, system_rev, tick);
    s->count++;
    return SU_OK;
}

uint32_t su_sched_add_host(SuSchedule *s, const SuHostOpDesc *d, SuLirModule *decl, uint32_t schema_version,
                           uint32_t gen, uint64_t txid, uint64_t system_rev, uint64_t tick) {
    SuSysEntry *e;
    if (s == NULL || decl == NULL || su_host_desc_validate(d) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (decl->insn_count != 0u || decl->fields == NULL || decl->system_id == NULL || decl->query == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!decl_matches_host(decl, d)) {
        return SU_E_SCHEMA_MISMATCH;
    }
    if (s->count >= SU_SCHED_MAX_SYSTEMS) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (s->count == 0) {
        snprintf(s->schema_id, sizeof s->schema_id, "%s", d->schema_id);
        s->schema_version = schema_version;
        s->layout_generation = gen;
    } else if (strcmp(s->schema_id, d->schema_id) != 0 || s->schema_version != schema_version ||
               s->layout_generation != gen) {
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
    e->host = d;
    e->mod = decl;
    e->ctx = NULL;
    fill_entry_ids(e, txid, system_rev, tick);
    s->count++;
    return SU_OK;
}

/* Adds one field to the union table, or checks it against an existing row. */
static uint32_t union_add(SuSchedField **tab, uint32_t *n, uint32_t *cap, const SuLirField *f) {
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

void su_sched_explain_mismatch(const SuSchedule *s, char *msg, size_t cap) {
    uint32_t a, b, i, j;
    if (msg == NULL || cap == 0u) {
        return;
    }
    msg[0] = '\0';
    for (a = 0; a < s->count; a++) {
        const SuLirModule *ma = s->sys[a].mod;
        for (b = a; b < s->count; b++) {
            const SuLirModule *mb = s->sys[b].mod;
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
                             s->sys[b].system_id, (unsigned)fb->id, fb->name, vtype_name(SU_LIR_VALUE_TYPE(fb)), hint);
                    return;
                }
            }
        }
    }
}

uint32_t su_sched_finalize(SuSchedule *s) {
    SuSchedField *tab = NULL;
    uint32_t n = 0;
    uint32_t cap = 0;
    uint32_t i;
    uint32_t j;
    uint32_t st = SU_OK;

    for (i = 0; i < s->count && st == SU_OK; i++) {
        const SuLirModule *m = s->sys[i].mod;
        for (j = 0; j < m->field_count && st == SU_OK; j++) {
            st = union_add(&tab, &n, &cap, &m->fields[j]);
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
    free(s->fields);
    s->fields = tab;
    s->field_count = n;
    return SU_OK;
}

uint32_t su_sched_check_candidate(const SuSchedule *s, const SuLirModule *m, uint32_t schema_version,
                                  uint32_t gen, char *why, size_t cap) {
    uint32_t i;
    int idx = su_sched_find(s, m->system_id);
    if (idx >= 0 && s->sys[idx].kind == SU_SYS_KIND_HOST) {
        snprintf(why, cap, "system %s is a host entry and cannot be replaced by a candidate", m->system_id);
        return SU_E_CAPABILITY_DENIED;
    }
    if (strcmp(s->schema_id, m->query) != 0) {
        snprintf(why, cap, "candidate query %s does not match world schema %s", m->query, s->schema_id);
        return SU_E_SCHEMA_MISMATCH;
    }
    if (schema_version != s->schema_version) {
        snprintf(why, cap, "candidate schema version %u does not match world %u", (unsigned)schema_version,
                 (unsigned)s->schema_version);
        return SU_E_SCHEMA_MISMATCH;
    }
    if (gen != s->layout_generation) {
        snprintf(why, cap, "candidate layout_generation %u does not match world %u", (unsigned)gen,
                 (unsigned)s->layout_generation);
        return SU_E_SCHEMA_MISMATCH;
    }
    for (i = 0; i < m->field_count; i++) {
        const SuLirField *f = &m->fields[i];
        uint32_t k;
        int found = 0;
        for (k = 0; k < s->field_count; k++) {
            const SuSchedField *w = &s->fields[k];
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
 * A failed run commits nothing. */
static uint32_t run_host(SuWorld *w, const SuSysEntry *e) {
    const SuHostOpDesc *d = e->host;
    SuInvocation inv;
    SuOutputs out;
    SuColumnBinding rb[SU_LIMIT_FIELDS];
    SuColumnBinding wb[SU_LIMIT_FIELDS];
    uint32_t wid[SU_LIMIT_FIELDS];
    uint32_t nr = 0;
    uint32_t nw = 0;
    uint32_t i;
    uint32_t st;
    SuInvocation full_inv;
    SuOutputs full_out;
    uint32_t cur_in = 0;
    uint32_t cur_out = 0;

    st = su_world_build_invocation(w, &full_inv);
    if (st != SU_OK) {
        return st;
    }
    st = su_world_build_outputs(w, &full_out);
    if (st != SU_OK) {
        return st;
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
    st = d->run(w, &inv, &out, e->host_inst != NULL ? e->host_inst : d->host_ctx);
    if (st != SU_OK) {
        return st;
    }
    return su_world_commit_staging(w, wid, nw);
}

static uint32_t run_one_system(SuWorld *w, SuSchedule *s, uint32_t i, const SuSchedTickOpts *opt) {
    if (s->sys[i].kind == SU_SYS_KIND_HOST) {
        return run_host(w, &s->sys[i]);
    }
#ifdef SU_HAVE_NATIVE
    if (s->sys[i].native_seq != 0u) {
        if (opt != NULL && opt->workers > 1u && su_world_row_count(w) >= opt->chunk_min_rows) {
            return su_nv_run_system_chunked(s->sys[i].native_seq, w, opt->workers, opt->chunk_fault, opt->chunk_ctx);
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
        st = su_jobs_run_system_chunked(w, s->sys[i].mod, s->layout_generation, &jo, &rep);
        su_jobs_report_free(&rep);
        return st;
    }
    return su_vm_run_system(w, s->sys[i].ctx);
}

uint32_t su_sched_run_system(SuWorld *w, SuSchedule *s, uint32_t i, const SuSchedTickOpts *opt) {
    if (w == NULL || s == NULL || i >= s->count) {
        return SU_E_INVALID_ARGUMENT;
    }
    return run_one_system(w, s, i, opt);
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
        if (s->sys[k].mod->uses_entity_ops) {
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
                st = run_one_system(w, s, i, opt);
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

void su_sched_swap(SuSchedule *s, uint32_t idx, SuSysEntry *fresh, SuSysEntry *old) {
    *old = s->sys[idx];
    s->sys[idx] = *fresh;
    memset(fresh, 0, sizeof *fresh);
}
