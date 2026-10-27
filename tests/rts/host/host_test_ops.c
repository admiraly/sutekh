/* Test-only host operators (M3-04a). See host_test_ops.h. */
#define _POSIX_C_SOURCE 200809L

#include "host_test_ops.h"

#include <stddef.h>
#include <string.h>

#include "ir/su_sha256.h"
#include "sutekh/abi.h"
#include "sutekh/status.h"

static const char k_vel_text[] = "test_host_ops v1\nvel_bump: staging = working + 1 (all rows)\n";
static const char k_pos_text[] = "test_host_ops v1\npos_writer: staging = 0 (all rows)\n";
static const char k_tgt_text[] = "test_host_ops v1\ntarget_writer: staging[0] = working[0] + 100 (row 0 only)\n";
static const char k_probe_text[] = "test_host_ops v1\nprobe: count nonzero dead rows of velocity_x\n";

static const SuHostField k_vel_fields[1] = {
    {2u, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "velocity_x"},
};
static const SuHostField k_pos_fields[1] = {
    {1u, SU_TYPE_U32, SU_ACCESS_WRITE, "position_x"},
};

static const SuHostField k_rw_fields[1] = {
    {2u, SU_TYPE_U32, SU_ACCESS_READ | SU_ACCESS_WRITE, "velocity_x"},
};
static const SuHostField k_r_fields[1] = {
    {2u, SU_TYPE_U32, SU_ACCESS_READ, "velocity_x"},
};

static uint32_t *column_of(const SuColumnBinding *b, uint32_t n, uint32_t id, uint64_t rows) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (b[i].field_id == id && b[i].row_count == rows) {
            return (uint32_t *)b[i].data;
        }
    }
    return NULL;
}

static uint32_t run_vel_bump(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    TestHostCtx *c = (TestHostCtx *)host_ctx;
    const uint32_t *src;
    uint32_t *dst;
    uint64_t r;
    uint32_t idx;
    (void)w;
    idx = c->calls++;
    if (idx == c->fail_at) {
        return c->fail_status;
    }
    if (in->binding_count != 1u || out->binding_count != 1u) {
        c->bad_views++;
    }
    src = column_of(in->bindings, in->binding_count, 2u, in->row_count);
    dst = column_of(out->bindings, out->binding_count, 2u, in->row_count);
    if (src == NULL || dst == NULL) {
        c->bad_views++;
        return SU_E_INTERNAL;
    }
    for (r = 0; r < in->row_count; r++) {
        dst[r] = src[r] + 1u;
    }
    return SU_OK;
}

static uint32_t run_pos_writer(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    uint32_t *dst;
    uint64_t r;
    (void)w;
    (void)in;
    (void)host_ctx;
    dst = column_of(out->bindings, out->binding_count, 1u, in->row_count);
    if (dst == NULL) {
        return SU_E_INTERNAL;
    }
    for (r = 0; r < in->row_count; r++) {
        dst[r] = 0u;
    }
    return SU_OK;
}

static uint32_t run_target_writer(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    const uint32_t *src = column_of(in->bindings, in->binding_count, 2u, in->row_count);
    uint32_t *dst = column_of(out->bindings, out->binding_count, 2u, in->row_count);
    (void)w;
    if (host_ctx != NULL) {
        ((TestHostCtx *)host_ctx)->calls++;
    }
    if (src == NULL || dst == NULL || in->row_count == 0u) {
        return SU_E_INTERNAL;
    }
    dst[0] = src[0] + 100u; /* only the target row is stored; the rest is the working copy */
    return SU_OK;
}

static uint32_t run_probe(const SuWorld *w, const SuInvocation *in, SuOutputs *out, void *host_ctx) {
    TestHostCtx *c = (TestHostCtx *)host_ctx;
    const uint32_t *src = column_of(in->bindings, in->binding_count, 2u, in->row_count);
    uint64_t r;
    (void)w;
    (void)out;
    c->calls++;
    if (src == NULL) {
        return SU_E_INTERNAL;
    }
    for (r = TEST_ALIVE_ROWS; r < in->row_count; r++) {
        if (src[r] != 0u) {
            c->dead_nonzero++;
        }
    }
    return SU_OK;
}

SuHostOpDesc g_test_target_writer = {
    TEST_HOST_TGT_ID, "demo.actor", 1u, {0}, 1u, k_rw_fields, run_target_writer, NULL,
    0u, NULL, NULL, /* no target set (issue 140) */
};
SuHostOpDesc g_test_probe = {
    TEST_HOST_PROBE_ID, "demo.actor", 1u, {0}, 1u, k_r_fields, run_probe, NULL,
    0u, NULL, NULL, /* no target set (issue 140) */
};

SuHostOpDesc g_test_vel_bump = {
    TEST_HOST_VEL_ID, "demo.actor", 1u, {0}, 1u, k_vel_fields, run_vel_bump, NULL,
    0u, NULL, NULL, /* no target set (issue 140) */
};
SuHostOpDesc g_test_pos_writer = {
    TEST_HOST_POS_ID, "demo.actor", 1u, {0}, 1u, k_pos_fields, run_pos_writer, NULL,
    0u, NULL, NULL, /* no target set (issue 140) */
};

void test_host_ops_init(void) {
    SuSha256 c;
    su_sha256_init(&c);
    su_sha256_update(&c, k_vel_text, strlen(k_vel_text));
    su_sha256_final(&c, g_test_vel_bump.constants_hash);
    su_sha256_init(&c);
    su_sha256_update(&c, k_pos_text, strlen(k_pos_text));
    su_sha256_final(&c, g_test_pos_writer.constants_hash);
    su_sha256_init(&c);
    su_sha256_update(&c, k_tgt_text, strlen(k_tgt_text));
    su_sha256_final(&c, g_test_target_writer.constants_hash);
    su_sha256_init(&c);
    su_sha256_update(&c, k_probe_text, strlen(k_probe_text));
    su_sha256_final(&c, g_test_probe.constants_hash);
}
