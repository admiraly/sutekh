/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.3 through the in-process host API (include/sutekh/host.h): an IR 0.3 capsule builds a world, the typed
 * helpers report value types and lanes, a q32 input queued with su_host_input_push_i64 is applied before the
 * tick, and su_host_read_i64 returns the value the profile defines (hand-derived). Run from the repository root. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/su_num.h"
#include "sutekh/host.h"
#include "sutekh/ir.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define ONE SU_Q32_ONE

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    b = malloc((size_t)sz + 1u);
    *len = b != NULL ? fread(b, 1, (size_t)sz, f) : 0u;
    fclose(f);
    return b;
}

int main(void) {
    SuHostCapsule cap;
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint64_t ent = 0, done = 0;
    uint32_t vt = 0, lane = 0;
    int64_t v[1] = {0};
    int64_t dt = 0;
    uint32_t cells[1];
    /* rot_w = 1.0 (id 30: low word 0, high word 1), vel_x = 2.0 (id 20), vel_y = -0.5 (id 22). */
    SuHostValue init[3] = {{31u, 1u}, {21u, 2u}, {22u, 0x80000000u}};
    memset(&cap, 0, sizeof cap);
    cap.capsule_json = slurp("tests/fixtures/ir03/kinematics.capsule.json", &cap.capsule_len);
    cap.lir_json = slurp("tests/fixtures/ir03/kinematics.lir.json", &cap.lir_len);
    if (cap.capsule_json == NULL || cap.lir_json == NULL) {
        fprintf(stderr, "run from the repository root\n");
        return 1;
    }
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d;
    d.capsule_count = 1;
    d.capsules = &cap;
    d.max_entities = 4;
    d.tick_rate_hz = 120;
    CHECK(su_host_world_create(&d, &w, &e) == SU_OK);
    if (w == NULL) {
        fprintf(stderr, "create: %s\n", e.message);
        return 1;
    }
    CHECK(su_host_field_value_type(w, 10u, &vt, &lane) == SU_OK && vt == SU_TYPE_Q32 && lane == 0u);
    CHECK(su_host_field_value_type(w, 11u, &vt, &lane) == SU_OK && vt == SU_TYPE_Q32 && lane == 1u);
    CHECK(su_host_field_value_type(w, 44u, &vt, &lane) == SU_OK && vt == SU_TYPE_Q32 && lane == 0u);
    CHECK(su_host_field_value_type(w, 999u, &vt, &lane) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_spawn(w, init, 3u, 7u, &ent, &e) == SU_OK);
    /* vel_y high word 0xFFFFFFFF with low 0x80000000 is -0.5: set the high lane with a plain cell write. */
    {
        SuHostInput hi;
        hi.entity = ent;
        hi.field_id = 23u;
        hi.value = 0xFFFFFFFFu;
        CHECK(su_host_input_push(w, 0, &hi, 1u, &e) == SU_OK);
    }
    CHECK(su_host_input_push_i64(w, 0, ent, 10u, 3 * (ONE / 2), &e) == SU_OK); /* pos_x = 1.5 */
    CHECK(su_host_input_push_i64(w, 0, ent, 11u, 0, &e) == SU_E_TYPE);         /* high lane */
    CHECK(su_host_input_push_i64(w, 0, ent, 999u, 0, &e) == SU_E_SCHEMA_MISMATCH);
    CHECK(su_host_step(w, 1, &done, &e) == SU_OK && done == 1u);
    CHECK(su_q32_from_ratio(1, 120, &dt));
    CHECK(su_host_read_i64(w, 10u, 0, 1, v) == SU_OK && v[0] == 3 * (ONE / 2) + su_q32_mul(2 * ONE, dt));
    CHECK(su_host_read_i64(w, 12u, 0, 1, v) == SU_OK && v[0] == su_q32_mul(-(ONE / 2), dt));
    CHECK(su_host_read_i64(w, 30u, 0, 1, v) == SU_OK && v[0] == ONE); /* zero angular velocity: identity */
    CHECK(su_host_read_i64(w, 11u, 0, 1, v) == SU_E_TYPE);
    CHECK(su_host_read_i64(w, 10u, 4, 1, v) == SU_E_INVALID_ARGUMENT);
    CHECK(su_host_read(w, 11u, 0, 1, cells) == SU_OK && cells[0] == 1u); /* the raw high lane of 1.5 + 1/60 */
    su_host_world_destroy(w);
    free((void *)cap.capsule_json);
    free((void *)cap.lir_json);
    printf("test_host03: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
