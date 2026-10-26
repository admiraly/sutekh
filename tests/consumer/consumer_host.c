/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
/* A consumer of Sutekh::host (static library, public headers only): creates a world from the repository's movement
 * example, runs 120 ticks, snapshots at tick 60, restores and re-simulates, and checks the digests match.
 * Usage: consumer_host SUTEKH_ROOT */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/host.h"

static uint8_t *slurp(const char *root, const char *rel, size_t *len) {
    char path[4096];
    FILE *f;
    uint8_t *b;
    long n;
    if (snprintf(path, sizeof path, "%s/%s", root, rel) >= (int)sizeof path || (f = fopen(path, "rb")) == NULL) {
        fprintf(stderr, "cannot open %s/%s\n", root, rel);
        exit(2);
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        (b = malloc((size_t)n + 1u)) == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int step(SuHostWorld *w, uint64_t ticks) {
    SuHostError e;
    uint64_t done = 0;
    uint32_t st = su_host_step(w, ticks, &done, &e);
    if (st != SU_OK || done != ticks) {
        fprintf(stderr, "step: %s: %s\n", su_host_status_name(st), e.message);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    SuHostCapsule cap;
    SuHostWorldDesc desc;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint8_t *snap = NULL;
    size_t snap_len = 0;
    uint8_t digest[32];
    uint8_t replayed[32];
    char hex[65];
    uint32_t st;
    int ok;

    if (argc != 2) {
        fprintf(stderr, "usage: consumer_host SUTEKH_ROOT\n");
        return 2;
    }
    memset(&desc, 0, sizeof desc);
    cap.capsule_json = slurp(argv[1], "examples/movement.capsule.json", &cap.capsule_len);
    cap.lir_json = slurp(argv[1], "examples/movement.lir.json", &cap.lir_len);
    desc.struct_size = sizeof desc;
    desc.capsule_count = 1;
    desc.capsules = &cap;
    desc.fixture_json = slurp(argv[1], "examples/world.json", &desc.fixture_len);
    desc.tick_rate_hz = 60;
    st = su_host_world_create(&desc, &w, &e);
    if (st != SU_OK) {
        fprintf(stderr, "world create: %s: %s\n", su_host_status_name(st), e.message);
        return 1;
    }
    ok = step(w, 60);
    ok = ok && su_host_snapshot_size(w, &snap_len) == SU_OK && (snap = malloc(snap_len)) != NULL &&
         su_host_snapshot_write(w, snap, snap_len, &snap_len) == SU_OK;
    ok = ok && step(w, 60) && su_host_digest(w, digest) == SU_OK;
    ok = ok && su_host_snapshot_restore(w, snap, snap_len, &e) == SU_OK && step(w, 60) &&
         su_host_digest(w, replayed) == SU_OK && memcmp(digest, replayed, sizeof digest) == 0;
    ok = ok && su_host_tick(w) == 120u;
    su_host_hex(digest, hex);
    printf("host api %u, schema %s, tick %llu, digest %s, rollback %s\n", (unsigned)su_host_api_version(),
           su_host_schema_id(w), (unsigned long long)su_host_tick(w), hex, ok ? "matches" : "FAILED");
    free(snap);
    su_host_world_destroy(w);
    free((void *)cap.capsule_json);
    free((void *)cap.lir_json);
    free((void *)desc.fixture_json);
    return ok ? 0 : 1;
}
