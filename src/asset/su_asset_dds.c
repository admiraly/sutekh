/* DDS parsing and CPU BC6H decoding (sutekh/dds.h). Headless: no GPU, no allocation. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/dds.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DDS_MAGIC 0x20534444u
#define DDSD_MIPMAPCOUNT 0x20000u
#define DDPF_ALPHAPIXELS 0x1u
#define DDPF_FOURCC 0x4u
#define DDPF_RGB 0x40u
#define DDSCAPS2_CUBEMAP 0x200u
#define DDSCAPS2_CUBE_ALL_FACES 0xFC00u
#define DDSCAPS2_VOLUME 0x200000u
#define DX10_MISC_CUBE 0x4u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void say(char *msg, const char *text) {
    if (msg != NULL) {
        (void)snprintf(msg, 160, "%s", text);
    }
}

/* The sRGB twin of a format, or 0. */
static uint32_t srgb_of(uint32_t f) {
    switch (f) {
    case SU_TEX_RGBA8_UNORM: return SU_TEX_RGBA8_SRGB;
    case SU_TEX_BC1_UNORM: return SU_TEX_BC1_SRGB;
    case SU_TEX_BC2_UNORM: return SU_TEX_BC2_SRGB;
    case SU_TEX_BC3_UNORM: return SU_TEX_BC3_SRGB;
    case SU_TEX_BC7_UNORM: return SU_TEX_BC7_SRGB;
    default: return 0u;
    }
}

static uint32_t linear_of(uint32_t f) {
    switch (f) {
    case SU_TEX_RGBA8_SRGB: return SU_TEX_RGBA8_UNORM;
    case SU_TEX_BC1_SRGB: return SU_TEX_BC1_UNORM;
    case SU_TEX_BC2_SRGB: return SU_TEX_BC2_UNORM;
    case SU_TEX_BC3_SRGB: return SU_TEX_BC3_UNORM;
    case SU_TEX_BC7_SRGB: return SU_TEX_BC7_UNORM;
    default: return f;
    }
}

/* DXGI_FORMAT to SU_TEX_*, 0 when not handled. */
static uint32_t format_of_dxgi(uint32_t d) {
    switch (d) {
    case 10: return SU_TEX_RGBA16F;       /* R16G16B16A16_FLOAT */
    case 28: return SU_TEX_RGBA8_UNORM;
    case 29: return SU_TEX_RGBA8_SRGB;
    case 71: return SU_TEX_BC1_UNORM;
    case 72: return SU_TEX_BC1_SRGB;
    case 74: return SU_TEX_BC2_UNORM;
    case 75: return SU_TEX_BC2_SRGB;
    case 77: return SU_TEX_BC3_UNORM;
    case 78: return SU_TEX_BC3_SRGB;
    case 80: return SU_TEX_BC4_UNORM;
    case 83: return SU_TEX_BC5_UNORM;
    case 95: return SU_TEX_BC6H_UF16;
    case 96: return SU_TEX_BC6H_SF16;
    case 98: return SU_TEX_BC7_UNORM;
    case 99: return SU_TEX_BC7_SRGB;
    default: return 0u;
    }
}

