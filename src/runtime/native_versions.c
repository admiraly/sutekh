/* Native kernel artifact registry (M1-02b). See native_versions.h. Compiled only with SU_HAVE_NATIVE. */
#include "runtime/native_versions.h"

#ifdef SU_HAVE_NATIVE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SU_TEST_FAULT_HOOKS
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

#include "backends/native/su_native.h"
#include "sutekh/abi.h"
#include "sutekh/native/abi.h"
#include "sutekh/status.h"
#include "vm/su_vm.h"
#include "world/su_world.h"

typedef struct SuNvArtifact SuNvArtifact;

struct SuNvArtifact {
    uint64_t seq;
    uint32_t state;
    uint32_t inflight;
    const SuLirModule *mod;         /* borrowed */
    uint32_t layout_generation;
    uint32_t caps;
    SuNativeArtifact *art;
    char artifact_hash[65];
    uint32_t backend_id;
    uint64_t code_bytes;
    SuColumnBinding *scr_in;        /* field_count entries */
    SuColumnBinding *scr_out;
    uint32_t n_in;
    uint32_t n_out;
    uint32_t *write_ids;            /* writable field ids, table order */
    SuNvMismatch first;             /* first mismatch of the last admission */
    uint32_t cur_case;              /* admission bookkeeping for `first` */
    uint64_t cur_rows;
    uint32_t cur_offset;
};
static SuNvArtifact *find(uint64_t seq);

#ifdef SU_TEST_FAULT_HOOKS
static uint32_t g_fault_mode = SU_NV_FAULT_NONE;
static uint32_t g_fault_delay_ms = 0u;

void su_nv_test_arm(uint32_t mode, uint32_t delay_ms) {
    g_fault_mode = mode;
    g_fault_delay_ms = delay_ms;
}

/* Runs before every kernel call on the admission and world paths. */
static void fault_before_kernel(void) {
    if (g_fault_mode == SU_NV_FAULT_CRASH) {
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
    } else if (g_fault_mode == SU_NV_FAULT_HANG) {
        for (;;) {
            sleep(1);
        }
    } else if (g_fault_mode == SU_NV_FAULT_DELAY) {
        struct timespec ts;
        ts.tv_sec = (time_t)(g_fault_delay_ms / 1000u);
        ts.tv_nsec = (long)(g_fault_delay_ms % 1000u) * 1000000L;
        nanosleep(&ts, NULL);
    }
}
#define NV_FAULT_BEFORE() fault_before_kernel()
#else
void su_nv_test_arm(uint32_t mode, uint32_t delay_ms) {
    (void)mode;
    (void)delay_ms;
}
#define NV_FAULT_BEFORE() ((void)0)
#endif

static void note_first(SuNvArtifact *a, uint32_t field_id, uint64_t row, uint32_t expected,
                       uint32_t actual) {
    if (a->first.valid) {
        return;
    }
    a->first.valid = 1u;
    a->first.case_index = a->cur_case;
    a->first.case_rows = (uint32_t)a->cur_rows;
    a->first.case_offset = a->cur_offset;
    a->first.field_id = field_id;
    a->first.row = row;
    a->first.expected = expected;
    a->first.actual = actual;
}

uint32_t su_nv_first_mismatch(uint64_t seq, SuNvMismatch *out) {
    SuNvArtifact *a = find(seq);
    if (a == NULL || out == NULL) {
        return SU_E_INTERNAL;
    }
    *out = a->first;
    return SU_OK;
}

uint32_t su_nv_trust_admitted(uint64_t seq, const char *admitted_hash) {
    SuNvArtifact *a = find(seq);
    if (a == NULL || a->state != SU_NV_STATE_PREPARED || admitted_hash == NULL) {
        return SU_E_INTERNAL;
    }
    if (strcmp(a->artifact_hash, admitted_hash) != 0) {
        return SU_E_CANDIDATE_FAILED;
    }
    a->state = SU_NV_STATE_ADMITTED;
    return SU_OK;
}

/* Process-global registry: the worker is single-threaded. Slots hold live artifacts only. */
static SuNvArtifact *g_slots[SU_NV_MAX_ARTIFACTS];
static uint64_t g_next_seq = 1u;
static uint64_t g_released = 0u;

static SuNvArtifact *find(uint64_t seq) {
    uint32_t i;
    if (seq == 0u) {
        return NULL;
    }
    for (i = 0; i < SU_NV_MAX_ARTIFACTS; i++) {
        if (g_slots[i] != NULL && g_slots[i]->seq == seq) {
            return g_slots[i];
        }
    }
    return NULL;
}

