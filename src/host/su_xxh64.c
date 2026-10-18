/* XXH64 from the published algorithm description. See su_xxh64.h. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "host/su_xxh64.h"

#define P1 UINT64_C(0x9E3779B185EBCA87)
#define P2 UINT64_C(0xC2B2AE3D27D4EB4F)
#define P3 UINT64_C(0x165667B19E3779F9)
#define P4 UINT64_C(0x85EBCA77C2B2AE63)
#define P5 UINT64_C(0x27D4EB2F165667C5)

static uint64_t rotl(uint64_t x, unsigned r) {
    return (x << r) | (x >> (64u - r));
}

/* Little-endian loads, independent of host byte order and alignment. */
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t round64(uint64_t acc, uint64_t in) {
    acc += in * P2;
    acc = rotl(acc, 31);
    return acc * P1;
}

static uint64_t merge64(uint64_t acc, uint64_t v) {
    acc ^= round64(0, v);
    return acc * P1 + P4;
}

uint64_t su_xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = data;
    const uint8_t *end = p + len;
    uint64_t h;

    if (len >= 32u) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - P1;
        do {
            v1 = round64(v1, rd64(p));
            v2 = round64(v2, rd64(p + 8));
            v3 = round64(v3, rd64(p + 16));
            v4 = round64(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge64(h, v1);
        h = merge64(h, v2);
        h = merge64(h, v3);
        h = merge64(h, v4);
    } else {
        h = seed + P5;
    }
    h += (uint64_t)len;
    while (end - p >= 8) {
        h ^= round64(0, rd64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (uint64_t)rd32(p) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p) * P5;
        h = rotl(h, 11) * P1;
        p++;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}
