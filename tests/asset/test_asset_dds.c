/* DDS loading and the CPU BC6H decoder (sutekh/dds.h), headless. Every DDS file here is generated in memory by a
 * tiny writer below (no third-party files):
 *   - headers: legacy FourCC and DX10, 2D and cube, mip chains with non-multiple-of-4 sizes, explicit colour space,
 *     trailing metadata after the pixels is ignored, truncation and malformed headers are refused, header fuzzing;
 *   - BC6H: single-colour cube faces decoded and checked against an independent formula; fourteen blocks (one per
 *     mode) unsigned and signed whose output hashes were produced by the MIT / Unlicense bcdec reference decoder
 *     (run offline on the same blocks; 4 million random blocks matched it bit for bit, see the handoff); reserved
 *     modes give zeros;
 *   - half-float conversion: all 65536 values round trip.
 * Usage: test_asset_dds */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/dds.h"

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

/* ---------- the tiny writer ---------- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct DdsSpec {
    uint32_t w, h, mips;
    int cube;
    uint32_t fourcc;      /* legacy FourCC, or 'DX10' */
    uint32_t dxgi;        /* with DX10 */
    uint32_t array_size;  /* DX10 (0 = 1) */
    uint32_t caps2_extra;
    int rgba8_masks;      /* legacy uncompressed RGBA8 */
} DdsSpec;

/* Writes headers plus pixel_bytes of pixels (copied from pixels) and returns the file size. */
static size_t write_dds(uint8_t *out, const DdsSpec *s, const uint8_t *pixels, size_t pixel_bytes) {
    size_t at = 128;
    uint32_t caps2 = s->caps2_extra;
    memset(out, 0, 148);
    put32(out, 0x20534444u);
    put32(out + 4, 124);
    put32(out + 8, 0x1007u | (s->mips > 1u ? 0x20000u : 0u)); /* CAPS | HEIGHT | WIDTH | PIXELFORMAT, MIPMAPCOUNT */
    put32(out + 12, s->h);
    put32(out + 16, s->w);
    put32(out + 28, s->mips);
    put32(out + 76, 32);
    if (s->rgba8_masks) {
        put32(out + 80, 0x41u); /* RGB | ALPHAPIXELS */
        put32(out + 88, 32);
        put32(out + 92, 0x000000FFu);
        put32(out + 96, 0x0000FF00u);
        put32(out + 100, 0x00FF0000u);
        put32(out + 104, 0xFF000000u);
    } else {
        put32(out + 80, 4); /* FOURCC */
        put32(out + 84, s->fourcc);
    }
    if (s->cube) {
        caps2 |= 0x200u | 0xFC00u;
    }
    put32(out + 108, 0x1000u | (s->mips > 1u ? 0x400008u : 0u));
    put32(out + 112, caps2);
    if (s->fourcc == FOURCC('D', 'X', '1', '0')) {
        put32(out + 128, s->dxgi);
        put32(out + 132, 3);
        put32(out + 136, s->cube ? 4u : 0u);
        put32(out + 140, s->array_size != 0u ? s->array_size : 1u);
        at = 148;
    }
    memcpy(out + at, pixels, pixel_bytes);
    return at + pixel_bytes;
}

/* ---------- BC6H single-colour block (mode 11: 10 bit endpoints, no delta) ---------- */

static void put_bits(uint8_t b[16], uint32_t *pos, uint32_t v, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1u) {
            b[*pos >> 3] = (uint8_t)(b[*pos >> 3] | (1u << (*pos & 7u)));
        }
        (*pos)++;
    }
}

static void bc6h_solid(uint8_t b[16], uint32_t r, uint32_t g, uint32_t bl) {
    uint32_t pos = 0;
    memset(b, 0, 16);
    put_bits(b, &pos, 0x03u, 5);
    put_bits(b, &pos, r, 10);
    put_bits(b, &pos, g, 10);
    put_bits(b, &pos, bl, 10);
    put_bits(b, &pos, r, 10);
    put_bits(b, &pos, g, 10);
    put_bits(b, &pos, bl, 10);
    /* the 63 index bits stay 0: every texel takes endpoint 0 */
}