static void unregister_and_free(SuNvArtifact *a) {
    uint32_t i;
    for (i = 0; i < SU_NV_MAX_ARTIFACTS; i++) {
        if (g_slots[i] == a) {
            g_slots[i] = NULL;
        }
    }
    su_native_artifact_free(a->art);
    free(a->scr_in);
    free(a->scr_out);
    free(a->write_ids);
    free(a);
    g_released++;
}

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2u * i] = digits[b[i] >> 4];
        out[2u * i + 1u] = digits[b[i] & 0xFu];
    }
    out[2u * n] = '\0';
}

uint32_t su_nv_prepare(const SuLirModule *m, uint32_t layout_generation, uint32_t caps,
                       uint64_t *seq, char *msg, size_t cap) {
    SuNvArtifact *a;
    SuNativeDiag diag;
    SuNativeArtifactInfo info;
    uint32_t st;
    uint32_t slot = SU_NV_MAX_ARTIFACTS;
    uint32_t i;
    uint32_t f;

    *seq = 0u;
    for (i = 0; i < SU_NV_MAX_ARTIFACTS; i++) {
        if (g_slots[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == SU_NV_MAX_ARTIFACTS) {
        snprintf(msg, cap, "native artifact registry full (%u live)", (unsigned)SU_NV_MAX_ARTIFACTS);
        return SU_E_RESOURCE_LIMIT;
    }
    a = calloc(1u, sizeof *a);
    if (a == NULL) {
        snprintf(msg, cap, "out of memory");
        return SU_E_RESOURCE_LIMIT;
    }
    st = su_native_compile(m, layout_generation, caps, &a->art, &diag);
    if (st != SU_OK) {
        snprintf(msg, cap, "%s", diag.message[0] != '\0' ? diag.message : su_status_name(st));
        free(a);
        return st;
    }
    if (su_native_artifact_info(a->art, &info) != SU_OK) {
        su_native_artifact_free(a->art);
        free(a);
        snprintf(msg, cap, "artifact info unavailable");
        return SU_E_INTERNAL;
    }
    a->mod = m;
    a->layout_generation = layout_generation;
    a->caps = caps;
    a->backend_id = info.backend_id;
    a->code_bytes = info.code_size;
    hex_of(info.artifact_hash, SU_HASH_BYTES, a->artifact_hash);
    a->scr_in = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *a->scr_in);
    a->scr_out = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *a->scr_out);
    a->write_ids = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *a->write_ids);
    if (a->scr_in == NULL || a->scr_out == NULL || a->write_ids == NULL) {
        su_native_artifact_free(a->art);
        free(a->scr_in);
        free(a->scr_out);
        free(a->write_ids);
        free(a);
        snprintf(msg, cap, "out of memory");
        return SU_E_RESOURCE_LIMIT;
    }
    for (f = 0; f < m->field_count; f++) {
        if ((m->fields[f].access & SU_ACCESS_WRITE) != 0u) {
            a->write_ids[a->n_out++] = m->fields[f].id;
        }
    }
    a->seq = g_next_seq++;
    a->state = SU_NV_STATE_PREPARED;
    g_slots[slot] = a;
    *seq = a->seq;
    return SU_OK;
}

/* ---- admission -------------------------------------------------------------------------- */

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static const uint64_t k_sizes[] = {0u, 1u, 7u, 8u, 9u, 17u, 1024u};

/* One corpus case: fills the inputs of rows (offset 0 or 1 element), runs the VM and the
 * kernel, compares every staged word and both statuses. Returns the number of mismatches. */
