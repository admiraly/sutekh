/* Minimal presentation-host loop over the in-process API (sutekh/host.h, sutekh/clock.h).
 *
 * It creates a world from a capsule, its .lir.json and a fixture, then runs a simulated frame loop: frame
 * times vary around 144 Hz, the fixed-step clock decides how many 120 Hz ticks each frame runs, inputs are
 * pushed per tick, and positions are interpolated between the last two ticks with the clock's alpha (what a
 * renderer would draw). Halfway through it demonstrates a rollback: restore an earlier snapshot, re-run the
 * same inputs and check the digest matches. Frame times come from a fixed sequence, so every run prints the
 * same numbers; a real host passes measured times instead.
 *
 * Usage: minimal_host [capsule.json lir.json fixture.json]   (defaults: the examples/ movement capsule,
 *        run from the repository root). Exit status 0 when the rollback check passed. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/clock.h"
#include "sutekh/host.h"

#define FRAMES 288u /* two seconds at 144 Hz */

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void die(const char *what, uint32_t st, const SuHostError *e) {
    fprintf(stderr, "%s: %s: %s\n", what, su_host_status_name(st), e != NULL ? e->message : "");
    exit(1);
}

/* The host's control input for a tick: entity 0 accelerates every 30 ticks. */
static void push_inputs(SuHostWorld *w, uint64_t tick, uint32_t vel_field) {
    SuHostInput in;
    SuHostError e;
    uint64_t ent = 0;
    uint32_t st;
    if (tick % 30u != 0u || su_host_entity_at(w, 0, &ent) != SU_OK) {
        return;
    }
    in.entity = ent;
    in.field_id = vel_field;
    in.value = (uint32_t)(tick / 30u + 1u);
    st = su_host_input_push(w, tick, &in, 1, &e);
    if (st != SU_OK) {
        die("input", st, &e);
    }
}

/* Runs n ticks, one at a time, pushing each tick's inputs first. prev receives position_x before the last. */
static void run_ticks(SuHostWorld *w, uint32_t n, uint32_t pos_field, uint32_t vel_field, uint32_t *prev) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        SuHostError e;
        uint64_t done = 0;
        uint32_t st;
        if (prev != NULL) {
            (void)su_host_read(w, pos_field, 0, 1, prev);
        }
        push_inputs(w, su_host_tick(w), vel_field);
        st = su_host_step(w, 1, &done, &e);
        if (st != SU_OK) {
            die("step", st, &e);
        }
    }
}

int main(int argc, char **argv) {
    const char *cap_path = argc == 4 ? argv[1] : "examples/movement.capsule.json";
    const char *lir_path = argc == 4 ? argv[2] : "examples/movement.lir.json";
    const char *fx_path = argc == 4 ? argv[3] : "examples/world.json";
    SuHostCapsule cap;
    SuHostWorldDesc desc;
    SuHostWorld *w = NULL;
    SuHostClock clock;
    SuHostError e;
    uint32_t pos_field = 0;
    uint32_t vel_field = 0;
    uint32_t prev_pos = 0;
    uint32_t frame;
    uint64_t x = UINT64_C(0x2545F4914F6CDD1D);
    uint8_t *snap = NULL;
    size_t snap_len = 0;
    uint64_t snap_tick = 0;
    uint8_t digest[32];
    uint8_t replayed[32];
    char hex[65];
    uint32_t st;
    int ok;

    memset(&desc, 0, sizeof desc);
    cap.capsule_json = slurp(cap_path, &cap.capsule_len);
    cap.lir_json = slurp(lir_path, &cap.lir_len);
    desc.struct_size = sizeof desc;
    desc.capsule_count = 1;
    desc.capsules = &cap;
    desc.fixture_json = slurp(fx_path, &desc.fixture_len);
    desc.tick_rate_hz = 120;
    st = su_host_world_create(&desc, &w, &e);
    if (st != SU_OK) {
        die("world create", st, &e);
    }
    if (su_host_field_id(w, "position_x", &pos_field) != SU_OK || su_host_field_id(w, "velocity_x", &vel_field) != SU_OK) {
        fprintf(stderr, "this example needs position_x and velocity_x fields\n");
        return 2;
    }
    (void)su_host_clock_init(&clock, su_host_tick_rate(w), 8);

    for (frame = 0; frame < FRAMES; frame++) {
        double alpha = 0.0;
        uint64_t frame_ns;
        uint32_t ticks;
        uint32_t cur = 0;
        /* 144 Hz frames with up to +-1 ms of jitter, from a fixed xorshift sequence. */
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        frame_ns = UINT64_C(6944444) - UINT64_C(1000000) + x % UINT64_C(2000001);
        ticks = su_host_clock_advance(&clock, frame_ns, &alpha);
        run_ticks(w, ticks, pos_field, vel_field, ticks > 0u ? &prev_pos : NULL);
        (void)su_host_read(w, pos_field, 0, 1, &cur);
        if (frame % 48u == 0u) {
            /* What a renderer draws: prev + (cur - prev) * alpha (u32 positions, shown as doubles here). */
            double drawn = (double)prev_pos + ((double)cur - (double)prev_pos) * alpha;
            printf("frame %3u tick %4llu ticks_run %u alpha %.3f entity0 x %.3f\n", (unsigned)frame,
                   (unsigned long long)su_host_tick(w), (unsigned)ticks, alpha, drawn);
        }
        if (frame == FRAMES / 2u) {
            /* Keep a snapshot for the rollback demonstration below. */
            (void)su_host_snapshot_size(w, &snap_len);
            snap = malloc(snap_len);
            if (snap == NULL || su_host_snapshot_write(w, snap, snap_len, &snap_len) != SU_OK) {
                fprintf(stderr, "snapshot failed\n");
                return 1;
            }
            snap_tick = su_host_tick(w);
        }
    }
    (void)su_host_digest(w, digest);
    su_host_hex(digest, hex);
    printf("ticks %llu (dropped %llu) digest %s\n", (unsigned long long)su_host_tick(w),
           (unsigned long long)clock.dropped_ticks, hex);

    /* Rollback: restore the mid-run snapshot and re-simulate the same ticks with the same inputs. */
    {
        uint64_t end_tick = su_host_tick(w);
        st = su_host_snapshot_restore(w, snap, snap_len, &e);
        if (st != SU_OK) {
            die("restore", st, &e);
        }
        run_ticks(w, (uint32_t)(end_tick - snap_tick), pos_field, vel_field, NULL);
        (void)su_host_digest(w, replayed);
        ok = memcmp(digest, replayed, 32) == 0;
        printf("rollback from tick %llu and re-simulation to tick %llu: digest %s\n", (unsigned long long)snap_tick,
               (unsigned long long)end_tick, ok ? "matches" : "DIFFERS");
    }
    free(snap);
    su_host_world_destroy(w);
    free((void *)cap.capsule_json);
    free((void *)cap.lir_json);
    free((void *)desc.fixture_json);
    return ok ? 0 : 1;
}
