/* Block compression for the cook step (sutekh/asset.h, su_asset_bc_encode): BC1, BC3, BC4, BC5. Project-authored;
 * a straightforward encoder, not a quality-tuned one: BC1 endpoints from the principal axis of the block's colours
 * (power iteration), quantized to 5:6:5, nearest-palette indices, then one least-squares refinement of the
 * endpoints kept only when it lowers the block's error; BC4 endpoints are the block's minimum and maximum (8-level
 * mode) with nearest indices. sRGB formats are encoded on the stored (sRGB) bytes, which is what the hardware
 * interpolates. Partial edge blocks repeat the last row and column. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/asset.h"

static uint16_t pack565(const float *c) {
    int r = (int)lroundf(c[0] * 31.0f / 255.0f);
    int g = (int)lroundf(c[1] * 63.0f / 255.0f);
    int b = (int)lroundf(c[2] * 31.0f / 255.0f);
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 63 ? 63 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void unpack565(uint16_t v, int *c) {
    int r = (v >> 11) & 31;
    int g = (v >> 5) & 63;
    int b = v & 31;
    c[0] = (r << 3) | (r >> 2);
    c[1] = (g << 2) | (g >> 4);
    c[2] = (b << 3) | (b >> 2);
}

/* 4-colour palette of two endpoints (c0 > c1 for BC1's opaque mode; BC3 always uses four colours). */
static void palette(uint16_t c0, uint16_t c1, int pal[4][3]) {
    int k;
    unpack565(c0, pal[0]);
    unpack565(c1, pal[1]);
    for (k = 0; k < 3; k++) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
}

/* Indices for the palette; returns the summed squared error. */
static long assign(const uint8_t px[16][4], int pal[4][3], uint32_t *idx) {
    long err = 0;
    int i;
    *idx = 0;
    for (i = 0; i < 16; i++) {
        long best = 0x7FFFFFFF;
        int bi = 0;
        int k;
        for (k = 0; k < 4; k++) {
            long dr = px[i][0] - pal[k][0];
            long dg = px[i][1] - pal[k][1];
            long db = px[i][2] - pal[k][2];
            long d = dr * dr + dg * dg + db * db;
            if (d < best) {
                best = d;
                bi = k;
            }
        }
        err += best;
        *idx |= (uint32_t)bi << (2 * i);
    }
    return err;
}

static void put16(uint8_t *o, uint16_t v) {
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *o, uint32_t v) {
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
    o[2] = (uint8_t)(v >> 16);
    o[3] = (uint8_t)(v >> 24);
}

/* Orders the endpoints for 4-colour mode (c0 > c1), remapping indices when swapped. */
static void emit_bc1(uint8_t out[8], uint16_t c0, uint16_t c1, uint32_t idx) {
    if (c0 < c1) {
        uint16_t t = c0;
        c0 = c1;
        c1 = t;
        idx ^= 0x55555555u; /* 0<->1, 2<->3 */
    } else if (c0 == c1) {
        idx = 0;            /* one colour: every index 0 (in 3-colour mode index 3 would be black) */
    }
    put16(out, c0);
    put16(out + 2, c1);
    put32(out + 4, idx);
}