static uint64_t admit_case(SuNvArtifact *a, SuExecContext *vm, uint64_t rows, uint32_t off,
                           uint32_t *rng, uint32_t edge) {
    const SuLirModule *m = a->mod;
    uint32_t **in_base;
    uint32_t **in_buf;
    uint32_t **vm_out;
    uint32_t **nat_out;
    uint32_t i;
    uint64_t r;
    uint64_t bad = 0;
    uint32_t n_in = 0;
    uint32_t n_out = 0;
    SuInvocation inv;
    SuOutputs outs_vm;
    SuOutputs outs_nat;
    uint32_t st_vm;
    uint32_t st_nat;
    uint32_t fidx;

    in_base = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *in_base);
    in_buf = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *in_buf);
    vm_out = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *vm_out);
    nat_out = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *nat_out);
    if (in_base == NULL || in_buf == NULL || vm_out == NULL || nat_out == NULL) {
        free(in_base);
        free(in_buf);
        free(vm_out);
        free(nat_out);
        return 1u;
    }
    /* Column storage: one extra element so the view can start one element in. */
    for (fidx = 0; fidx < m->field_count; fidx++) {
        uint32_t acc = m->fields[fidx].access;
        uint32_t *base;
        uint64_t k;
        if ((acc & SU_ACCESS_READ) != 0u) {
            base = malloc(((size_t)rows + 2u) * sizeof *base);
            if (base == NULL) {
                bad++;
                continue;
            }
            for (k = 0; k < rows + 2u; k++) {
                uint32_t v;
                if (edge == 1u) {
                    v = (k & 1u) ? 0xFFFFFFFFu : 0u;
                } else if (edge == 2u) {
                    v = (uint32_t)(0x80000000u | (uint32_t)k);
                } else {
                    v = xorshift(rng);
                }
                base[k] = v;
            }
            in_base[n_in] = base;
            in_buf[n_in] = base + off;
            n_in++;
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            uint32_t *vb = malloc(((size_t)rows + 2u) * sizeof *vb);
            uint32_t *nb = malloc(((size_t)rows + 2u) * sizeof *nb);
            if (vb == NULL || nb == NULL) {
                free(vb);
                free(nb);
                bad++;
                continue;
            }
            for (k = 0; k < rows + 2u; k++) {
                vb[k] = 0xA5A5A5A5u ^ (uint32_t)k;
                nb[k] = 0xA5A5A5A5u ^ (uint32_t)k;
            }
            vm_out[n_out] = vb + off;
            nat_out[n_out] = nb + off;
            n_out++;
        }
    }
    if (bad != 0u) {
        goto done;
    }

    /* The kernel and the VM receive the same bindings in table order (as su_vm_run_system). */
    {
        uint32_t ri = 0;
        uint32_t wi = 0;
        SuColumnBinding *bvo = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *bvo);
        SuColumnBinding *bno = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *bno);
        SuColumnBinding *bvi = calloc(m->field_count > 0u ? m->field_count : 1u, sizeof *bvi);
        if (bvo == NULL || bno == NULL || bvi == NULL) {
            free(bvo);
            free(bno);
            free(bvi);
            bad++;
            goto done;
        }
        for (fidx = 0; fidx < m->field_count; fidx++) {
            uint32_t acc = m->fields[fidx].access;
            if ((acc & SU_ACCESS_READ) != 0u) {
                SuColumnBinding *b = &bvi[ri];
                memset(b, 0, sizeof *b);
                b->struct_size = (uint32_t)sizeof *b;
                b->field_id = m->fields[fidx].id;
                b->elem_type = SU_TYPE_U32;
                b->access = SU_ACCESS_READ;
                b->layout_generation = a->layout_generation;
                b->stride_bytes = 4u;
                b->row_count = rows;
                b->data = in_buf[ri];
                ri++;
            }
            if ((acc & SU_ACCESS_WRITE) != 0u) {
                SuColumnBinding *v = &bvo[wi];
                SuColumnBinding *n = &bno[wi];
                memset(v, 0, sizeof *v);
                memset(n, 0, sizeof *n);
                v->struct_size = n->struct_size = (uint32_t)sizeof *v;
                v->field_id = n->field_id = m->fields[fidx].id;
                v->elem_type = n->elem_type = SU_TYPE_U32;
                v->access = n->access = SU_ACCESS_WRITE;
                v->layout_generation = n->layout_generation = a->layout_generation;
                v->stride_bytes = n->stride_bytes = 4u;
                v->row_count = n->row_count = rows;
                v->data = vm_out[wi];
                n->data = nat_out[wi];
                wi++;
            }
        }
        inv.struct_size = (uint32_t)sizeof inv;
        inv.abi_version = SU_ABI_VERSION;
        inv.layout_generation = a->layout_generation;
        inv.binding_count = n_in;
        inv.row_count = rows;
        inv.bindings = bvi;

        outs_vm.struct_size = (uint32_t)sizeof outs_vm;
        outs_vm.binding_count = n_out;
        outs_vm.failed_insn = UINT32_MAX;
        outs_vm.reserved = 0u;
        outs_vm.bindings = bvo;
        outs_nat = outs_vm;
        outs_nat.bindings = bno;

        st_vm = su_vm_invoke_v1(vm, &inv, &outs_vm);
        NV_FAULT_BEFORE();
        st_nat = su_native_run_checked(a->art, &inv, &outs_nat);