uint32_t su_dds_parse(const uint8_t *data, size_t size, uint32_t color_space, SuDdsImage *out, char message[160]) {
    uint32_t flags;
    uint32_t pf_flags;
    uint32_t fourcc;
    uint32_t caps2;
    uint32_t width;
    uint32_t height;
    uint32_t mips = 1;
    uint32_t faces = 1;
    uint32_t format = 0;
    uint32_t dxgi = 0;
    uint32_t big;
    uint32_t maxm = 1;
    uint32_t m;
    uint32_t f;
    uint64_t offset = 128;
    uint64_t per_face = 0;
    uint64_t total;
    if (data == NULL || out == NULL || color_space > SU_DDS_CS_LINEAR) {
        say(message, "null pointer or bad colour-space argument");
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (size < 128u || rd32(data) != DDS_MAGIC || rd32(data + 4) != 124u || rd32(data + 76) != 32u) {
        say(message, "not a DDS file (magic or header size)");
        return SU_E_PARSE;
    }
    flags = rd32(data + 8);
    height = rd32(data + 12);
    width = rd32(data + 16);
    if ((flags & DDSD_MIPMAPCOUNT) != 0u) {
        mips = rd32(data + 28);
        mips = mips == 0u ? 1u : mips;
    }
    pf_flags = rd32(data + 80);
    fourcc = rd32(data + 84);
    caps2 = rd32(data + 112);
    if (width == 0u || height == 0u || width > 16384u || height > 16384u) {
        say(message, "bad dimensions");
        return SU_E_PARSE;
    }
    if ((caps2 & DDSCAPS2_VOLUME) != 0u) {
        say(message, "volume textures are not supported");
        return SU_E_UNSUPPORTED_METHOD;
    }
    if ((caps2 & DDSCAPS2_CUBEMAP) != 0u) {
        if ((caps2 & DDSCAPS2_CUBE_ALL_FACES) != DDSCAPS2_CUBE_ALL_FACES) {
            say(message, "cube map without all six faces");
            return SU_E_UNSUPPORTED_METHOD;
        }
        faces = 6;
    }
    if ((pf_flags & DDPF_FOURCC) != 0u) {
        if (fourcc == FOURCC('D', 'X', '1', '0')) {
            uint32_t dim;
            uint32_t misc;
            uint32_t arr;
            if (size < 148u) {
                say(message, "truncated DX10 header");
                return SU_E_PARSE;
            }
            dxgi = rd32(data + 128);
            dim = rd32(data + 132);
            misc = rd32(data + 136);
            arr = rd32(data + 140);
            offset = 148;
            if (dim != 3u) {
                say(message, "only 2D textures are supported (DX10 resource dimension)");
                return SU_E_UNSUPPORTED_METHOD;
            }
            if (arr != 1u) {
                say(message, "texture arrays are not supported");
                return SU_E_UNSUPPORTED_METHOD;
            }
            if ((misc & DX10_MISC_CUBE) != 0u) {
                if ((caps2 & DDSCAPS2_CUBEMAP) == 0u) {
                    say(message, "DX10 cube flag without the cube caps");
                    return SU_E_PARSE;
                }
                faces = 6;
            }
            format = format_of_dxgi(dxgi);
            if (format == 0u) {
                if (message != NULL) {
                    (void)snprintf(message, 160, "unsupported DXGI format %u", (unsigned)dxgi);
                }
                return SU_E_UNSUPPORTED_METHOD;
            }
        } else if (fourcc == FOURCC('D', 'X', 'T', '1')) {
            format = SU_TEX_BC1_UNORM;
        } else if (fourcc == FOURCC('D', 'X', 'T', '3')) {
            format = SU_TEX_BC2_UNORM;
        } else if (fourcc == FOURCC('D', 'X', 'T', '5')) {
            format = SU_TEX_BC3_UNORM;
        } else if (fourcc == FOURCC('A', 'T', 'I', '1') || fourcc == FOURCC('B', 'C', '4', 'U')) {
            format = SU_TEX_BC4_UNORM;
        } else if (fourcc == FOURCC('A', 'T', 'I', '2') || fourcc == FOURCC('B', 'C', '5', 'U')) {
            format = SU_TEX_BC5_UNORM;
        } else if (fourcc == 113u) { /* D3DFMT_A16B16G16R16F */
            format = SU_TEX_RGBA16F;
        } else {
            say(message, "unsupported FourCC");
            return SU_E_UNSUPPORTED_METHOD;
        }
    } else if ((pf_flags & DDPF_RGB) != 0u && (pf_flags & DDPF_ALPHAPIXELS) != 0u && rd32(data + 88) == 32u &&
               rd32(data + 92) == 0x000000FFu && rd32(data + 96) == 0x0000FF00u && rd32(data + 100) == 0x00FF0000u &&
               rd32(data + 104) == 0xFF000000u) {
        format = SU_TEX_RGBA8_UNORM;
    } else {
        say(message, "unsupported pixel format (legacy masks other than RGBA8)");
        return SU_E_UNSUPPORTED_METHOD;
    }
    if (color_space == SU_DDS_CS_SRGB) {
        if (srgb_of(format) != 0u) {
            format = srgb_of(format);
        } else if (linear_of(format) == format) {
            say(message, "this format has no sRGB variant");
            return SU_E_INVALID_ARGUMENT;
        }
    } else if (color_space == SU_DDS_CS_LINEAR) {
        format = linear_of(format);
    }
    big = width > height ? width : height;
    while ((big >>= 1) != 0u) {
        maxm++;
    }
    if (mips > maxm || mips > SU_ASSET_MAX_MIPS) {
        say(message, "mip count does not fit the dimensions");
        return SU_E_PARSE;
    }
    for (m = 0; m < mips; m++) {
        per_face += su_asset_texture_level_size(format, width >> m ? width >> m : 1u, height >> m ? height >> m : 1u);
    }
    total = per_face * faces;
    if ((uint64_t)size < offset + total) {
        say(message, "pixel data is truncated");
        return SU_E_PARSE;
    }
    out->format = format;
    out->width = width;
    out->height = height;
    out->mip_count = mips;
    out->faces = faces;
    out->dxgi_format = dxgi;
    out->pixel_bytes = total;
    out->trailing_bytes = (uint64_t)size - offset - total;
    for (f = 0; f < faces; f++) {
        uint64_t at = offset + (uint64_t)f * per_face;
        for (m = 0; m < mips; m++) {
            out->level[f][m] = data + at;
            at += su_asset_texture_level_size(format, width >> m ? width >> m : 1u, height >> m ? height >> m : 1u);
        }
    }
    say(message, "ok");
    return SU_OK;
}

void su_dds_level_table(const SuDdsImage *img, const uint8_t *ptrs[SU_ASSET_MAX_MIPS * 6u]) {
    uint32_t m;
    uint32_t f;
    for (m = 0; m < img->mip_count; m++) {
        for (f = 0; f < img->faces; f++) {
            ptrs[m * img->faces + f] = img->level[f][m];
        }
    }
}

/* ---------- half floats ---------- */

float su_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    uint32_t bits;
    float f;
    if (exp == 0u) {
        if (man == 0u) {
            bits = sign;
        } else { /* subnormal: value = man * 2^-24 */
            float v = (float)man * (1.0f / 16777216.0f);
            memcpy(&bits, &v, sizeof bits);
            bits |= sign;
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    memcpy(&f, &bits, sizeof f);
    return f;
}

uint16_t su_float_to_half(float f) {
    uint32_t b;
    uint32_t sign;
    int32_t e;
    uint32_t man;
    memcpy(&b, &f, sizeof b);
    sign = (b >> 16) & 0x8000u;
    e = (int32_t)((b >> 23) & 0xFFu) - 127 + 15;
    man = b & 0x7FFFFFu;
    if (((b >> 23) & 0xFFu) == 0xFFu) {
        return (uint16_t)(sign | 0x7C00u | (man != 0u ? 0x200u : 0u));
    }
    if (e >= 31) {
        return (uint16_t)(sign | 0x7C00u);
    }
    if (e <= 0) {
        uint32_t shift;
        uint32_t half;
        uint32_t rem;
        uint32_t halfway;
        if (e < -10) {
            return (uint16_t)sign;
        }
        man |= 0x800000u;
        shift = (uint32_t)(14 - e);
        half = man >> shift;
        rem = man & ((1u << shift) - 1u);
        halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half & 1u) != 0u)) {
            half++;
        }
        return (uint16_t)(sign | half);
    }
    {
        uint32_t half = ((uint32_t)e << 10) | (man >> 13);
        uint32_t rem = man & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) {
            half++; /* may carry into the exponent, up to infinity: correct */
        }
        return (uint16_t)(sign | half);
    }
}

