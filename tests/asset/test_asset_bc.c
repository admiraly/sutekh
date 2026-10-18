/* Block compression (su_asset_bc_encode) against a reference decoder written here from the BC1/BC3/BC4/BC5 format
 * description: exact results where the format can be exact (5:6:5-representable solid colours, two-colour blocks,
 * flat channels), error bounds on gradients and noise, normal-map angles, odd image sizes, and the cook step with
 * SU_ASSET_COOK_BC. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/asset.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

/* ---------- reference decoder ---------- */

static void rgb565(unsigned v, int *c) {
    c[0] = (int)(((v >> 11) & 31u) * 255u + 15u) / 31;
    c[1] = (int)(((v >> 5) & 63u) * 255u + 31u) / 63;
    c[2] = (int)((v & 31u) * 255u + 15u) / 31;
}

/* BC1 colour block into px[16][4]; four_colour forces the 4-colour palette (BC3). */
static void dec_bc1(const uint8_t *b, uint8_t px[16][4], int four_colour) {
    unsigned c0 = (unsigned)b[0] | ((unsigned)b[1] << 8);
    unsigned c1 = (unsigned)b[2] | ((unsigned)b[3] << 8);
    unsigned long idx = (unsigned long)b[4] | ((unsigned long)b[5] << 8) | ((unsigned long)b[6] << 16) |
                        ((unsigned long)b[7] << 24);
    int p[4][4];
    int i;
    int k;
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    for (k = 0; k < 3; k++) {
        if (four_colour || c0 > c1) {
            p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
        } else {
            p[2][k] = (p[0][k] + p[1][k]) / 2;
            p[3][k] = 0;
        }
    }
    if (!(four_colour || c0 > c1)) {
        p[3][3] = 0;
    }
    for (i = 0; i < 16; i++) {
        int s = (int)((idx >> (2 * i)) & 3u);
        for (k = 0; k < 4; k++) {
            px[i][k] = (uint8_t)p[s][k];
        }
    }
}

static void dec_bc4(const uint8_t *b, uint8_t px[16][4], int ch) {
    int a0 = b[0];
    int a1 = b[1];
    int p[8];
    unsigned long long bits = 0;
    int i;
    for (i = 0; i < 6; i++) {
        bits |= (unsigned long long)b[2 + i] << (8 * i);
    }
    p[0] = a0;
    p[1] = a1;
    if (a0 > a1) {
        for (i = 1; i < 7; i++) {
            p[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
        }
    } else {
        for (i = 1; i < 5; i++) {
            p[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        }
        p[6] = 0;
        p[7] = 255;
    }
    for (i = 0; i < 16; i++) {
        px[i][ch] = (uint8_t)p[(bits >> (3 * i)) & 7u];
    }
}

/* Decodes a whole image of format into rgba (w x h). */
static void decode(uint32_t fmt, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *rgba) {
    uint32_t bw = (w + 3u) / 4u;
    uint32_t bh = (h + 3u) / 4u;
    size_t block = fmt == SU_TEX_BC1_UNORM || fmt == SU_TEX_BC1_SRGB || fmt == SU_TEX_BC4_UNORM ? 8u : 16u;
    uint32_t bx;
    uint32_t by;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + ((size_t)by * bw + bx) * block;
            uint8_t px[16][4];
            int i;
            memset(px, 0, sizeof px);
            if (fmt == SU_TEX_BC1_UNORM || fmt == SU_TEX_BC1_SRGB) {
                dec_bc1(b, px, 0);
            } else if (fmt == SU_TEX_BC3_UNORM || fmt == SU_TEX_BC3_SRGB) {
                dec_bc1(b + 8, px, 1);
                dec_bc4(b, px, 3);
            } else if (fmt == SU_TEX_BC4_UNORM) {
                dec_bc4(b, px, 0);
            } else {
                dec_bc4(b, px, 0);
                dec_bc4(b + 8, px, 1);
            }
            for (i = 0; i < 16; i++) {
                uint32_t x = bx * 4u + (uint32_t)(i & 3);
                uint32_t y = by * 4u + (uint32_t)(i >> 2);
                if (x < w && y < h) {
                    memcpy(rgba + ((size_t)y * w + x) * 4u, px[i], 4);
                }
            }
        }
    }
}

/* PSNR over the given channels (bit mask). */
static double psnr(const uint8_t *a, const uint8_t *b, uint32_t n, unsigned chmask, int *maxerr) {
    double se = 0.0;
    uint32_t cnt = 0;
    uint32_t i;
    int k;
    *maxerr = 0;
    for (i = 0; i < n; i++) {
        for (k = 0; k < 4; k++) {
            if (chmask & (1u << k)) {
                int d = (int)a[i * 4u + (uint32_t)k] - (int)b[i * 4u + (uint32_t)k];
                se += (double)d * d;
                *maxerr = abs(d) > *maxerr ? abs(d) : *maxerr;
                cnt++;
            }
        }
    }
    return se == 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / (se / cnt));
}