/* The unsigned decode of a 10 bit endpoint with weight 0, from the specification's formulas. */
static uint16_t solid_half(uint32_t x10) {
    uint32_t unq = x10 == 0u ? 0u : (x10 == 1023u ? 0xFFFFu : ((x10 << 16) + 0x8000u) >> 10);
    return (uint16_t)((unq * 31u) >> 6);
}

/* FNV-1a over the little-endian bytes of the 48 half floats */
static uint32_t fnv_halves(const uint16_t *h, uint32_t n) {
    uint32_t x = 2166136261u;
    uint32_t i;
    for (i = 0; i < n; i++) {
        x ^= (uint8_t)(h[i] & 255u);
        x *= 16777619u;
        x ^= (uint8_t)(h[i] >> 8);
        x *= 16777619u;
    }
    return x;
}

/* One block per mode (random bytes with the mode bits set), then the hash of the reference decoder's output. */
static const struct {
    uint8_t block[16];
    uint32_t hash_unsigned, hash_signed;
} bc6h_vectors[14] = {
    {{0x58, 0x16, 0xE1, 0x92, 0x5C, 0x8C, 0xE9, 0xAC, 0x92, 0x28, 0x01, 0xCD, 0x00, 0x96, 0x4E, 0x59}, 0x80279661u, 0xA2F8DEFEu},
    {{0x3D, 0x02, 0xAA, 0xDA, 0xB6, 0x61, 0xE2, 0x3B, 0xE8, 0xA7, 0xF6, 0x0C, 0xA8, 0x0F, 0x5D, 0xC1}, 0xC8BFBDC6u, 0x5A45125Du},
    {{0x42, 0x57, 0x3F, 0xF4, 0x3E, 0x02, 0xBA, 0xA0, 0xA0, 0x96, 0x1E, 0x65, 0x24, 0xDC, 0x32, 0xC7}, 0xA09C4B00u, 0x471143D4u},
    {{0x06, 0x24, 0x6F, 0x70, 0x43, 0x80, 0x41, 0x6B, 0x09, 0x05, 0x4A, 0x68, 0xC5, 0x0E, 0x9F, 0xFC}, 0x0EF932E1u, 0xE741481Cu},
    {{0x8A, 0x7A, 0x0A, 0xDF, 0x15, 0xEA, 0x47, 0x2C, 0x73, 0x04, 0x49, 0xA6, 0xDC, 0xB4, 0x73, 0xEE}, 0x416DAB5Du, 0xB047B6E1u},
    {{0x2E, 0x67, 0xE0, 0xCF, 0x04, 0x50, 0x9D, 0x74, 0x2E, 0xA4, 0xEB, 0xAD, 0xB8, 0xDE, 0x7E, 0x2F}, 0x6902DB6Cu, 0xD2CC0AE5u},
    {{0x52, 0xFD, 0xC2, 0xD2, 0x61, 0xC2, 0x12, 0xD2, 0x8A, 0xF4, 0x00, 0x10, 0xA9, 0x9D, 0x91, 0x4F}, 0x10AAFD12u, 0x7A6ACDCAu},
    {{0x36, 0x4B, 0x7F, 0x77, 0x7B, 0x51, 0x76, 0xD6, 0xD8, 0x04, 0x58, 0x5C, 0x00, 0x00, 0x7B, 0xDC}, 0x1CF8E0B0u, 0x0A504CBCu},
    {{0x3A, 0x61, 0xE7, 0x4F, 0xA2, 0x0C, 0x99, 0x11, 0x67, 0xE5, 0xC4, 0x23, 0x0C, 0x17, 0x0C, 0x68}, 0x13DE3698u, 0x539C31E8u},
    {{0xBE, 0x50, 0xCA, 0xE9, 0x27, 0x03, 0x4C, 0x12, 0x87, 0xA5, 0x13, 0xF3, 0x1D, 0xF2, 0x14, 0x82}, 0x3FA516AAu, 0x1940396Bu},
    {{0xC3, 0x27, 0xF9, 0xD5, 0x58, 0x46, 0x5E, 0x69, 0x89, 0x56, 0x15, 0x5E, 0x83, 0xA1, 0x64, 0xBA}, 0x7861D59Eu, 0xC66CC726u},
    {{0xE7, 0xF6, 0x43, 0xA3, 0x87, 0xE6, 0x9F, 0xA6, 0xBB, 0x07, 0x9B, 0xF4, 0x8E, 0x35, 0xCA, 0xA0}, 0x34D1C1ADu, 0xE98B032Fu},
    {{0x8B, 0xCD, 0x78, 0xE3, 0x03, 0xF2, 0xE0, 0x5A, 0x6F, 0xC9, 0x73, 0x43, 0x8F, 0xBD, 0x19, 0xC5}, 0x391FF296u, 0xECED96DEu},
    {{0xAF, 0xBD, 0x68, 0x26, 0x1D, 0x7A, 0xEF, 0x13, 0xF4, 0xAB, 0x6F, 0xDD, 0xD5, 0x49, 0x1E, 0xB8}, 0x99CCE347u, 0x13C5F8D0u},
};