/* ---------- BC6H ---------- */

enum { CH_R = 0, CH_G = 1, CH_B = 2, EP_W = 0, EP_X = 1, EP_Y = 2, EP_Z = 3 };

typedef struct Bc6Field {
    uint8_t ch;    /* CH_* */
    uint8_t ep;    /* EP_*: the endpoint (W, then deltas X, Y, Z) */
    uint8_t lo;    /* destination bit of the field's least significant stored bit */
    uint8_t n;     /* stored bits */
    uint8_t rev;   /* 1: the stored bits are in reverse order (the high bits of 12 and 16 bit modes) */
} Bc6Field;

#define F(c, e, lo, n) {CH_##c, EP_##e, lo, n, 0}
#define V(c, e, lo, n) {CH_##c, EP_##e, lo, n, 1}

/* The 14 modes' field lists, in stream order after the mode bits (D3D11 functional specification, BC6H; OpenGL
 * ARB_texture_compression_bptc table "BPTC float mode bit layout"). Modes 0..9 end with the 5 partition bits. */
static const Bc6Field mode0[] = {F(G, Y, 4, 1), F(B, Y, 4, 1), F(B, Z, 4, 1), F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10),
                                 F(R, X, 0, 5), F(G, Z, 4, 1), F(G, Y, 0, 4), F(G, X, 0, 5), F(B, Z, 0, 1), F(G, Z, 0, 4),
                                 F(B, X, 0, 5), F(B, Z, 1, 1), F(B, Y, 0, 4), F(R, Y, 0, 5), F(B, Z, 2, 1), F(R, Z, 0, 5),
                                 F(B, Z, 3, 1)};