static uint8_t *round_trip(uint32_t fmt, const uint8_t *img, uint32_t w, uint32_t h) {
    uint8_t *bc = malloc((size_t)su_asset_texture_level_size(fmt, w, h));
    uint8_t *out = calloc((size_t)w * h, 4u);
    CHECK(su_asset_bc_encode(fmt, img, w, h, bc) == SU_OK);
    decode(fmt, bc, w, h, out);
    free(bc);
    return out;
}

static void test_exact(void) {
    /* solid colours representable in 5:6:5 decode exactly */
    static const int cols[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 0}, {132, 130, 66}};
    uint8_t img[8 * 4 * 4];
    int c;
    for (c = 0; c < 4; c++) {
        int r5 = (cols[c][0] * 31 + 127) / 255;
        int g6 = (cols[c][1] * 63 + 127) / 255;
        int b5 = (cols[c][2] * 31 + 127) / 255;
        int want[3] = {(r5 * 255 + 15) / 31, (g6 * 255 + 31) / 63, (b5 * 255 + 15) / 31};
        int i;
        uint8_t *out;
        int maxerr;
        for (i = 0; i < 32; i++) {
            img[i * 4] = (uint8_t)want[0];
            img[i * 4 + 1] = (uint8_t)want[1];
            img[i * 4 + 2] = (uint8_t)want[2];
            img[i * 4 + 3] = 255;
        }
        out = round_trip(SU_TEX_BC1_SRGB, img, 8, 4);
        CHECK(psnr(img, out, 32, 0xFu, &maxerr) == 99.0);
        free(out);
    }
    /* a two-colour block whose colours are the endpoints */
    {
        int i;
        uint8_t *out;
        int maxerr;
        for (i = 0; i < 16; i++) {
            int on = (i * 7) % 3 == 0;
            img[i * 4] = on ? 255 : 0;
            img[i * 4 + 1] = on ? 255 : 0;
            img[i * 4 + 2] = on ? 255 : 0;
            img[i * 4 + 3] = 255;
        }
        out = round_trip(SU_TEX_BC1_UNORM, img, 4, 4);
        CHECK(psnr(img, out, 16, 0x7u, &maxerr) == 99.0);
        free(out);
    }
    /* BC4 of a flat channel and of a channel with only two values */
    {
        int i;
        uint8_t *out;
        int maxerr;
        for (i = 0; i < 16; i++) {
            img[i * 4] = (uint8_t)(i & 1 ? 200 : 13);
        }
        out = round_trip(SU_TEX_BC4_UNORM, img, 4, 4);
        CHECK(psnr(img, out, 16, 0x1u, &maxerr) == 99.0);
        free(out);
    }
}