static uint32_t g_seed = 99;
static uint32_t rnd(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static void test_formats(void) {
    CHECK(su_asset_texture_level_size(SU_TEX_BC2_UNORM, 10, 6) == 6u * 16u);
    CHECK(su_asset_texture_level_size(SU_TEX_BC6H_UF16, 1, 1) == 16u);
    CHECK(su_asset_texture_level_size(SU_TEX_BC6H_SF16, 8, 8) == 64u);
    CHECK(su_asset_texture_level_size(SU_TEX_RGBA16F, 3, 2) == 48u);
    CHECK(su_asset_texture_level_size(SU_TEX_FORMAT_COUNT, 4, 4) == 0u);
    CHECK(strcmp(su_asset_texture_format_name(SU_TEX_BC6H_UF16), "bc6h_uf16") == 0);
    CHECK(strcmp(su_asset_texture_format_name(SU_TEX_RGBA16F), "rgba16f") == 0);
}

static void test_half(void) {
    uint32_t h;
    uint32_t bad = 0;
    for (h = 0; h < 65536u; h++) {
        float f = su_half_to_float((uint16_t)h);
        uint16_t back = su_float_to_half(f);
        int is_nan = (h & 0x7C00u) == 0x7C00u && (h & 0x3FFu) != 0u;
        if (is_nan) {
            bad += !(f != f && (back & 0x7C00u) == 0x7C00u && (back & 0x3FFu) != 0u);
        } else {
            bad += back != (uint16_t)h;
        }
        if (!is_nan && (h & 0x7C00u) != 0x7C00u) { /* finite: exact value from the definition */
            uint32_t e = (h >> 10) & 31u;
            uint32_t m = h & 1023u;
            double v = e == 0u ? ldexp((double)m, -24) : ldexp((double)(1024u + m), (int)e - 25);
            if ((h & 0x8000u) != 0u) {
                v = -v;
            }
            bad += (double)f != v;
        }
    }
    CHECK(bad == 0u);
    CHECK(su_float_to_half(65520.0f) == 0x7C00u);   /* rounds to infinity */
    CHECK(su_float_to_half(65519.0f) == 0x7BFFu);
    CHECK(su_float_to_half(1.0f / 3.0f) == 0x3555u); /* round to nearest even */
    CHECK(su_float_to_half(1e-9f) == 0u);
}

static void test_bc6h(void) {
    uint32_t m;
    uint16_t px[48];
    uint8_t blk[16];
    uint32_t x;
    for (m = 0; m < 14u; m++) {
        CHECK(su_bc6h_decode_block(bc6h_vectors[m].block, 0, px) == 1u);
        CHECK(fnv_halves(px, 48) == bc6h_vectors[m].hash_unsigned);
        CHECK(su_bc6h_decode_block(bc6h_vectors[m].block, 1, px) == 1u);
        CHECK(fnv_halves(px, 48) == bc6h_vectors[m].hash_signed);
    }
    /* reserved modes: all zeros */
    {
        static const uint8_t reserved[4] = {0x13, 0x17, 0x1B, 0x1F};
        uint32_t i;
        for (i = 0; i < 4u; i++) {
            uint32_t k;
            uint32_t zeros = 1;
            memset(blk, 0xA5, 16);
            blk[0] = (uint8_t)((blk[0] & 0xE0u) | reserved[i]);
            CHECK(su_bc6h_decode_block(blk, i & 1u, px) == 0u);
            for (k = 0; k < 48u; k++) {
                zeros &= px[k] == 0u;
            }
            CHECK(zeros);
        }
    }
    /* single colour, the specification's formula, every 10 bit value for red, and a few mixed triples */
    for (x = 0; x < 1024u; x++) {
        uint32_t t;
        uint32_t ok = 1;
        bc6h_solid(blk, x, (x * 7u) & 1023u, 1023u - x);
        (void)su_bc6h_decode_block(blk, 0, px);
        for (t = 0; t < 16u; t++) {
            ok &= px[t * 3u] == solid_half(x) && px[t * 3u + 1u] == solid_half((x * 7u) & 1023u) &&
                  px[t * 3u + 2u] == solid_half(1023u - x);
        }
        CHECK(ok);
    }
    CHECK(solid_half(1023) == 0x7BFFu && solid_half(0) == 0u);
}

static void test_parse(void) {
    static uint8_t file[1 << 16];
    static uint8_t pix[1 << 15];
    SuDdsImage img;
    char msg[160];
    DdsSpec s;
    size_t n;
    uint32_t i;
    uint64_t expect;
    for (i = 0; i < sizeof pix; i++) {
        pix[i] = (uint8_t)(i * 31u + 7u);
    }

    /* BC1, legacy DXT1, 10x6, four mips: blocks 3x2, 2x1, 1x1, 1x1 */
    memset(&s, 0, sizeof s);
    s.w = 10;
    s.h = 6;
    s.mips = 4;
    s.fourcc = FOURCC('D', 'X', 'T', '1');
    expect = 6u * 8u + 2u * 8u + 8u + 8u;
    n = write_dds(file, &s, pix, (size_t)expect);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK);
    CHECK(img.format == SU_TEX_BC1_UNORM && img.width == 10 && img.height == 6 && img.mip_count == 4 && img.faces == 1);
    CHECK(img.pixel_bytes == expect && img.trailing_bytes == 0u && img.dxgi_format == 0u);
    CHECK(img.level[0][0] == file + 128 && img.level[0][1] == file + 128 + 48 && img.level[0][2] == file + 128 + 64 &&
          img.level[0][3] == file + 128 + 72);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_SRGB, &img, msg) == SU_OK && img.format == SU_TEX_BC1_SRGB);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_LINEAR, &img, msg) == SU_OK && img.format == SU_TEX_BC1_UNORM);
    /* trailing bytes after the pixels (tool metadata) are ignored, and counted */
    {
        uint32_t k;
        for (k = 0; k < 37u; k++) {
            file[n + k] = (uint8_t)(0xEE ^ k);
        }
        CHECK(su_dds_parse(file, n + 37u, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.trailing_bytes == 37u &&
              img.pixel_bytes == expect);
    }
    /* truncated by one byte */
    CHECK(su_dds_parse(file, n - 1u, SU_DDS_CS_FILE, &img, msg) == SU_E_PARSE);
    CHECK(strstr(msg, "truncated") != NULL);

    /* DX10 BC7 sRGB 4x4 and the overrides */
    memset(&s, 0, sizeof s);
    s.w = s.h = 4;
    s.mips = 1;
    s.fourcc = FOURCC('D', 'X', '1', '0');
    s.dxgi = 99;
    n = write_dds(file, &s, pix, 16);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.format == SU_TEX_BC7_SRGB && img.dxgi_format == 99u);
    CHECK(img.level[0][0] == file + 148);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_LINEAR, &img, msg) == SU_OK && img.format == SU_TEX_BC7_UNORM);
    s.dxgi = 98;
    n = write_dds(file, &s, pix, 16);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.format == SU_TEX_BC7_UNORM);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_SRGB, &img, msg) == SU_OK && img.format == SU_TEX_BC7_SRGB);

    /* the other formats of the list */
    {
        static const struct {
            uint32_t dxgi, format, blk_bytes;
        } fm[] = {{71, SU_TEX_BC1_UNORM, 8},  {72, SU_TEX_BC1_SRGB, 8},  {74, SU_TEX_BC2_UNORM, 16}, {75, SU_TEX_BC2_SRGB, 16},
                  {77, SU_TEX_BC3_UNORM, 16}, {78, SU_TEX_BC3_SRGB, 16}, {80, SU_TEX_BC4_UNORM, 8},  {83, SU_TEX_BC5_UNORM, 16},
                  {95, SU_TEX_BC6H_UF16, 16}, {96, SU_TEX_BC6H_SF16, 16}, {98, SU_TEX_BC7_UNORM, 16}, {99, SU_TEX_BC7_SRGB, 16}};
        for (i = 0; i < sizeof fm / sizeof fm[0]; i++) {
            s.dxgi = fm[i].dxgi;
            s.w = s.h = 8;
            n = write_dds(file, &s, pix, 4u * fm[i].blk_bytes);
            CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.format == fm[i].format &&
                  img.pixel_bytes == 4u * fm[i].blk_bytes);
        }
    }
    /* legacy FourCCs */
    {
        static const struct {
            uint32_t cc, format;
        } lg[] = {{FOURCC('D', 'X', 'T', '3'), SU_TEX_BC2_UNORM}, {FOURCC('D', 'X', 'T', '5'), SU_TEX_BC3_UNORM},
                  {FOURCC('A', 'T', 'I', '1'), SU_TEX_BC4_UNORM}, {FOURCC('B', 'C', '4', 'U'), SU_TEX_BC4_UNORM},
                  {FOURCC('A', 'T', 'I', '2'), SU_TEX_BC5_UNORM}, {FOURCC('B', 'C', '5', 'U'), SU_TEX_BC5_UNORM},
                  {113, SU_TEX_RGBA16F}};
        for (i = 0; i < sizeof lg / sizeof lg[0]; i++) {
            memset(&s, 0, sizeof s);
            s.w = s.h = 4;
            s.mips = 1;
            s.fourcc = lg[i].cc;
            n = write_dds(file, &s, pix, 128);
            CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.format == lg[i].format);
        }
    }
    /* legacy uncompressed RGBA8 masks */
    memset(&s, 0, sizeof s);
    s.w = 3;
    s.h = 2;
    s.mips = 1;
    s.rgba8_masks = 1;
    n = write_dds(file, &s, pix, 24);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.format == SU_TEX_RGBA8_UNORM &&
          img.pixel_bytes == 24u);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_SRGB, &img, msg) == SU_OK && img.format == SU_TEX_RGBA8_SRGB);
    {
        float out[24];
        pix[0] = 255;
        pix[1] = 0;
        pix[2] = 128;
        pix[3] = 51;
        n = write_dds(file, &s, pix, 24);
        CHECK(su_dds_parse(file, n, SU_DDS_CS_SRGB, &img, msg) == SU_OK);
        CHECK(su_dds_decode_rgba32f(&img, 0, 0, out) == SU_OK);
        CHECK(out[0] == 1.0f && out[1] == 0.0f && fabsf(out[2] - 0.2158605f) < 1e-5f && fabsf(out[3] - 0.2f) < 1e-6f);
    }

    /* BC6H cube, 8x8, two mips: six faces each solid colour, face-major in the file */
    {
        static uint8_t cube[6 * (64 + 16)];
        uint32_t f;
        uint32_t b;
        for (f = 0; f < 6u; f++) {
            for (b = 0; b < 5u; b++) {
                bc6h_solid(cube + f * 80u + b * 16u, 100u * f + 10u, 100u * f + 20u + b, 1000u - 100u * f);
            }
        }
        memset(&s, 0, sizeof s);
        s.w = s.h = 8;
        s.mips = 2;
        s.cube = 1;
        s.fourcc = FOURCC('D', 'X', '1', '0');
        s.dxgi = 95;
        n = write_dds(file, &s, cube, sizeof cube);
        CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK);
        CHECK(img.format == SU_TEX_BC6H_UF16 && img.faces == 6 && img.mip_count == 2 && img.pixel_bytes == sizeof cube);
        for (f = 0; f < 6u; f++) {
            CHECK(img.level[f][0] == file + 148 + f * 80u && img.level[f][1] == file + 148 + f * 80u + 64u);
        }
        {
            const uint8_t *tab[SU_ASSET_MAX_MIPS * 6u];
            su_dds_level_table(&img, tab);
            CHECK(tab[0 * 6 + 3] == img.level[3][0] && tab[1 * 6 + 5] == img.level[5][1]);
        }
        {
            static float lvl[8 * 8 * 4];
            uint32_t ok = 1;
            for (f = 0; f < 6u; f++) {
                uint32_t x;
                uint32_t y;
                CHECK(su_dds_decode_rgba32f(&img, f, 0, lvl) == SU_OK);
                for (y = 0; y < 8u; y++) {
                    for (x = 0; x < 8u; x++) {
                        const float *p = lvl + ((size_t)y * 8u + x) * 4u;
                        uint32_t blk = (y / 4u) * 2u + x / 4u;
                        ok &= p[0] == su_half_to_float(solid_half(100u * f + 10u)) &&
                              p[1] == su_half_to_float(solid_half(100u * f + 20u + blk)) &&
                              p[2] == su_half_to_float(solid_half(1000u - 100u * f)) && p[3] == 1.0f;
                    }
                }
            }
            CHECK(ok);
            CHECK(su_dds_decode_rgba32f(&img, 6, 0, lvl) == SU_E_INVALID_ARGUMENT);
            CHECK(su_dds_decode_rgba32f(&img, 0, 2, lvl) == SU_E_INVALID_ARGUMENT);
        }
        /* an sRGB request on an HDR format is refused; trailing metadata is fine */
        CHECK(su_dds_parse(file, n, SU_DDS_CS_SRGB, &img, msg) == SU_E_INVALID_ARGUMENT);
        CHECK(su_dds_parse(file, n, SU_DDS_CS_LINEAR, &img, msg) == SU_OK && img.format == SU_TEX_BC6H_UF16);
        memset(file + n, 0x5A, 123);
        CHECK(su_dds_parse(file, n + 123u, SU_DDS_CS_FILE, &img, msg) == SU_OK && img.trailing_bytes == 123u);
        /* a cube without its six faces, and one cut short */
        file[112] = 0x00;
        file[113] = 0x02 | 0x04; /* CUBEMAP and +X only */
        CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_E_UNSUPPORTED_METHOD);
        write_dds(file, &s, cube, sizeof cube);
        CHECK(su_dds_parse(file, 148u + 5u * 80u + 70u, SU_DDS_CS_FILE, &img, msg) == SU_E_PARSE);
    }

    /* refusals */
    memset(&s, 0, sizeof s);
    s.w = s.h = 4;
    s.mips = 1;
    s.fourcc = FOURCC('D', 'X', '1', '0');
    s.dxgi = 95;
    n = write_dds(file, &s, pix, 16);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, &img, msg) == SU_OK);
    CHECK(su_dds_parse(NULL, n, SU_DDS_CS_FILE, &img, msg) == SU_E_INVALID_ARGUMENT);
    CHECK(su_dds_parse(file, n, SU_DDS_CS_FILE, NULL, msg) == SU_E_INVALID_ARGUMENT);
    CHECK(su_dds_parse(file, n, 7u, &img, msg) == SU_E_INVALID_ARGUMENT);
    CHECK(su_dds_parse(file, 100, SU_DDS_CS_FILE, &img, NULL) == SU_E_PARSE);
    {
        uint8_t bad[256];
        memcpy(bad, file, n);
        bad[0] = 'X';
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_PARSE);
        memcpy(bad, file, n);
        put32(bad + 140, 2); /* array of two */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_UNSUPPORTED_METHOD);
        memcpy(bad, file, n);
        put32(bad + 128, 87); /* B8G8R8A8: not handled */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_UNSUPPORTED_METHOD);
        memcpy(bad, file, n);
        put32(bad + 132, 4); /* 3D */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_UNSUPPORTED_METHOD);
        memcpy(bad, file, n);
        put32(bad + 112, 0x200000u); /* volume caps */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_UNSUPPORTED_METHOD);
        memcpy(bad, file, n);
        put32(bad + 12, 0); /* zero height */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_PARSE);
        memcpy(bad, file, n);
        put32(bad + 8, 0x20000u | 0x1007u);
        put32(bad + 28, 4); /* four mips cannot fit a 4x4 image */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_PARSE);
        memcpy(bad, file, n);
        put32(bad + 84, FOURCC('D', 'X', 'T', '2')); /* not DX10: premultiplied DXT2 is not handled */
        CHECK(su_dds_parse(bad, n, SU_DDS_CS_FILE, &img, msg) == SU_E_UNSUPPORTED_METHOD);
    }

    /* fuzz: flip header bytes of valid files; a success must stay inside the buffer */
    {
        uint32_t iter;
        uint32_t accepted = 0;
        for (iter = 0; iter < 30000u; iter++) {
            uint8_t fz[400];
            SuDdsImage r2;
            uint32_t k;
            uint32_t nf;
            uint32_t m2;
            size_t sz = write_dds(fz, &s, pix, 16);
            for (k = 0; k < 1u + rnd() % 3u; k++) {
                fz[rnd() % 148u] = (uint8_t)rnd();
            }
            if ((rnd() & 3u) == 0u) {
                sz = rnd() % sz;
            }
            if (su_dds_parse(fz, sz, (uint32_t)(rnd() % 3u), &r2, NULL) == SU_OK) {
                accepted++;
                for (nf = 0; nf < r2.faces; nf++) {
                    for (m2 = 0; m2 < r2.mip_count; m2++) {
                        uint64_t lsz = su_asset_texture_level_size(r2.format, r2.width >> m2 ? r2.width >> m2 : 1u,
                                                                   r2.height >> m2 ? r2.height >> m2 : 1u);
                        CHECK(r2.level[nf][m2] >= fz && r2.level[nf][m2] + lsz <= fz + sz);
                    }
                }
            }
        }
        printf("fuzz: 30000 mutated headers, %u accepted, all inside their buffers\n", (unsigned)accepted);
    }
}

int main(void) {
    test_formats();
    test_half();
    test_bc6h();
    test_parse();
    printf("test_asset_dds: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail != 0 ? 1 : 0;
}
