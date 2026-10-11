#define _POSIX_C_SOURCE 200809L

#include "runtime/reload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/su_module.h"
#include "agent/su_path.h"
#include "su_platform.h"
#include "sutekh/ir.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"

#define RELOAD_CAPSULE_MAX (64u * 1024u)

void su_reload_module_free(SuLirModule *m) {
    if (m != NULL) {
        su_lir_module_free(m);
        free(m);
    }
}

uint32_t su_reload_load(const char *capsule_path, const char *source_path,
                        const char *expect_source_hash, const char *expect_capsule_hash,
                        SuLirModule **out, uint32_t *schema_version, uint32_t *layout_generation,
                        char *msg, size_t msg_cap) {
    uint8_t *cap = NULL;
    uint8_t *lir = NULL;
    size_t cap_len = 0;
    size_t lir_len = 0;
    char source_name[SU_MODULE_NAME_MAX + 1];
    SuLirModule *m = NULL;
    SuDiag diag;
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t st;

    *out = NULL;
    st = su_path_read_file(capsule_path, RELOAD_CAPSULE_MAX, &cap, &cap_len);
    if (st != SU_OK) {
        snprintf(msg, msg_cap, "cannot read capsule");
        goto done;
    }
    st = su_path_read_file(source_path, SU_LIMIT_IR_SOURCE_BYTES, &lir, &lir_len);
    if (st != SU_OK) {
        snprintf(msg, msg_cap, "cannot read source module");
        goto done;
    }
    st = su_module_capsule_meta(cap, cap_len, source_name, sizeof source_name, &sv, &gen);
    if (st != SU_OK) {
        snprintf(msg, msg_cap, "capsule metadata is malformed");
        goto done;
    }
    m = calloc(1, sizeof *m);
    if (m == NULL) {
        st = SU_E_RESOURCE_LIMIT;
        snprintf(msg, msg_cap, "allocation failed");
        goto done;
    }
    su_diag_init(&diag);
    st = su_lir_verify(cap, cap_len, lir, lir_len, m, &diag);
    if (st != SU_OK) {
        snprintf(msg, msg_cap, "verification failed: %.200s", diag.message);
        goto done;
    }
    if ((expect_source_hash != NULL && strcmp(expect_source_hash, m->source_hash_hex) != 0) ||
        (expect_capsule_hash != NULL && strcmp(expect_capsule_hash, m->capsule_hash_hex) != 0)) {
        st = SU_E_CANDIDATE_FAILED;
        snprintf(msg, msg_cap, "candidate bytes changed since submission");
        goto done;
    }
    *out = m;
    m = NULL;
    *schema_version = sv;
    *layout_generation = gen;
    st = SU_OK;
done:
    free(cap);
    free(lir);
    su_reload_module_free(m);
    return st;
}

/* Terminal failure of one transaction: records the state and fills out->msg. */
static uint32_t reject(SuRegistry *reg, SuReloadResult *out, uint64_t txid, SuRevState state,
                       uint32_t code, const char *reason) {
    char text[SU_REG_REASON_MAX];
    su_reg_finish(reg, txid, state, code, reason);
    /* Deliberate truncation of the reason to the registry record size. */
    size_t rlen = strnlen(reason, sizeof text - 1);
    memcpy(text, reason, rlen);
    text[rlen] = '\0';
    (void)snprintf(out->msg, sizeof out->msg, "txid=%llu state=%s: %.*s", (unsigned long long)txid,
                   su_rev_state_name(state), (int)rlen, text);
    return code;
}

static void step(SuRegistry *reg, uint64_t txid, SuRevState to) {
    (void)su_reg_advance(reg, txid, to);
}