static void bc1_block(const uint8_t px[16][4], uint8_t out[8]) {
    float mean[3] = {0, 0, 0};
    float cov[6] = {0, 0, 0, 0, 0, 0};
    float axis[3] = {1.0f, 1.0f, 1.0f};
    float lo = INFINITY;
    float hi = -INFINITY;
    float e0[3];
    float e1[3];
    uint16_t c0;
    uint16_t c1;
    uint32_t idx;
    int pal[4][3];
    long err;
    int i;
    int k;
    for (i = 0; i < 16; i++) {
        for (k = 0; k < 3; k++) {
            mean[k] += px[i][k] / 16.0f;
        }
    }
    for (i = 0; i < 16; i++) {
        float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
        cov[0] += d[0] * d[0];
        cov[1] += d[0] * d[1];
        cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1];
        cov[4] += d[1] * d[2];
        cov[5] += d[2] * d[2];
    }
    {
        /* start from the covariance row of largest norm: a fixed start such as (1, 1, 1) can be orthogonal to the
         * principal axis (red rising while green falls) and would collapse the block to its mean */
        float rows[3][3] = {{cov[0], cov[1], cov[2]}, {cov[1], cov[3], cov[4]}, {cov[2], cov[4], cov[5]}};
        float best = -1.0f;
        int r;
        for (r = 0; r < 3; r++) {
            float n = rows[r][0] * rows[r][0] + rows[r][1] * rows[r][1] + rows[r][2] * rows[r][2];
            if (n > best) {
                best = n;
                axis[0] = rows[r][0];
                axis[1] = rows[r][1];
                axis[2] = rows[r][2];
            }
        }
        if (best <= 1e-12f) {
            axis[0] = axis[1] = axis[2] = 1.0f; /* a flat block: any axis */
        }
    }
    for (i = 0; i < 8; i++) {
        float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        float l = sqrtf(x * x + y * y + z * z);
        if (l < 1e-6f) {
            break;
        }
        axis[0] = x / l;
        axis[1] = y / l;
        axis[2] = z / l;
    }
    {
        float l = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        for (k = 0; k < 3; k++) {
            axis[k] /= l;
        }
    }
    for (i = 0; i < 16; i++) {
        float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        lo = t < lo ? t : lo;
        hi = t > hi ? t : hi;
    }
    for (k = 0; k < 3; k++) {
        e0[k] = mean[k] + axis[k] * hi;
        e1[k] = mean[k] + axis[k] * lo;
    }
    c0 = pack565(e0);
    c1 = pack565(e1);
    palette(c0, c1, pal);
    err = assign(px, pal, &idx);
    /* one least-squares refinement of the endpoints for these indices */
    {
        static const float w0[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        float aa = 0.0f;
        float bb = 0.0f;
        float ab = 0.0f;
        float ax[3] = {0, 0, 0};
        float bx[3] = {0, 0, 0};
        float det;
        for (i = 0; i < 16; i++) {
            float a = w0[(idx >> (2 * i)) & 3u];
            float b = 1.0f - a;
            aa += a * a;
            bb += b * b;
            ab += a * b;
            for (k = 0; k < 3; k++) {
                ax[k] += a * px[i][k];
                bx[k] += b * px[i][k];
            }
        }
        det = aa * bb - ab * ab;
        if (fabsf(det) > 1e-6f) {
            float r0[3];
            float r1[3];
            uint16_t d0;
            uint16_t d1;
            uint32_t idx2;
            int pal2[4][3];
            long err2;
            for (k = 0; k < 3; k++) {
                r0[k] = (ax[k] * bb - bx[k] * ab) / det;
                r1[k] = (bx[k] * aa - ax[k] * ab) / det;
            }
            d0 = pack565(r0);
            d1 = pack565(r1);
            palette(d0, d1, pal2);
            err2 = assign(px, pal2, &idx2);
            if (err2 < err) {
                c0 = d0;
                c1 = d1;
                idx = idx2;
            }
        }
    }
    emit_bc1(out, c0, c1, idx);
}

/* BC4 on channel ch of the block (8 bytes). */
static void bc4_block(const uint8_t px[16][4], int ch, uint8_t out[8]) {
    int lo = 255;
    int hi = 0;
    int pal[8];
    uint64_t bits = 0;
    int i;
    int k;
    for (i = 0; i < 16; i++) {
        lo = px[i][ch] < lo ? px[i][ch] : lo;
        hi = px[i][ch] > hi ? px[i][ch] : hi;
    }
    out[0] = (uint8_t)hi;
    out[1] = (uint8_t)lo;
    if (hi == lo) {
        memset(out + 2, 0, 6);
        return;
    }
    pal[0] = hi;
    pal[1] = lo;
    for (k = 1; k < 7; k++) {
        pal[k + 1] = ((7 - k) * hi + k * lo) / 7;
    }
    for (i = 0; i < 16; i++) {
        int best = 0x7FFFFFFF;
        int bi = 0;
        for (k = 0; k < 8; k++) {
            int d = abs(px[i][ch] - pal[k]);
            if (d < best) {
                best = d;
                bi = k;
            }
        }
        bits |= (uint64_t)bi << (3 * i);
    }
    for (i = 0; i < 6; i++) {
        out[2 + i] = (uint8_t)(bits >> (8 * i));
    }
}

uint32_t su_asset_bc_encode(uint32_t format, const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *out) {
    uint32_t bx;
    uint32_t by;
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    size_t block = format == SU_TEX_BC1_UNORM || format == SU_TEX_BC1_SRGB || format == SU_TEX_BC4_UNORM ? 8u : 16u;
    if (rgba == NULL || out == NULL || width == 0u || height == 0u ||
        (format != SU_TEX_BC1_UNORM && format != SU_TEX_BC1_SRGB && format != SU_TEX_BC3_UNORM &&
         format != SU_TEX_BC3_SRGB && format != SU_TEX_BC4_UNORM && format != SU_TEX_BC5_UNORM)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            uint8_t *o = out + ((size_t)by * bw + bx) * block;
            int i;
            for (i = 0; i < 16; i++) {
                uint32_t x = bx * 4u + (uint32_t)(i & 3);
                uint32_t y = by * 4u + (uint32_t)(i >> 2);
                x = x < width ? x : width - 1u;
                y = y < height ? y : height - 1u;
                memcpy(px[i], rgba + ((size_t)y * width + x) * 4u, 4);
            }
            switch (format) {
            case SU_TEX_BC1_UNORM:
            case SU_TEX_BC1_SRGB:
                bc1_block(px, o);
                break;
            case SU_TEX_BC3_UNORM:
            case SU_TEX_BC3_SRGB:
                bc4_block(px, 3, o);
                bc1_block(px, o + 8);
                break;
            case SU_TEX_BC4_UNORM:
                bc4_block(px, 0, o);
                break;
            default:
                bc4_block(px, 0, o);
                bc4_block(px, 1, o + 8);
                break;
            }
        }
    }
    return SU_OK;
}