#ifdef SU_TEST_FAULT_HOOKS
        if (g_fault_mode == SU_NV_FAULT_WRONG_KERNEL && st_nat == SU_OK && n_out > 0u && rows > 0u) {
            nat_out[0][0] ^= 1u;
        }
#endif
        if (st_vm != st_nat) {
            bad++;
            note_first(a, 0u, 0u, (uint32_t)st_vm, (uint32_t)st_nat);
        } else if (st_vm == SU_OK) {
            for (i = 0; i < n_out; i++) {
                for (r = 0; r < rows; r++) {
                    if (vm_out[i][r] != nat_out[i][r]) {
                        bad++;
                        note_first(a, a->write_ids[i], r, vm_out[i][r], nat_out[i][r]);
                    }
                }
                /* One element beyond the view must be untouched by both. */
                if (vm_out[i][rows] != nat_out[i][rows]) {
                    bad++;
                }
            }
        }
        free(bvo);
        free(bno);
        free(bvi);
    }
done:
    for (i = 0; i < n_in; i++) {
        free(in_base[i]);
    }
    for (i = 0; i < n_out; i++) {
        free(vm_out[i] - off);
        free(nat_out[i] - off);
    }
    free(in_base);
    free(in_buf);
    free(vm_out);
    free(nat_out);
    return bad;
}

uint32_t su_nv_admit(uint64_t seq, uint32_t seed, uint32_t *cases, uint64_t *rows_checked,
                     uint64_t *mismatches) {
    SuNvArtifact *a = find(seq);
    SuExecContext *vm = NULL;
    uint32_t rng = seed != 0u ? seed : 1u;
    uint32_t n = 0;
    uint64_t rows = 0;
    uint64_t bad = 0;
    size_t s;
    uint32_t st;

    *cases = 0;
    *rows_checked = 0;
    *mismatches = 0;
    if (a == NULL || a->state != SU_NV_STATE_PREPARED) {
        return SU_E_INTERNAL;
    }
    st = su_vm_context_create(a->mod, a->layout_generation, &vm);
    if (st != SU_OK) {
        return st;
    }
    memset(&a->first, 0, sizeof a->first);
    for (s = 0; s < sizeof k_sizes / sizeof k_sizes[0]; s++) {
        uint32_t off;
        for (off = 0; off < 2u; off++) {
            uint32_t rep;
            for (rep = 0; rep < 3u; rep++) {
                uint32_t edge = rep == 0u ? 1u : (rep == 1u ? 2u : 0u);
                a->cur_case = n;
                a->cur_rows = k_sizes[s];
                a->cur_offset = off;
                bad += admit_case(a, vm, k_sizes[s], off, &rng, edge);
                n++;
                rows += k_sizes[s];
            }
        }
    }
    su_vm_context_destroy(vm);
    *cases = n;
    *rows_checked = rows;
    *mismatches = bad;
    if (bad == 0u) {
        a->state = SU_NV_STATE_ADMITTED;
        return SU_OK;
    }
    return SU_E_CANDIDATE_FAILED;
}

uint32_t su_nv_install(uint64_t seq) {
    SuNvArtifact *a = find(seq);
    if (a == NULL) {
        return SU_E_INTERNAL;
    }
    if (a->state != SU_NV_STATE_ADMITTED) {
        return SU_E_INTERNAL;
    }
    a->state = SU_NV_STATE_INSTALLED;
    return SU_OK;
}