static const Bc6Field mode1[] = {F(G, Y, 5, 1), F(G, Z, 4, 1), F(G, Z, 5, 1), F(R, W, 0, 7), F(B, Z, 0, 1), F(B, Z, 1, 1),
                                 F(B, Y, 4, 1), F(G, W, 0, 7), F(B, Y, 5, 1), F(B, Z, 2, 1), F(G, Y, 4, 1), F(B, W, 0, 7),
                                 F(B, Z, 3, 1), F(B, Z, 5, 1), F(B, Z, 4, 1), F(R, X, 0, 6), F(G, Y, 0, 4), F(G, X, 0, 6),
                                 F(G, Z, 0, 4), F(B, X, 0, 6), F(B, Y, 0, 4), F(R, Y, 0, 6), F(R, Z, 0, 6)};
static const Bc6Field mode2[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 5), F(R, W, 10, 1), F(G, Y, 0, 4),
                                 F(G, X, 0, 4), F(G, W, 10, 1), F(B, Z, 0, 1), F(G, Z, 0, 4), F(B, X, 0, 4), F(B, W, 10, 1),
                                 F(B, Z, 1, 1), F(B, Y, 0, 4), F(R, Y, 0, 5), F(B, Z, 2, 1), F(R, Z, 0, 5), F(B, Z, 3, 1)};
static const Bc6Field mode3[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 4), F(R, W, 10, 1), F(G, Z, 4, 1),
                                 F(G, Y, 0, 4), F(G, X, 0, 5), F(G, W, 10, 1), F(G, Z, 0, 4), F(B, X, 0, 4), F(B, W, 10, 1),
                                 F(B, Z, 1, 1), F(B, Y, 0, 4), F(R, Y, 0, 4), F(B, Z, 0, 1), F(B, Z, 2, 1), F(R, Z, 0, 4),
                                 F(G, Y, 4, 1), F(B, Z, 3, 1)};
static const Bc6Field mode4[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 4), F(R, W, 10, 1), F(B, Y, 4, 1),
                                 F(G, Y, 0, 4), F(G, X, 0, 4), F(G, W, 10, 1), F(B, Z, 0, 1), F(G, Z, 0, 4), F(B, X, 0, 5),
                                 F(B, W, 10, 1), F(B, Y, 0, 4), F(R, Y, 0, 4), F(B, Z, 1, 1), F(B, Z, 2, 1), F(R, Z, 0, 4),
                                 F(B, Z, 4, 1), F(B, Z, 3, 1)};
static const Bc6Field mode5[] = {F(R, W, 0, 9), F(B, Y, 4, 1), F(G, W, 0, 9), F(G, Y, 4, 1), F(B, W, 0, 9), F(B, Z, 4, 1),
                                 F(R, X, 0, 5), F(G, Z, 4, 1), F(G, Y, 0, 4), F(G, X, 0, 5), F(B, Z, 0, 1), F(G, Z, 0, 4),
                                 F(B, X, 0, 5), F(B, Z, 1, 1), F(B, Y, 0, 4), F(R, Y, 0, 5), F(B, Z, 2, 1), F(R, Z, 0, 5),
                                 F(B, Z, 3, 1)};
