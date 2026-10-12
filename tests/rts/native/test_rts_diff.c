/* RTS host-operator differential driver (M3-04). Reads a scene dump written by tests/rts/native/diff_driver.py,
 * spawns the rows in a 39-field rts world, despawns the dead rows, runs one tick through the real scheduler
 * (rts.spatial, rts.scatter), and writes the committed outputs of the two operators per slot.
 *
 * Input (text): line 1 "cap C"; then C lines, one per slot: "alive v1 ... v39" (values in field id order).
 * Output (text): C lines, one per slot: anchor_x anchor_y anchor_valid target_slot target_gen target_dist_sq
 *                target_valid target_x target_y truncated incoming.
 *
 * Usage: test_rts_diff <input> <output> */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rts_fixture.h"

static int fail(const char *msg) {
    fprintf(stderr, "test_rts_diff: %s\n", msg);
    return 2;
}

int main(int argc, char **argv) {
    FILE *in;
    FILE *out;
    unsigned long cap_ul = 0;
    uint32_t cap;
    uint32_t s;
    SuWorld *w;
    SuSchedule sched;
    RtsRow *rows;
    uint8_t *alive;
    uint32_t st;
    uint32_t dead_count = 0;

    if (argc != 3) {
        return fail("usage: test_rts_diff <input> <output>");
    }
    in = fopen(argv[1], "r");
    if (in == NULL) {
        return fail("cannot open input");
    }
    if (fscanf(in, " cap %lu", &cap_ul) != 1 || cap_ul == 0u || cap_ul > RTS_CAP_MAX_TEST) {
        fclose(in);
        return fail("bad capacity line");
    }
    cap = (uint32_t)cap_ul;
    rows = calloc(cap, sizeof(RtsRow));
    alive = calloc(cap, 1u);
    if (rows == NULL || alive == NULL) {
        return fail("allocation");
    }
    for (s = 0; s < cap; s++) {
        unsigned int a = 0;
        uint32_t id;
        if (fscanf(in, "%u", &a) != 1) {
            return fail("short input");
        }
        alive[s] = a ? 1u : 0u;
        for (id = 1; id <= RTS_F_COUNT; id++) {
            unsigned int v = 0;
            if (fscanf(in, "%u", &v) != 1) {
                return fail("short row");
            }
            rows[s].v[id] = (uint32_t)v;
        }
    }
    fclose(in);

    w = rts_world_create(cap);
    if (w == NULL) {
        return fail("world create");
    }
    for (s = 0; s < cap; s++) {
        SuEntity h = 0;
        if (rts_spawn(w, &rows[s], &h) != SU_OK || (uint32_t)(h & 0xFFFFFFFFu) != s) {
            return fail("spawn order");
        }
    }
    for (s = 0; s < cap; s++) {
        if (!alive[s]) {
            uint32_t gen = 0;
            if (su_world_generation_view(w, s, 1u, &gen) != SU_OK ||
                su_world_despawn(w, ((uint64_t)gen << 32) | s) != SU_OK) {
                return fail("despawn");
            }
            dead_count++;
        }
    }
    st = rts_schedule_build(&sched);
    if (st != SU_OK) {
        return fail("schedule build");
    }
    st = rts_tick(w, &sched);
    if (st != SU_OK) {
        fprintf(stderr, "test_rts_diff: tick status %u\n", st);
        return 1;
    }

    out = fopen(argv[2], "w");
    if (out == NULL) {
        return fail("cannot open output");
    }
    for (s = 0; s < cap; s++) {
        fprintf(out, "%u %u %u %u %u %u %u %u %u %u %u\n", rts_read(w, F_ANCHOR_X, s), rts_read(w, F_ANCHOR_Y, s),
                rts_read(w, F_ANCHOR_VALID, s), rts_read(w, F_TARGET_SLOT, s), rts_read(w, F_TARGET_GEN, s),
                rts_read(w, F_TARGET_DIST, s), rts_read(w, F_TARGET_VALID, s), rts_read(w, F_TARGET_X, s),
                rts_read(w, F_TARGET_Y, s), rts_read(w, F_TRUNCATED, s), rts_read(w, F_INCOMING, s));
    }
    fclose(out);
    printf("test_rts_diff: %u rows, %u dead, tick ok\n", cap, dead_count);
    free(rows);
    free(alive);
    su_sched_free(&sched);
    su_world_destroy(w);
    return 0;
}