/* Binding selection and commit as su_vm_run_system, with the kernel in place of the VM. */
static uint32_t run_bound(SuNvArtifact *a, SuWorld *w) {
    const SuLirModule *m = a->mod;
    SuInvocation wi;
    SuOutputs wo;
    SuInvocation inv;
    SuOutputs outs;
    uint32_t i;
    uint32_t n_in = 0;
    uint32_t n_out = 0;
    uint32_t st;

    st = su_world_build_invocation(w, &wi);
    if (st != SU_OK) {
        return st;
    }
    st = su_world_build_outputs(w, &wo);
    if (st != SU_OK) {
        return st;
    }
    for (i = 0; i < m->field_count; i++) {
        uint32_t acc = m->fields[i].access;
        uint32_t id = m->fields[i].id;
        uint32_t j;
        int found = 0;
        if ((acc & SU_ACCESS_READ) != 0u) {
            for (j = 0; j < wi.binding_count && !found; j++) {
                if (wi.bindings[j].field_id == id) {
                    a->scr_in[n_in++] = wi.bindings[j];
                    found = 1;
                }
            }
            if (!found) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
        if ((acc & SU_ACCESS_WRITE) != 0u) {
            found = 0;
            for (j = 0; j < wo.binding_count && !found; j++) {
                if (wo.bindings[j].field_id == id) {
                    a->scr_out[n_out++] = wo.bindings[j];
                    found = 1;
                }
            }
            if (!found) {
                return SU_E_SCHEMA_MISMATCH;
            }
        }
    }
    inv.struct_size = (uint32_t)sizeof inv;
    inv.abi_version = SU_ABI_VERSION;
    inv.layout_generation = wi.layout_generation;
    inv.binding_count = n_in;
    inv.row_count = wi.row_count;
    inv.bindings = a->scr_in;
    outs.struct_size = (uint32_t)sizeof outs;
    outs.binding_count = n_out;
    outs.failed_insn = UINT32_MAX;
    outs.reserved = 0u;
    outs.bindings = a->scr_out;

    NV_FAULT_BEFORE();
    st = su_native_run_checked(a->art, &inv, &outs);
#ifdef SU_TEST_FAULT_HOOKS
    if (g_fault_mode == SU_NV_FAULT_WRONG_WORLD && st == SU_OK && n_out > 0u && wi.row_count > 0u) {
        ((uint32_t *)a->scr_out[0].data)[0] ^= 1u;
    }
#endif
    if (st != SU_OK) {
        return st;
    }
    return su_world_commit_staging(w, a->write_ids, n_out);
}

uint32_t su_nv_run_system(uint64_t seq, SuWorld *w) {
    SuNvArtifact *a = find(seq);
    uint32_t st;
    if (a == NULL || w == NULL || a->state != SU_NV_STATE_INSTALLED) {
        return SU_E_INTERNAL;
    }
    a->inflight++;
    st = run_bound(a, w);
    a->inflight--;
    if (a->state == SU_NV_STATE_RETIRING && a->inflight == 0u) {
        unregister_and_free(a);
    }
    return st;
}

void su_nv_release(uint64_t seq) {
    SuNvArtifact *a = find(seq);
    if (a == NULL || a->state == SU_NV_STATE_RETIRING) {
        return;
    }
    a->state = SU_NV_STATE_RETIRING;
    if (a->inflight == 0u) {
        unregister_and_free(a);
    }
}

uint32_t su_nv_state(uint64_t seq) {
    SuNvArtifact *a = find(seq);
    return a == NULL ? SU_NV_STATE_NONE : a->state;
}

uint32_t su_nv_live_count(void) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < SU_NV_MAX_ARTIFACTS; i++) {
        n += g_slots[i] != NULL ? 1u : 0u;
    }
    return n;
}

uint32_t su_nv_inflight(uint64_t seq) {
    SuNvArtifact *a = find(seq);
    return a == NULL ? 0u : a->inflight;
}

uint64_t su_nv_released_count(void) {
    return g_released;
}

uint32_t su_nv_artifact_hash(uint64_t seq, char out[65]) {
    SuNvArtifact *a = find(seq);
    if (a == NULL) {
        return SU_E_INTERNAL;
    }
    memcpy(out, a->artifact_hash, 65u);
    return SU_OK;
}

uint32_t su_nv_backend_id(uint64_t seq, uint32_t *out) {
    SuNvArtifact *a = find(seq);
    if (a == NULL) {
        return SU_E_INTERNAL;
    }
    *out = a->backend_id;
    return SU_OK;
}

uint32_t su_nv_code_bytes(uint64_t seq, uint64_t *out) {
    SuNvArtifact *a = find(seq);
    if (a == NULL) {
        return SU_E_INTERNAL;
    }
    *out = a->code_bytes;
    return SU_OK;
}

void su_nv_test_enter(uint64_t seq) {
    SuNvArtifact *a = find(seq);
    if (a != NULL) {
        a->inflight++;
    }
}

void su_nv_test_leave(uint64_t seq) {
    SuNvArtifact *a = find(seq);
    if (a == NULL || a->inflight == 0u) {
        return;
    }
    a->inflight--;
    if (a->state == SU_NV_STATE_RETIRING && a->inflight == 0u) {
        unregister_and_free(a);
    }
}

#endif /* SU_HAVE_NATIVE */