static const Bc6Field mode6[] = {F(R, W, 0, 8), F(G, Z, 4, 1), F(B, Y, 4, 1), F(G, W, 0, 8), F(B, Z, 2, 1), F(G, Y, 4, 1),
                                 F(B, W, 0, 8), F(B, Z, 3, 1), F(B, Z, 4, 1), F(R, X, 0, 6), F(G, Y, 0, 4), F(G, X, 0, 5),
                                 F(B, Z, 0, 1), F(G, Z, 0, 4), F(B, X, 0, 5), F(B, Z, 1, 1), F(B, Y, 0, 4), F(R, Y, 0, 6),
                                 F(R, Z, 0, 6)};
static const Bc6Field mode7[] = {F(R, W, 0, 8), F(B, Z, 0, 1), F(B, Y, 4, 1), F(G, W, 0, 8), F(G, Y, 5, 1), F(G, Y, 4, 1),
                                 F(B, W, 0, 8), F(G, Z, 5, 1), F(B, Z, 4, 1), F(R, X, 0, 5), F(G, Z, 4, 1), F(G, Y, 0, 4),
                                 F(G, X, 0, 6), F(G, Z, 0, 4), F(B, X, 0, 5), F(B, Z, 1, 1), F(B, Y, 0, 4), F(R, Y, 0, 5),
                                 F(B, Z, 2, 1), F(R, Z, 0, 5), F(B, Z, 3, 1)};
static const Bc6Field mode8[] = {F(R, W, 0, 8), F(B, Z, 1, 1), F(B, Y, 4, 1), F(G, W, 0, 8), F(B, Y, 5, 1), F(G, Y, 4, 1),
                                 F(B, W, 0, 8), F(B, Z, 5, 1), F(B, Z, 4, 1), F(R, X, 0, 5), F(G, Z, 4, 1), F(G, Y, 0, 4),
                                 F(G, X, 0, 5), F(B, Z, 0, 1), F(G, Z, 0, 4), F(B, X, 0, 6), F(B, Y, 0, 4), F(R, Y, 0, 5),
                                 F(B, Z, 2, 1), F(R, Z, 0, 5), F(B, Z, 3, 1)};
static const Bc6Field mode9[] = {F(R, W, 0, 6), F(G, Z, 4, 1), F(B, Z, 0, 1), F(B, Z, 1, 1), F(B, Y, 4, 1), F(G, W, 0, 6),
                                 F(G, Y, 5, 1), F(B, Y, 5, 1), F(B, Z, 2, 1), F(G, Y, 4, 1), F(B, W, 0, 6), F(G, Z, 5, 1),
                                 F(B, Z, 3, 1), F(B, Z, 5, 1), F(B, Z, 4, 1), F(R, X, 0, 6), F(G, Y, 0, 4), F(G, X, 0, 6),
                                 F(G, Z, 0, 4), F(B, X, 0, 6), F(B, Y, 0, 4), F(R, Y, 0, 6), F(R, Z, 0, 6)};
static const Bc6Field mode10[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 10), F(G, X, 0, 10), F(B, X, 0, 10)};
static const Bc6Field mode11[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 9), F(R, W, 10, 1),
                                  F(G, X, 0, 9),  F(G, W, 10, 1), F(B, X, 0, 9),  F(B, W, 10, 1)};
static const Bc6Field mode12[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 8), V(R, W, 10, 2),
                                  F(G, X, 0, 8),  V(G, W, 10, 2), F(B, X, 0, 8),  V(B, W, 10, 2)};
static const Bc6Field mode13[] = {F(R, W, 0, 10), F(G, W, 0, 10), F(B, W, 0, 10), F(R, X, 0, 4), V(R, W, 10, 6),
                                  F(G, X, 0, 4),  V(G, W, 10, 6), F(B, X, 0, 4),  V(B, W, 10, 6)};

#undef F
#undef V