uint32_t su_reload_install(SuRegistry *reg, SuSchedule *s, const SuReloadRequest *req,
                           SuReloadResult *out) {
    SuLirModule *mod = NULL;
    SuSysEntry fresh;
    SuSysEntry old;
    char msg[256];
    char why[SU_REG_REASON_MAX];
    uint32_t sv = 0;
    uint32_t gen = 0;
    uint32_t st;
    int idx;
    uint64_t txid;
    uint64_t t_entry = 0;
    uint64_t t_cs0 = 0;
    uint64_t t_cs1 = 0;
    int clk_entry;

    memset(out, 0, sizeof *out);
    clk_entry = su_platform_monotonic_ns(&t_entry);
    memset(&fresh, 0, sizeof fresh);
    memset(&old, 0, sizeof old);
    txid = su_reg_begin(reg, req->world_id, req->candidate_id, req->expect_source_hash,
                        req->expect_capsule_hash);
    if (txid == 0) {
        snprintf(out->msg, sizeof out->msg, "version registry is full of live revisions");
        return SU_E_RESOURCE_LIMIT;
    }
    out->txid = txid;

    /* 2. Stale expected revision (cheap, before any file is read). */
    if (strcmp(req->expected_revision, req->current_revision) != 0) {
        snprintf(msg, sizeof msg, "expected_world_revision %s does not match %s", req->expected_revision,
                 req->current_revision);
        return reject(reg, out, txid, SU_REV_STALE, SU_E_STALE_REVISION, msg);
    }

    /* 3. Verify the candidate bytes. */
    st = su_reload_load(req->capsule_path, req->source_path, req->expect_source_hash,
                        req->expect_capsule_hash, &mod, &sv, &gen, msg, sizeof msg);
    if (st != SU_OK) {
        return reject(reg, out, txid, SU_REV_REJECTED, st, msg);
    }
    step(reg, txid, SU_REV_SOURCE_VALIDATED);
    step(reg, txid, SU_REV_EFFECTS_VERIFIED);
    snprintf(out->system_id, sizeof out->system_id, "%s", mod->system_id);
    snprintf(out->source_hash, sizeof out->source_hash, "%s", mod->source_hash_hex);
    snprintf(out->capsule_hash, sizeof out->capsule_hash, "%s", mod->capsule_hash_hex);
    su_reg_set_system(reg, txid, mod->system_id);

    /* 4. The system must already be part of this world's schedule. */
    idx = su_sched_find(s, mod->system_id);
    if (idx < 0) {
        snprintf(why, sizeof why, "system %s is not in this world's schedule", mod->system_id);
        su_reload_module_free(mod);
        return reject(reg, out, txid, SU_REV_REJECTED, SU_E_INVALID_ARGUMENT, why);
    }

    /* 5. Layout compatibility with the world. */
    st = su_sched_check_candidate(s, mod, sv, gen, why, sizeof why);
    if (st != SU_OK) {
        su_reload_module_free(mod);
        return reject(reg, out, txid, SU_REV_REJECTED, st, why);
    }

    /* 6. Prepare the entry: the context borrows the heap module. */
    fresh.mod = mod;
    st = su_vm_context_create(mod, gen, &fresh.ctx);
    if (st != SU_OK) {
        su_reload_module_free(mod);
        return reject(reg, out, txid, SU_REV_REJECTED, SU_E_RESOURCE_LIMIT,
                      "VM context could not be prepared");
    }
    step(reg, txid, SU_REV_PREPARED);
    out->bindings_prepared = 1;

    /* 7. Evidence gate. The supervisor records evaluation results; the client cannot
     * set them. Only a passing evaluation admits a revision. */
    if (strcmp(req->evidence, "passed") != 0) {
        snprintf(why, sizeof why, "candidate has no passing evaluation (evidence: %s)", req->evidence);
        su_vm_context_destroy(fresh.ctx);
        su_reload_module_free(mod);
        return reject(reg, out, txid, SU_REV_REJECTED, SU_E_CANDIDATE_FAILED, why);
    }
    step(reg, txid, SU_REV_PREVIEW_TESTED);

    /* 8. Admission: recheck revision and layout generation. */
    step(reg, txid, SU_REV_ADMITTED);
    if (strcmp(req->expected_revision, req->current_revision) != 0) {
        su_vm_context_destroy(fresh.ctx);
        su_reload_module_free(mod);
        snprintf(msg, sizeof msg, "world revision changed during preparation");
        return reject(reg, out, txid, SU_REV_STALE, SU_E_STALE_REVISION, msg);
    }
    if (gen != s->layout_generation) {
        su_vm_context_destroy(fresh.ctx);
        su_reload_module_free(mod);
        return reject(reg, out, txid, SU_REV_REJECTED, SU_E_SCHEMA_MISMATCH,
                      "layout generation changed during preparation");
    }

    /* 9. Boundary: the install must take effect at the tick the client allowed. */
    step(reg, txid, SU_REV_PENDING_BOUNDARY);
    if (req->current_tick != req->allowed_effective_tick) {
        su_vm_context_destroy(fresh.ctx);
        su_reload_module_free(mod);
        snprintf(msg, sizeof msg, "allowed_effective_tick %llu but the world is at tick %llu",
                 (unsigned long long)req->allowed_effective_tick, (unsigned long long)req->current_tick);
        return reject(reg, out, txid, SU_REV_STALE, SU_E_STALE_REVISION, msg);
    }

    /* 10. Publish. The critical section starts here: the old entry is still live. */
    int clk_cs0 = su_platform_monotonic_ns(&t_cs0);
    fresh.txid = txid;
    fresh.system_rev = s->sys[idx].system_rev + 1u;
    fresh.installed_tick = req->current_tick;
    snprintf(fresh.system_id, sizeof fresh.system_id, "%s", mod->system_id);
    snprintf(fresh.source_hash, sizeof fresh.source_hash, "%s", mod->source_hash_hex);
    snprintf(fresh.capsule_hash, sizeof fresh.capsule_hash, "%s", mod->capsule_hash_hex);
    snprintf(fresh.semantic_hash, sizeof fresh.semantic_hash, "%s", mod->semantic_hash_hex);
    snprintf(out->previous_semantic_hash, sizeof out->previous_semantic_hash, "%s",
             s->sys[idx].semantic_hash);
    out->previous_txid = s->sys[idx].txid;
    /* The registry is updated before the schedule changes, so a registry failure leaves
     * the schedule untouched. */
    if (su_reg_install(reg, txid, fresh.system_rev, req->current_tick, fresh.semantic_hash) != 0) {
        su_vm_context_destroy(fresh.ctx);
        su_reload_module_free(mod);
        return SU_E_INTERNAL;
    }
    su_sched_swap(s, (uint32_t)idx, &fresh, &old);
    /* The new entry is published: the critical section ends. */
    {
        int clk_cs1 = su_platform_monotonic_ns(&t_cs1);
        out->clock_ok = clk_entry && clk_cs0 && clk_cs1;
        if (out->clock_ok) {
            out->prepare_ns = t_cs0 >= t_entry ? t_cs0 - t_entry : 0;
            out->critical_section_ns = t_cs1 >= t_cs0 ? t_cs1 - t_cs0 : 0;
        }
    }
    if (old.txid != 0) {
        (void)su_reg_retire(reg, old.txid);
    }
    su_sched_entry_destroy(&old);

    out->system_rev = s->sys[idx].system_rev;
    out->effective_tick = req->current_tick;
    snprintf(out->semantic_hash, sizeof out->semantic_hash, "%s", s->sys[idx].semantic_hash);
    snprintf(out->msg, sizeof out->msg, "txid=%llu state=installed", (unsigned long long)txid);
    return SU_OK;
}