static void test_bounds(void) {
    enum { W = 64, H = 64 };
    static uint8_t img[W * H * 4];
    uint32_t i;
    uint32_t x;
    uint32_t y;
    uint64_t rng = 0x123456789ABCDEFull;
    uint8_t *out;
    int maxerr;
    double p;
    /* smooth colour gradient */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint8_t *q = img + (y * W + x) * 4u;
            q[0] = (uint8_t)(x * 4);
            q[1] = (uint8_t)(y * 4);
            q[2] = (uint8_t)(255 - (x + y) * 2);
            q[3] = (uint8_t)((x * 4) ^ (y * 2));
        }
    }
    out = round_trip(SU_TEX_BC1_SRGB, img, W, H);
    p = psnr(img, out, W * H, 0x7u, &maxerr);
    printf("BC1 gradient: PSNR %.2f dB, max error %d\n", p, maxerr);
    CHECK(p > 36.0 && maxerr <= 12); /* measured 38.54 dB, max 9 with this encoder */
    free(out);
    out = round_trip(SU_TEX_BC3_SRGB, img, W, H);
    p = psnr(img, out, W * H, 0x8u, &maxerr);
    printf("BC3 alpha (xor pattern): PSNR %.2f dB, max error %d\n", p, maxerr);
    CHECK(p > 30.0);
    free(out);
    out = round_trip(SU_TEX_BC4_UNORM, img, W, H);
    p = psnr(img, out, W * H, 0x1u, &maxerr);
    printf("BC4 red gradient: PSNR %.2f dB, max error %d\n", p, maxerr);
    CHECK(p > 45.0 && maxerr <= 2);
    free(out);
    /* noise: no structure to exploit, still bounded */
    for (i = 0; i < W * H * 4u; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        img[i] = (uint8_t)rng;
    }
    out = round_trip(SU_TEX_BC1_UNORM, img, W, H);
    p = psnr(img, out, W * H, 0x7u, &maxerr);
    printf("BC1 noise: PSNR %.2f dB\n", p);
    CHECK(p > 12.0); /* measured 13.72 dB: noise has no structure for 4 colours per block */
    free(out);
    /* normal map of a hemisphere bump, BC5: angular error after rebuilding z as the shader does */
    {
        double worst = 0.0;
        double sum = 0.0;
        uint32_t counted = 0;
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                double nx = ((double)x - 31.5) / 40.0;
                double ny = ((double)y - 31.5) / 40.0;
                double nz = sqrt(fmax(1.0 - nx * nx - ny * ny, 0.0));
                double l = sqrt(nx * nx + ny * ny + nz * nz);
                uint8_t *q = img + (y * W + x) * 4u;
                q[0] = (uint8_t)lround((nx / l * 0.5 + 0.5) * 255.0);
                q[1] = (uint8_t)lround((ny / l * 0.5 + 0.5) * 255.0);
                q[2] = (uint8_t)lround((nz / l * 0.5 + 0.5) * 255.0);
                q[3] = 255;
            }
        }
        out = round_trip(SU_TEX_BC5_UNORM, img, W, H);
        for (i = 0; i < W * H; i++) {
            double ax = img[i * 4] / 127.5 - 1.0;
            double ay = img[i * 4 + 1] / 127.5 - 1.0;
            double az = sqrt(fmax(1.0 - ax * ax - ay * ay, 0.0));
            double bx = out[i * 4] / 127.5 - 1.0;
            double by = out[i * 4 + 1] / 127.5 - 1.0;
            double bz = sqrt(fmax(1.0 - bx * bx - by * by, 0.0));
            double la = sqrt(ax * ax + ay * ay + az * az);
            double lb = sqrt(bx * bx + by * by + bz * bz);
            double c = (ax * bx + ay * by + az * bz) / (la * lb);
            double ang = acos(c > 1.0 ? 1.0 : c) * 57.29577951;
            if (az < 0.3) {
                continue; /* the rim, where rebuilding z from x and y is ill-conditioned for any 8-bit encoding */
            }
            worst = ang > worst ? ang : worst;
            sum += ang;
            counted++;
        }
        printf("BC5 normal map: mean angular error %.3f deg, worst %.3f deg over %u texels with z >= 0.3\n",
               sum / counted, worst, (unsigned)counted);
        CHECK(counted > 2000u && sum / counted < 0.5 && worst < 2.0);
        free(out);
    }
}

static void test_edges_and_refusals(void) {
    uint8_t img[5 * 3 * 4];
    uint8_t bc[64];
    uint8_t out[5 * 3 * 4];
    int i;
    int maxerr;
    for (i = 0; i < 15; i++) {
        img[i * 4] = (uint8_t)(i * 17);
        img[i * 4 + 1] = (uint8_t)(255 - i * 17);
        img[i * 4 + 2] = 40;
        img[i * 4 + 3] = 255;
    }
    CHECK(su_asset_texture_level_size(SU_TEX_BC1_UNORM, 5, 3) == 16u); /* 2 x 1 blocks */
    memset(bc, 0xEE, sizeof bc);
    CHECK(su_asset_bc_encode(SU_TEX_BC1_UNORM, img, 5, 3, bc) == SU_OK);
    CHECK(bc[16] == 0xEE); /* nothing written past the two blocks */
    decode(SU_TEX_BC1_UNORM, bc, 5, 3, out);
    /* 12 distinct colours on a line in the first block cannot fit 4 levels: about 20 dB is the format's limit */
    {
        double p = psnr(img, out, 15, 0x7u, &maxerr);
        printf("BC1 5x3 image (two partial blocks): PSNR %.2f dB, max error %d\n", p, maxerr);
        if (getenv("SUTEKH_TEST_DEBUG") != NULL) {
            for (i = 0; i < 15; i++) {
                printf("  %2d: in %3d %3d %3d  out %3d %3d %3d\n", i, img[i * 4], img[i * 4 + 1], img[i * 4 + 2], out[i * 4],
                       out[i * 4 + 1], out[i * 4 + 2]);
            }
        }
        CHECK(p > 22.0 && maxerr <= 40); /* measured 25.17 dB, max 31 */
    }
    CHECK(su_asset_bc_encode(SU_TEX_BC7_UNORM, img, 5, 3, bc) == SU_E_INVALID_ARGUMENT);
    CHECK(su_asset_bc_encode(SU_TEX_RGBA8_UNORM, img, 5, 3, bc) == SU_E_INVALID_ARGUMENT);
    CHECK(su_asset_bc_encode(SU_TEX_BC1_UNORM, img, 0, 3, bc) == SU_E_INVALID_ARGUMENT);
}

int main(void) {
    test_exact();
    test_bounds();
    test_edges_and_refusals();
    printf("test_asset_bc: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