typedef struct Bc6Mode {
    const Bc6Field *fields;
    uint8_t count;
    uint8_t mode_bits;       /* 2 or 5 */
    uint8_t mode_value;
    uint8_t w_bits;          /* endpoint W precision */
    uint8_t d_bits[3];       /* delta precision per channel */
    uint8_t subsets;         /* 1 or 2 */
    uint8_t transformed;     /* deltas relative to W */
} Bc6Mode;

#define M(t, bits, val, w, dr, dg, db, subsets, tr) \
    {t, (uint8_t)(sizeof t / sizeof t[0]), bits, val, w, {dr, dg, db}, subsets, tr}
static const Bc6Mode bc6_modes[14] = {
    M(mode0, 2, 0x00, 10, 5, 5, 5, 2, 1),   M(mode1, 2, 0x01, 7, 6, 6, 6, 2, 1),    M(mode2, 5, 0x02, 11, 5, 4, 4, 2, 1),
    M(mode3, 5, 0x06, 11, 4, 5, 4, 2, 1),   M(mode4, 5, 0x0A, 11, 4, 4, 5, 2, 1),   M(mode5, 5, 0x0E, 9, 5, 5, 5, 2, 1),
    M(mode6, 5, 0x12, 8, 6, 5, 5, 2, 1),    M(mode7, 5, 0x16, 8, 5, 6, 5, 2, 1),    M(mode8, 5, 0x1A, 8, 5, 5, 6, 2, 1),
    M(mode9, 5, 0x1E, 6, 6, 6, 6, 2, 0),    M(mode10, 5, 0x03, 10, 10, 10, 10, 1, 0), M(mode11, 5, 0x07, 11, 9, 9, 9, 1, 1),
    M(mode12, 5, 0x0B, 12, 8, 8, 8, 1, 1),  M(mode13, 5, 0x0F, 16, 4, 4, 4, 1, 1)};
#undef M

/* Two-subset partition shapes: bit t of shape[p] is the subset of texel t; fixup[p] is the second subset's anchor
 * texel (the first subset's anchor is always texel 0). */
static const uint16_t bc6_shape[32] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C};
static const uint8_t bc6_fixup[32] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                      15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2};

static const int bc6_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const int bc6_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct Bits128 {
    uint64_t lo, hi;
} Bits128;

static uint32_t bits_get(const Bits128 *b, uint32_t pos, uint32_t n) {
    uint64_t v;
    if (n == 0u) {
        return 0u;
    }
    if (pos >= 64u) {
        v = b->hi >> (pos - 64u);
    } else if (pos + n <= 64u) {
        v = b->lo >> pos;
    } else {
        v = (b->lo >> pos) | (b->hi << (64u - pos));
    }
    return (uint32_t)(v & ((1ull << n) - 1ull));
}

static int extend_sign(int v, int bits) {
    return (int)((int32_t)((uint32_t)v << (32 - bits)) >> (32 - bits));
}

static int unquantize(int v, int bits, int is_signed) {
    if (!is_signed) {
        if (bits >= 15) {
            return v;
        }
        if (v == 0) {
            return 0;
        }
        if (v == (1 << bits) - 1) {
            return 0xFFFF;
        }
        return ((v << 16) + 0x8000) >> bits;
    } else {
        int s = 0;
        int u;
        if (bits >= 16) {
            return v;
        }
        if (v < 0) {
            s = 1;
            v = -v;
        }
        if (v == 0) {
            u = 0;
        } else if (v >= (1 << (bits - 1)) - 1) {
            u = 0x7FFF;
        } else {
            u = ((v << 15) + 0x4000) >> (bits - 1);
        }
        return s ? -u : u;
    }
}

static uint16_t finish_unquantize(int v, int is_signed) {
    if (!is_signed) {
        return (uint16_t)((v * 31) >> 6);
    }
    v = v < 0 ? -(((-v) * 31) >> 5) : (v * 31) >> 5;
    if (v < 0) {
        return (uint16_t)(0x8000 | (-v));
    }
    return (uint16_t)v;
}

