/* CPU decoders for the LDR block formats BC1..BC5 and BC7 (sutekh/dds.h, su_bc_decode_block). Headless: no GPU, no
 * allocation. Written from the D3D11 block compression specification: BC1 to BC5 interpolate the 8-bit expanded
 * endpoints with integer arithmetic (hardware rounding of the BC1 thirds may differ by one or two codes); BC7 is
 * bit-exact (6-bit weights, (a * (64 - w) + b * w + 32) >> 6). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/dds.h"

#include <string.h>

/* ---------- BC1 .. BC5 ---------- */

static void rgb565(uint32_t v, int c[3]) {
    int r = (int)((v >> 11) & 31u);
    int g = (int)((v >> 5) & 63u);
    int b = (int)(v & 31u);
    c[0] = (r << 3) | (r >> 2);
    c[1] = (g << 2) | (g >> 4);
    c[2] = (b << 3) | (b >> 2);
}

/* Colour part of BC1/2/3: 8 bytes. four: always the four-colour mode (BC2, BC3). */
static void color_block(const uint8_t *b, int four, uint8_t out[64]) {
    uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    uint32_t c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    uint32_t idx = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    int pal[4][4];
    int e0[3];
    int e1[3];
    int k;
    uint32_t t;
    rgb565(c0, e0);
    rgb565(c1, e1);
    for (k = 0; k < 3; k++) {
        pal[0][k] = e0[k];
        pal[1][k] = e1[k];
        if (four || c0 > c1) {
            pal[2][k] = (2 * e0[k] + e1[k] + 1) / 3;
            pal[3][k] = (e0[k] + 2 * e1[k] + 1) / 3;
        } else {
            pal[2][k] = (e0[k] + e1[k] + 1) / 2;
            pal[3][k] = 0;
        }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = (four || c0 > c1) ? 255 : 0;
    for (t = 0; t < 16u; t++) {
        uint32_t s = (idx >> (2u * t)) & 3u;
        for (k = 0; k < 4; k++) {
            out[t * 4u + (uint32_t)k] = (uint8_t)pal[s][k];
        }
    }
}

/* BC4-style 8-byte channel block into channel ch of out. */
static void channel_block(const uint8_t *b, uint32_t ch, uint8_t out[64]) {
    int a0 = b[0];
    int a1 = b[1];
    int pal[8];
    uint64_t idx = 0;
    uint32_t t;
    int i;
    for (i = 0; i < 6; i++) {
        idx |= (uint64_t)b[2 + i] << (8 * i);
    }
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (i = 1; i < 7; i++) {
            pal[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
        }
    } else {
        for (i = 1; i < 5; i++) {
            pal[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        }
        pal[6] = 0;
        pal[7] = 255;
    }
    for (t = 0; t < 16u; t++) {
        out[t * 4u + ch] = (uint8_t)pal[(idx >> (3u * t)) & 7u];
    }
}

/* ---------- BC7 ---------- */

typedef struct Bc7Mode {
    uint8_t subsets, partition_bits, rotation_bits, index_sel_bits, color_bits, alpha_bits, endpoint_pbits, shared_pbits,
        index_bits, index_bits2;
} Bc7Mode;

static const Bc7Mode bc7_modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

/* Two-subset shapes: bit t = subset of texel t. */
static const uint16_t bc7_p2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
/* Three-subset shapes: 2 bits per texel, texel t at bits 2t. */
static const uint32_t bc7_p3[64] = {
    0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
    0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
    0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
    0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
    0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
    0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
    0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
    0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u};
static const uint8_t bc7_anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                        15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                        15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                        6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
static const uint8_t bc7_anchor3a[64] = {3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                                         3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                         8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                         3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
static const uint8_t bc7_anchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                         15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                         15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                         15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

static const int bc7_w2[4] = {0, 21, 43, 64};
static const int bc7_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const int bc7_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct Bc7Bits {
    const uint8_t *b;
    uint32_t pos;
} Bc7Bits;

static uint32_t take(Bc7Bits *s, uint32_t n) {
    uint32_t v = 0;
    uint32_t i;
    for (i = 0; i < n; i++, s->pos++) {
        v |= (uint32_t)((s->b[s->pos >> 3] >> (s->pos & 7u)) & 1u) << i;
    }
    return v;
}

static const int *weights_of(uint32_t bits) {
    return bits == 2u ? bc7_w2 : bits == 3u ? bc7_w3 : bc7_w4;
}

static uint32_t bc7_block(const uint8_t b[16], uint8_t out[64]) {
    Bc7Bits s;
    const Bc7Mode *md;
    uint32_t mode = 0;
    uint32_t partition;
    uint32_t rotation;
    uint32_t index_sel;
    uint32_t ns;
    uint32_t e;
    uint32_t c;
    uint32_t t;
    int ep[6][4];
    uint8_t subset_of[16];
    uint8_t idx1[16];
    uint8_t idx2[16];
    while (mode < 8u && ((b[0] >> mode) & 1u) == 0u) {
        mode++;
    }
    if (mode == 8u) {
        memset(out, 0, 64);
        return 0;
    }
    md = &bc7_modes[mode];
    s.b = b;
    s.pos = mode + 1u;
    ns = md->subsets;
    partition = take(&s, md->partition_bits);
    rotation = take(&s, md->rotation_bits);
    index_sel = take(&s, md->index_sel_bits);
    memset(ep, 0, sizeof ep);
    for (c = 0; c < 3u; c++) {
        for (e = 0; e < ns * 2u; e++) {
            ep[e][c] = (int)take(&s, md->color_bits);
        }
    }
    for (e = 0; e < ns * 2u; e++) {
        ep[e][3] = md->alpha_bits != 0u ? (int)take(&s, md->alpha_bits) : 255;
    }
    {
        uint32_t cb = md->color_bits;
        uint32_t ab = md->alpha_bits;
        uint32_t pb[6] = {0, 0, 0, 0, 0, 0};
        if (md->endpoint_pbits != 0u) {
            for (e = 0; e < ns * 2u; e++) {
                pb[e] = take(&s, 1);
            }
            cb++;
            if (ab != 0u) {
                ab++;
            }
        } else if (md->shared_pbits != 0u) {
            for (e = 0; e < ns; e++) {
                pb[e * 2u] = pb[e * 2u + 1u] = take(&s, 1);
            }
            cb++;
            if (ab != 0u) {
                ab++;
            }
        }
        for (e = 0; e < ns * 2u; e++) {
            for (c = 0; c < 4u; c++) {
                uint32_t bits = c < 3u ? cb : ab;
                int v = ep[e][c];
                if (c == 3u && md->alpha_bits == 0u) {
                    continue;
                }
                if (md->endpoint_pbits != 0u || md->shared_pbits != 0u) {
                    v = (v << 1) | (int)pb[e];
                }
                v <<= 8u - bits;
                v |= v >> bits;
                ep[e][c] = v & 255;
            }
        }
    }
    for (t = 0; t < 16u; t++) {
        if (ns == 1u) {
            subset_of[t] = 0;
        } else if (ns == 2u) {
            subset_of[t] = (uint8_t)((bc7_p2[partition] >> t) & 1u);
        } else {
            subset_of[t] = (uint8_t)((bc7_p3[partition] >> (2u * t)) & 3u);
        }
    }
    for (t = 0; t < 16u; t++) {
        uint32_t bits = md->index_bits;
        int anchor = t == 0u || (ns == 2u && t == bc7_anchor2[partition]) ||
                     (ns == 3u && (t == bc7_anchor3a[partition] || t == bc7_anchor3b[partition]));
        idx1[t] = (uint8_t)take(&s, anchor ? bits - 1u : bits);
    }
    if (md->index_bits2 != 0u) {
        for (t = 0; t < 16u; t++) {
            uint32_t bits = md->index_bits2;
            idx2[t] = (uint8_t)take(&s, t == 0u ? bits - 1u : bits);
        }
    }
    for (t = 0; t < 16u; t++) {
        const int *a = ep[subset_of[t] * 2u];
        const int *z = ep[subset_of[t] * 2u + 1u];
        int px[4];
        uint32_t cbits = md->index_bits;
        uint32_t abits = md->index_bits;
        uint32_t ci = idx1[t];
        uint32_t ai = idx1[t];
        if (md->index_bits2 != 0u) {
            if (index_sel == 0u) {
                abits = md->index_bits2;
                ai = idx2[t];
            } else {
                cbits = md->index_bits2;
                ci = idx2[t];
            }
        }
        for (c = 0; c < 4u; c++) {
            int w = c < 3u ? weights_of(cbits)[ci] : weights_of(abits)[ai];
            px[c] = (a[c] * (64 - w) + z[c] * w + 32) >> 6;
        }
        if (md->alpha_bits == 0u) {
            px[3] = 255;
        }
        if (rotation != 0u) {
            int tmp = px[3];
            px[3] = px[rotation - 1u];
            px[rotation - 1u] = tmp;
        }
        for (c = 0; c < 4u; c++) {
            out[t * 4u + c] = (uint8_t)px[c];
        }
    }
    return 1;
}

uint32_t su_bc_decode_block(uint32_t format, const uint8_t *block, uint8_t out[64]) {
    uint32_t t;
    if (block == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    switch (format) {
    case SU_TEX_BC1_UNORM:
    case SU_TEX_BC1_SRGB:
        color_block(block, 0, out);
        return SU_OK;
    case SU_TEX_BC2_UNORM:
    case SU_TEX_BC2_SRGB:
        color_block(block + 8, 1, out);
        for (t = 0; t < 16u; t++) {
            uint32_t a = (block[t >> 1] >> (4u * (t & 1u))) & 15u;
            out[t * 4u + 3u] = (uint8_t)(a * 17u);
        }
        return SU_OK;
    case SU_TEX_BC3_UNORM:
    case SU_TEX_BC3_SRGB:
        color_block(block + 8, 1, out);
        channel_block(block, 3, out);
        return SU_OK;
    case SU_TEX_BC4_UNORM:
        memset(out, 0, 64);
        channel_block(block, 0, out);
        for (t = 0; t < 16u; t++) {
            out[t * 4u + 3u] = 255;
        }
        return SU_OK;
    case SU_TEX_BC5_UNORM:
        memset(out, 0, 64);
        channel_block(block, 0, out);
        channel_block(block + 8, 1, out);
        for (t = 0; t < 16u; t++) {
            out[t * 4u + 3u] = 255;
        }
        return SU_OK;
    case SU_TEX_BC7_UNORM:
    case SU_TEX_BC7_SRGB:
        (void)bc7_block(block, out);
        return SU_OK;
    default:
        return SU_E_UNSUPPORTED_METHOD;
    }
}
