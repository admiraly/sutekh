#define _POSIX_C_SOURCE 200809L

#include "runtime/registry.h"

#include <stdio.h>
#include <string.h>

static const char *const k_state_names[] = {
    "received",     "source_validated", "effects_verified", "prepared",
    "preview_tested", "admitted",       "pending_boundary", "installed",
    "retired",      "rejected",         "stale",            "cancelled",
};

const char *su_rev_state_name(SuRevState s) {
    return (unsigned)s < sizeof k_state_names / sizeof k_state_names[0] ? k_state_names[s] : "unknown";
}

int su_rev_state_terminal(SuRevState s) {
    return s == SU_REV_RETIRED || s == SU_REV_REJECTED || s == SU_REV_STALE || s == SU_REV_CANCELLED;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

void su_reg_init(SuRegistry *r) {
    memset(r, 0, sizeof *r);
    r->next_txid = 0;
}

static SuRevRecord *find_slot_free_or_evict(SuRegistry *r) {
    SuRevRecord *victim = NULL;
    size_t i;
    for (i = 0; i < SU_REG_CAPACITY; i++) {
        if (!r->recs[i].used) {
            return &r->recs[i];
        }
    }
    for (i = 0; i < SU_REG_CAPACITY; i++) {
        SuRevRecord *c = &r->recs[i];
        if (su_rev_state_terminal(c->state) && (victim == NULL || c->txid < victim->txid)) {
            victim = c;
        }
    }
    if (victim != NULL) {
        r->evictions++;
        memset(victim, 0, sizeof *victim);
    }
    return victim;
}

uint64_t su_reg_begin(SuRegistry *r, const char *world_id, const char *candidate_id,
                      const char *source_hash, const char *capsule_hash) {
    SuRevRecord *rec = find_slot_free_or_evict(r);
    if (rec == NULL) {
        return 0;
    }
    memset(rec, 0, sizeof *rec);
    r->next_txid++;
    rec->used = 1;
    rec->txid = r->next_txid;
    rec->state = SU_REV_RECEIVED;
    rec->code = SU_OK;
    copy_text(rec->world_id, sizeof rec->world_id, world_id);
    copy_text(rec->candidate_id, sizeof rec->candidate_id, candidate_id);
    copy_text(rec->source_hash, sizeof rec->source_hash, source_hash);
    copy_text(rec->capsule_hash, sizeof rec->capsule_hash, capsule_hash);
    return rec->txid;
}

SuRevRecord *su_reg_find(SuRegistry *r, uint64_t txid) {
    size_t i;
    if (txid == 0) {
        return NULL;
    }
    for (i = 0; i < SU_REG_CAPACITY; i++) {
        if (r->recs[i].used && r->recs[i].txid == txid) {
            return &r->recs[i];
        }
    }
    return NULL;
}

int su_reg_advance(SuRegistry *r, uint64_t txid, SuRevState to) {
    SuRevRecord *rec = su_reg_find(r, txid);
    if (rec == NULL || su_rev_state_terminal(rec->state) || rec->state == SU_REV_INSTALLED) {
        return -1;
    }
    if (su_rev_state_terminal(to)) {
        rec->state = to;
        return 0;
    }
    if (to == SU_REV_INSTALLED) {
        return -1; /* only su_reg_install may enter INSTALLED */
    }
    if ((int)to <= (int)rec->state) {
        return -1;
    }
    rec->state = to;
    return 0;
}

void su_reg_set_system(SuRegistry *r, uint64_t txid, const char *system_id) {
    SuRevRecord *rec = su_reg_find(r, txid);
    if (rec != NULL) {
        copy_text(rec->system_id, sizeof rec->system_id, system_id);
    }
}

void su_reg_finish(SuRegistry *r, uint64_t txid, SuRevState terminal, uint32_t code,
                   const char *reason) {
    SuRevRecord *rec = su_reg_find(r, txid);
    if (rec == NULL || !su_rev_state_terminal(terminal)) {
        return;
    }
    if (su_rev_state_terminal(rec->state) || rec->state == SU_REV_INSTALLED) {
        return;
    }
    rec->state = terminal;
    rec->code = code;
    copy_text(rec->reason, sizeof rec->reason, reason);
}

int su_reg_install(SuRegistry *r, uint64_t txid, uint64_t system_rev, uint64_t effective_tick,
                   const char *semantic_hash) {
    SuRevRecord *rec = su_reg_find(r, txid);
    if (rec == NULL || su_rev_state_terminal(rec->state) || rec->state == SU_REV_INSTALLED) {
        return -1;
    }
    rec->state = SU_REV_INSTALLED;
    rec->system_rev = system_rev;
    rec->effective_tick = effective_tick;
    copy_text(rec->semantic_hash, sizeof rec->semantic_hash, semantic_hash);
    return 0;
}

int su_reg_retire(SuRegistry *r, uint64_t txid) {
    SuRevRecord *rec = su_reg_find(r, txid);
    if (rec == NULL || rec->state != SU_REV_INSTALLED) {
        return -1;
    }
    rec->state = SU_REV_RETIRED;
    return 0;
}

void su_reg_stats(const SuRegistry *r, uint32_t *in_use, uint32_t *installed) {
    uint32_t u = 0;
    uint32_t n = 0;
    size_t i;
    for (i = 0; i < SU_REG_CAPACITY; i++) {
        if (r->recs[i].used) {
            u++;
            if (r->recs[i].state == SU_REV_INSTALLED) {
                n++;
            }
        }
    }
    *in_use = u;
    *installed = n;
}