uint32_t su_bc6h_decode_block(const uint8_t block[16], int is_signed, uint16_t out[16 * 3]) {
    Bits128 b;
    const Bc6Mode *md = NULL;
    int ep[3][4];
    uint32_t pos;
    uint32_t i;
    uint32_t t;
    uint32_t lo2;
    uint32_t partition = 0;
    uint32_t index_bits;
    const int *weights;
    b.lo = 0;
    b.hi = 0;
    for (i = 0; i < 8u; i++) {
        b.lo |= (uint64_t)block[i] << (8u * i);
        b.hi |= (uint64_t)block[8u + i] << (8u * i);
    }
    lo2 = (uint32_t)(b.lo & 3u);
    if (lo2 < 2u) {
        md = &bc6_modes[lo2];
    } else {
        uint32_t m5 = (uint32_t)(b.lo & 31u);
        for (i = 2; i < 14u; i++) {
            if (bc6_modes[i].mode_value == m5) {
                md = &bc6_modes[i];
                break;
            }
        }
    }
    if (md == NULL) { /* reserved: zeros */
        memset(out, 0, 16u * 3u * sizeof *out);
        return 0;
    }
    memset(ep, 0, sizeof ep);
    pos = md->mode_bits;
    for (i = 0; i < md->count; i++) {
        const Bc6Field *f = &md->fields[i];
        uint32_t v = bits_get(&b, pos, f->n);
        pos += f->n;
        if (f->rev) {
            uint32_t r = 0;
            uint32_t k;
            for (k = 0; k < f->n; k++) {
                r = (r << 1) | ((v >> k) & 1u);
            }
            v = r;
        }
        ep[f->ch][f->ep] |= (int)(v << f->lo);
    }
    if (md->subsets == 2u) {
        partition = bits_get(&b, pos, 5);
        pos += 5;
    }
    {
        int nep = (int)md->subsets * 2;
        int e;
        int ch;
        if (is_signed) {
            for (ch = 0; ch < 3; ch++) {
                ep[ch][0] = extend_sign(ep[ch][0], md->w_bits);
            }
        }
        if (md->transformed || is_signed) {
            for (ch = 0; ch < 3; ch++) {
                for (e = 1; e < nep; e++) {
                    ep[ch][e] = extend_sign(ep[ch][e], md->d_bits[ch]);
                }
            }
        }
        if (md->transformed) {
            for (ch = 0; ch < 3; ch++) {
                for (e = 1; e < nep; e++) {
                    int v = (ep[ch][e] + ep[ch][0]) & ((1 << md->w_bits) - 1);
                    ep[ch][e] = is_signed ? extend_sign(v, md->w_bits) : v;
                }
            }
        }
        for (ch = 0; ch < 3; ch++) {
            for (e = 0; e < nep; e++) {
                ep[ch][e] = unquantize(ep[ch][e], md->w_bits, is_signed);
            }
        }
    }
    index_bits = md->subsets == 2u ? 3u : 4u;
    weights = md->subsets == 2u ? bc6_w3 : bc6_w4;
    for (t = 0; t < 16u; t++) {
        uint32_t subset = md->subsets == 2u ? (uint32_t)((bc6_shape[partition] >> t) & 1u) : 0u;
        uint32_t nbits = index_bits;
        uint32_t idx;
        int ch;
        if (t == 0u || (md->subsets == 2u && t == bc6_fixup[partition])) {
            nbits--;
        }
        idx = bits_get(&b, pos, nbits);
        pos += nbits;
        for (ch = 0; ch < 3; ch++) {
            int a = ep[ch][subset * 2u];
            int c = ep[ch][subset * 2u + 1u];
            int w = weights[idx];
            out[t * 3u + (uint32_t)ch] = finish_unquantize((a * (64 - w) + c * w + 32) >> 6, is_signed);
        }
    }
    return 1;
}

/* ---------- whole-level decoding ---------- */

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

uint32_t su_dds_decode_rgba32f(const SuDdsImage *img, uint32_t face, uint32_t mip, float *out) {
    uint32_t w;
    uint32_t h;
    const uint8_t *src;
    uint32_t x;
    uint32_t y;
    if (img == NULL || out == NULL || face >= img->faces || mip >= img->mip_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    w = img->width >> mip ? img->width >> mip : 1u;
    h = img->height >> mip ? img->height >> mip : 1u;
    src = img->level[face][mip];
    switch (img->format) {
    case SU_TEX_BC6H_UF16:
    case SU_TEX_BC6H_SF16: {
        uint32_t bw = (w + 3u) / 4u;
        uint32_t bx;
        uint32_t by;
        for (by = 0; by < (h + 3u) / 4u; by++) {
            for (bx = 0; bx < bw; bx++) {
                uint16_t px[48];
                uint32_t k;
                (void)su_bc6h_decode_block(src + ((size_t)by * bw + bx) * 16u, img->format == SU_TEX_BC6H_SF16, px);
                for (k = 0; k < 16u; k++) {
                    uint32_t tx = bx * 4u + (k & 3u);
                    uint32_t ty = by * 4u + (k >> 2);
                    if (tx < w && ty < h) {
                        float *o = out + ((size_t)ty * w + tx) * 4u;
                        uint32_t c;
                        for (c = 0; c < 3u; c++) {
                            o[c] = su_half_to_float(px[k * 3u + c]);
                        }
                        o[3] = 1.0f;
                    }
                }
            }
        }
        return SU_OK;
    }
    case SU_TEX_RGBA16F:
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint32_t c;
                for (c = 0; c < 4u; c++) {
                    const uint8_t *p = src + (((size_t)y * w + x) * 4u + c) * 2u;
                    out[((size_t)y * w + x) * 4u + c] = su_half_to_float((uint16_t)(p[0] | (p[1] << 8)));
                }
            }
        }
        return SU_OK;
    case SU_TEX_RGBA8_UNORM:
    case SU_TEX_RGBA8_SRGB:
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint32_t c;
                for (c = 0; c < 4u; c++) {
                    float v = (float)src[((size_t)y * w + x) * 4u + c] / 255.0f;
                    out[((size_t)y * w + x) * 4u + c] = (img->format == SU_TEX_RGBA8_SRGB && c < 3u) ? srgb_to_linear(v) : v;
                }
            }
        }
        return SU_OK;
    case SU_TEX_BC1_UNORM:
    case SU_TEX_BC1_SRGB:
    case SU_TEX_BC2_UNORM:
    case SU_TEX_BC2_SRGB:
    case SU_TEX_BC3_UNORM:
    case SU_TEX_BC3_SRGB:
    case SU_TEX_BC4_UNORM:
    case SU_TEX_BC5_UNORM:
    case SU_TEX_BC7_UNORM:
    case SU_TEX_BC7_SRGB: {
        int srgb = img->format == SU_TEX_BC1_SRGB || img->format == SU_TEX_BC2_SRGB || img->format == SU_TEX_BC3_SRGB ||
                   img->format == SU_TEX_BC7_SRGB;
        size_t bsize = (img->format == SU_TEX_BC1_UNORM || img->format == SU_TEX_BC1_SRGB || img->format == SU_TEX_BC4_UNORM)
                           ? 8u
                           : 16u;
        uint32_t bw = (w + 3u) / 4u;
        uint32_t bx;
        uint32_t by;
        float lut[256];
        for (x = 0; x < 256u; x++) {
            lut[x] = srgb ? srgb_to_linear((float)x / 255.0f) : (float)x / 255.0f;
        }
        for (by = 0; by < (h + 3u) / 4u; by++) {
            for (bx = 0; bx < bw; bx++) {
                uint8_t px[64];
                uint32_t k;
                (void)su_bc_decode_block(img->format, src + ((size_t)by * bw + bx) * bsize, px);
                for (k = 0; k < 16u; k++) {
                    uint32_t tx = bx * 4u + (k & 3u);
                    uint32_t ty = by * 4u + (k >> 2);
                    if (tx < w && ty < h) {
                        float *o = out + ((size_t)ty * w + tx) * 4u;
                        o[0] = lut[px[k * 4u]];
                        o[1] = lut[px[k * 4u + 1u]];
                        o[2] = lut[px[k * 4u + 2u]];
                        o[3] = (float)px[k * 4u + 3u] / 255.0f;
                    }
                }
            }
        }
        return SU_OK;
    }
    default:
        return SU_E_UNSUPPORTED_METHOD;
    }
}
