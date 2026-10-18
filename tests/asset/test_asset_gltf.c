/* glTF importer and cook step (su_asset_cook_gltf) on files this test writes itself: .gltf with external .bin and
 * PNG, .glb with an embedded buffer and a PNG in a buffer view, data-URI buffers, KTX2 (RGBA8, BC1 with mips, and a
 * supercompressed one that must be refused), node hierarchies, LOD-suffixed names, a mirrored node, flat-normal
 * generation, strips and fans, tangent generation, sRGB-correct mips, stable ids, determinism, malformed inputs, and
 * the background loader with the importer installed. No downloaded or third-party model is used.
 *
 * Usage: test_asset_gltf WORKDIR   (created if missing; files are written there) */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

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

static char g_dir[512];

static const char *path_of(const char *name) {
    static char buf[4][1024];
    static int k = 0;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "%s/%s", g_dir, name);
    return buf[k];
}

static void write_file(const char *name, const void *b, size_t n) {
    FILE *f = fopen(path_of(name), "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(fwrite(b, 1, n, f) == n);
        fclose(f);
    }
}

/* ---------- PNG (stored deflate) ---------- */

static uint32_t crc_table[256];

static void crc_init(void) {
    uint32_t n;
    for (n = 0; n < 256u; n++) {
        uint32_t c = n;
        int k;
        for (k = 0; k < 8; k++) {
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

static uint32_t crc32_of(const uint8_t *b, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ b[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t len) {
    be32(out, len);
    memcpy(out + 4, type, 4);
    if (len > 0u) {
        memcpy(out + 8, data, len);
    }
    be32(out + 8 + len, crc32_of(out + 4, len + 4u));
    return 12u + len;
}

/* RGBA8 image -> PNG bytes (malloc'd). */
static uint8_t *make_png(const uint8_t *rgba, uint32_t w, uint32_t h, size_t *len) {
    size_t raw_len = (size_t)h * (1u + w * 4u);
    size_t z_len = 2u + 5u * (raw_len / 65535u + 1u) + raw_len + 4u;
    uint8_t *raw = malloc(raw_len);
    uint8_t *z = malloc(z_len);
    uint8_t *out = malloc(z_len + 128u);
    uint8_t ihdr[13];
    size_t o = 0;
    size_t zo = 0;
    size_t i;
    uint32_t a = 1;
    uint32_t b = 0;
    uint32_t y;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (y = 0; y < h; y++) {
        raw[(size_t)y * (1u + w * 4u)] = 0;
        memcpy(raw + (size_t)y * (1u + w * 4u) + 1u, rgba + (size_t)y * w * 4u, (size_t)w * 4u);
    }
    z[zo++] = 0x78;
    z[zo++] = 0x01;
    for (i = 0; i < raw_len || i == 0u; i += 65535u) {
        size_t n = raw_len - i < 65535u ? raw_len - i : 65535u;
        z[zo++] = (uint8_t)(i + n >= raw_len ? 1u : 0u);
        z[zo++] = (uint8_t)n;
        z[zo++] = (uint8_t)(n >> 8);
        z[zo++] = (uint8_t)~n;
        z[zo++] = (uint8_t)(~n >> 8);
        memcpy(z + zo, raw + i, n);
        zo += n;
        if (raw_len == 0u) {
            break;
        }
    }
    for (i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    be32(z + zo, (b << 16) | a);
    zo += 4u;
    memcpy(out, sig, 8);
    o = 8;
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    o += png_chunk(out + o, "IHDR", ihdr, 13);
    o += png_chunk(out + o, "IDAT", z, (uint32_t)zo);
    o += png_chunk(out + o, "IEND", NULL, 0);
    free(raw);
    free(z);
    *len = o;
    return out;
}

/* ---------- KTX2 ---------- */

static void le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void le64(uint8_t *p, uint64_t v) {
    le32(p, (uint32_t)v);
    le32(p + 4, (uint32_t)(v >> 32));
}

/* A minimal KTX2: header, level index, then the levels (smallest last); no DFD/KVD (offsets 0). */
static uint8_t *make_ktx2(uint32_t vkformat, uint32_t w, uint32_t h, uint32_t levels, const uint8_t *const *data,
                          const size_t *sizes, uint32_t scheme, size_t *len) {
    static const uint8_t id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    size_t total = 80u + 24u * levels;
    size_t at;
    uint32_t i;
    uint8_t *b;
    for (i = 0; i < levels; i++) {
        total += sizes[i] + 8u;
    }
    b = calloc(1, total);
    memcpy(b, id, 12);
    le32(b + 12, vkformat);
    le32(b + 16, 1);
    le32(b + 20, w);
    le32(b + 24, h);
    le32(b + 28, 0);
    le32(b + 32, 0);
    le32(b + 36, 1);
    le32(b + 40, levels);
    le32(b + 44, scheme);
    at = 80u + 24u * levels;
    for (i = 0; i < levels; i++) {
        at = (at + 7u) & ~(size_t)7u;
        le64(b + 80 + 24 * i, at);
        le64(b + 80 + 24 * i + 8, sizes[i]);
        le64(b + 80 + 24 * i + 16, sizes[i]);
        memcpy(b + at, data[i], sizes[i]);
        at += sizes[i];
    }
    *len = at;
    return b;
}

/* ---------- glTF builder ---------- */

typedef struct G {
    uint8_t bin[1 << 16];
    size_t n;
    char views[16384];
    int nviews;
    char accs[16384];
    int naccs;
} G;

static void appendf(char *dst, size_t cap, const char *fmt, ...) {
    size_t l = strlen(dst);
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(dst + l, cap - l, fmt, ap);
    va_end(ap);
}

static int add_view(G *g, const void *data, size_t len) {
    g->n = (g->n + 3u) & ~(size_t)3u;
    memcpy(g->bin + g->n, data, len);
    appendf(g->views, sizeof g->views, "%s{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}", g->nviews ? "," : "",
            (unsigned)g->n, (unsigned)len);
    g->n += len;
    return g->nviews++;
}

/* comp: 5126 float, 5123 u16, 5125 u32. */
static int add_acc(G *g, const void *data, size_t len, int comp, int count, const char *type, const char *extra) {
    int v = add_view(g, data, len);
    appendf(g->accs, sizeof g->accs, "%s{\"bufferView\":%d,\"componentType\":%d,\"count\":%d,\"type\":\"%s\"%s}",
            g->naccs ? "," : "", v, comp, count, type, extra != NULL ? extra : "");
    return g->naccs++;
}

static int pos_acc(G *g, const float *p, int count) {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    char mm[256];
    int i;
    int k;
    for (i = 0; i < count; i++) {
        for (k = 0; k < 3; k++) {
            lo[k] = p[i * 3 + k] < lo[k] ? p[i * 3 + k] : lo[k];
            hi[k] = p[i * 3 + k] > hi[k] ? p[i * 3 + k] : hi[k];
        }
    }
    snprintf(mm, sizeof mm, ",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    return add_acc(g, p, (size_t)count * 12u, 5126, count, "VEC3", mm);
}

static char *gltf_json(const G *g, const char *buffer_uri, const char *rest) {
    char *j = malloc(1 << 16);
    j[0] = '\0';
    appendf(j, 1 << 16, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"sutekh test\"},\"buffers\":[{\"byteLength\":%u%s%s%s}],"
                        "\"bufferViews\":[%s],\"accessors\":[%s],%s}",
            (unsigned)g->n, buffer_uri ? ",\"uri\":\"" : "", buffer_uri ? buffer_uri : "", buffer_uri ? "\"" : "",
            g->views, g->accs, rest);
    return j;
}

static uint8_t *make_glb(const char *json, const uint8_t *bin, size_t bin_len, size_t *len) {
    size_t jl = strlen(json);
    size_t jp = (jl + 3u) & ~(size_t)3u;
    size_t bp = (bin_len + 3u) & ~(size_t)3u;
    size_t total = 12u + 8u + jp + 8u + bp;
    uint8_t *b = calloc(1, total);
    le32(b, 0x46546C67u);
    le32(b + 4, 2);
    le32(b + 8, (uint32_t)total);
    le32(b + 12, (uint32_t)jp);
    le32(b + 16, 0x4E4F534Au);
    memcpy(b + 20, json, jl);
    memset(b + 20 + jl, ' ', jp - jl);
    le32(b + 20 + jp, (uint32_t)bp);
    le32(b + 24 + jp, 0x004E4942u);
    memcpy(b + 28 + jp, bin, bin_len);
    *len = total;
    return b;
}

static char *base64(const uint8_t *b, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc(n / 3u * 4u + 8u);
    size_t i;
    size_t k = 0;
    for (i = 0; i + 2u < n; i += 3u) {
        uint32_t v = ((uint32_t)b[i] << 16) | ((uint32_t)b[i + 1] << 8) | b[i + 2];
        o[k++] = t[v >> 18];
        o[k++] = t[(v >> 12) & 63u];
        o[k++] = t[(v >> 6) & 63u];
        o[k++] = t[v & 63u];
    }
    if (n - i == 1u) {
        uint32_t v = (uint32_t)b[i] << 16;
        o[k++] = t[v >> 18];
        o[k++] = t[(v >> 12) & 63u];
        o[k++] = '=';
        o[k++] = '=';
    } else if (n - i == 2u) {
        uint32_t v = ((uint32_t)b[i] << 16) | ((uint32_t)b[i + 1] << 8);
        o[k++] = t[v >> 18];
        o[k++] = t[(v >> 12) & 63u];
        o[k++] = t[(v >> 6) & 63u];
        o[k++] = '=';
    }
    o[k] = '\0';
    return o;
}

/* ---------- helpers on the cooked pack ---------- */

static SuAssetPak *cook(const char *file, const char *name, SuAssetCookReport *rep, uint32_t *status, char *msg) {
    SuAssetCookOptions o;
    uint8_t *b = NULL;
    size_t len = 0;
    SuAssetPak *p = NULL;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.name = name;
    *status = su_asset_cook_gltf(path_of(file), &o, &b, &len, rep, msg);
    if (*status == SU_OK) {
        uint32_t st = su_asset_pak_open(b, len, 0, &p, msg);
        CHECK(st == SU_OK);
        if (st != SU_OK) {
            fprintf(stderr, "cooked pack does not open: %s\n", msg);
        }
    }
    free(b);
    return p;
}

static uint64_t idf(const char *fmt, const char *a) {
    char buf[512];
    snprintf(buf, sizeof buf, fmt, a);
    return su_asset_id(buf);
}

static float srgb_to_linear(float s) {
    return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
}

/* ---------- scene A: .gltf + .bin + PNG, hierarchy, materials, generated tangents, mips ---------- */

static const float QUAD_POS[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
static const float QUAD_NRM[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
static const float QUAD_UV[8] = {0, 1, 1, 1, 1, 0, 0, 0};
static const uint16_t QUAD_IDX[6] = {0, 1, 2, 0, 2, 3};

static void write_scene_a(float x0) {
    G *g = calloc(1, sizeof *g);
    uint8_t px[4 * 4 * 4];
    size_t png_len = 0;
    uint8_t *png;
    char rest[4096];
    char *json;
    float pos[12];
    int ap;
    int an;
    int au;
    int ai;
    int i;
    memcpy(pos, QUAD_POS, sizeof pos);
    pos[0] = x0;
    for (i = 0; i < 16; i++) {
        int checker = ((i % 4) + (i / 4)) & 1;
        px[i * 4] = checker ? 0 : 255;
        px[i * 4 + 1] = checker ? 255 : 0;
        px[i * 4 + 2] = 0;
        px[i * 4 + 3] = 255;
    }
    png = make_png(px, 4, 4, &png_len);
    write_file("checker.png", png, png_len);
    free(png);
    ap = pos_acc(g, pos, 4);
    an = add_acc(g, QUAD_NRM, sizeof QUAD_NRM, 5126, 4, "VEC3", NULL);
    au = add_acc(g, QUAD_UV, sizeof QUAD_UV, 5126, 4, "VEC2", NULL);
    ai = add_acc(g, QUAD_IDX, sizeof QUAD_IDX, 5123, 6, "SCALAR", NULL);
    snprintf(rest, sizeof rest,
             "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],"
             "\"meshes\":[{\"name\":\"panel\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"TEXCOORD_0\":%d},"
             "\"indices\":%d,\"material\":0}]}],"
             "\"materials\":[{\"name\":\"hull\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.5,0.25,1],"
             "\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.75,\"roughnessFactor\":0.375},"
             "\"emissiveFactor\":[1,0.5,0],\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4}},"
             "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.25,\"doubleSided\":true}],"
             "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33648}],"
             "\"textures\":[{\"source\":0,\"sampler\":0}],"
             "\"images\":[{\"uri\":\"checker.png\"}],"
             "\"nodes\":[{\"name\":\"root\",\"translation\":[5,0,0],\"scale\":[2,2,2],\"children\":[1],\"mesh\":0},"
             "{\"name\":\"child\",\"translation\":[0,1,0],\"mesh\":0}],"
             "\"scenes\":[{\"nodes\":[0]}],\"scene\":0",
             ap, an, au, ai);
    json = gltf_json(g, "scene_a.bin", rest);
    write_file("scene_a.bin", g->bin, g->n);
    write_file("scene_a.gltf", json, strlen(json));
    free(json);
    free(g);
}

/* ---------- scene P: SUTEKH_materials_layered_paint ---------- */

static void test_scene_paint(void) {
    G *g = calloc(1, sizeof *g);
    uint8_t px[4 * 4 * 4];
    size_t png_len = 0;
    uint8_t *png;
    char rest[6144];
    char *json;
    int ap;
    int an;
    int au;
    int ai;
    int i;
    SuAssetCookReport rep;
    uint32_t st;
    char msg[256];
    SuAssetPak *p;
    SuAssetMaterial m;
    SuAssetPaint pt;
    uint64_t mid = su_asset_id("p/mat/craft");
    for (i = 0; i < 16; i++) {
        px[i * 4] = (uint8_t)(i * 16);
        px[i * 4 + 1] = 128;
        px[i * 4 + 2] = 64;
        px[i * 4 + 3] = 255;
    }
    png = make_png(px, 4, 4, &png_len);
    write_file("paint_a.png", png, png_len);
    write_file("paint_n.png", png, png_len);
    write_file("paint_m.png", png, png_len);
    free(png);
    ap = pos_acc(g, QUAD_POS, 4);
    an = add_acc(g, QUAD_NRM, sizeof QUAD_NRM, 5126, 4, "VEC3", NULL);
    au = add_acc(g, QUAD_UV, sizeof QUAD_UV, 5126, 4, "VEC2", NULL);
    ai = add_acc(g, QUAD_IDX, sizeof QUAD_IDX, 5123, 6, "SCALAR", NULL);
    snprintf(rest, sizeof rest,
             "\"extensionsUsed\":[\"SUTEKH_materials_layered_paint\",\"KHR_materials_emissive_strength\"],"
             "\"meshes\":[{\"name\":\"hull\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"TEXCOORD_0\":%d},"
             "\"indices\":%d,\"material\":0}]}],"
             "\"materials\":[{\"name\":\"craft\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.5,0.5,1],"
             "\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.9,\"roughnessFactor\":0.7},"
             "\"normalTexture\":{\"index\":1,\"scale\":0.5},\"emissiveFactor\":[1,0.5,0.25],"
             "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":3},"
             "\"SUTEKH_materials_layered_paint\":{\"maskTexture\":{\"index\":2},\"roughnessTexture\":{\"index\":2,\"texCoord\":1},"
             "\"channels\":{\"roughness\":1,\"metal\":2,\"specular\":3,\"dirt\":0,\"ao\":-1},\"specular\":0.25,"
             "\"dirt\":{\"amount\":0.4,\"darken\":0.7,\"roughen\":0.2},"
             "\"layers\":[{\"color\":[1,0,0],\"roughness\":0.3,\"metallic\":1,\"opacity\":0.8},"
             "{\"color\":[0.5,0.5,1],\"blend\":\"multiply\",\"glowColor\":[0,1,0],\"glowStrength\":4,\"metallic\":0.5,\"roughness\":0.9}],"
             "\"order\":[1,0]}}}],"
             "\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2}],"
             "\"images\":[{\"uri\":\"paint_a.png\"},{\"uri\":\"paint_n.png\"},{\"uri\":\"paint_m.png\"}],"
             "\"nodes\":[{\"name\":\"root\",\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0",
             ap, an, au, ai);
    json = gltf_json(g, "scene_p.bin", rest);
    write_file("scene_p.bin", g->bin, g->n);
    write_file("scene_p.gltf", json, strlen(json));
    free(json);
    free(g);
    p = cook("scene_p.gltf", "p", &rep, &st, msg);
    CHECK(st == SU_OK && p != NULL);
    if (p == NULL) {
        fprintf(stderr, "scene paint: %s\n", msg);
        return;
    }
    CHECK(rep.materials == 1u && rep.warnings == 0u);
    CHECK(su_asset_pak_material(p, mid, &m) == SU_OK && su_asset_pak_paint(p, mid, &pt) == SU_OK);
    CHECK(m.base_color[0] == 0.5f && m.metallic == 0.9f && m.roughness == 0.7f && m.normal_scale == 0.5f &&
          m.emissive[1] == 0.5f && m.emissive_strength == 3.0f);
    CHECK(m.texture[0] == 0u && m.texture[SU_ASSET_SLOT_NORMAL] == 0u); /* the paint slots hold the textures */
    CHECK(pt.texture[SU_PAINT_SLOT_ALBEDO] == su_asset_id("p/tex/paint_a.png.srgb"));
    CHECK(pt.texture[SU_PAINT_SLOT_NORMAL] == su_asset_id("p/tex/paint_n.png.normal"));
    CHECK(pt.texture[SU_PAINT_SLOT_MASK] == su_asset_id("p/tex/paint_m.png.linear"));
    CHECK(pt.texture[SU_PAINT_SLOT_ROUGHNESS] == su_asset_id("p/tex/paint_m.png.linear"));
    CHECK((pt.tex_info[SU_PAINT_SLOT_ROUGHNESS] & 0xFFu) == 1u && (pt.tex_info[SU_PAINT_SLOT_MASK] & 0xFFu) == 0u);
    CHECK(pt.texture[SU_PAINT_SLOT_GLOW] == 0u && pt.texture[SU_PAINT_SLOT_AO] == 0u && pt.texture[SU_PAINT_SLOT_DIRT] == 0u);
    CHECK(pt.channel[SU_PAINT_CH_ROUGHNESS] == 1 && pt.channel[SU_PAINT_CH_METAL] == 2 &&
          pt.channel[SU_PAINT_CH_SPECULAR] == 3 && pt.channel[SU_PAINT_CH_DIRT] == 0 &&
          pt.channel[SU_PAINT_CH_AO] == SU_PAINT_CH_NONE);
    CHECK(pt.specular == 0.25f && pt.dirt_amount == 0.4f && pt.dirt_darken == 0.7f && pt.dirt_roughen == 0.2f);
    CHECK(pt.layer_count == 2u && pt.layer_order[0] == 1 && pt.layer_order[1] == 0);
    CHECK(pt.layer[0].color[0] == 1.0f && pt.layer[0].roughness == 0.3f && pt.layer[0].metallic == 1.0f &&
          pt.layer[0].opacity == 0.8f && pt.layer[0].flags == 0u);
    CHECK(pt.layer[1].flags == (SU_PAINT_LAYER_MULTIPLY | SU_PAINT_LAYER_GLOW_TINT) && pt.layer[1].glow_color[1] == 1.0f &&
          pt.layer[1].glow_strength == 4.0f && pt.layer[1].opacity == 1.0f);
    {
        SuAssetTextureView tv;
        CHECK(su_asset_pak_texture(p, pt.texture[SU_PAINT_SLOT_NORMAL], &tv) == SU_OK && (tv.header->format == SU_TEX_BC5_UNORM || tv.header->format == SU_TEX_RGBA8_UNORM)); /* linear, 2-channel capable */
        CHECK(su_asset_pak_texture(p, pt.texture[SU_PAINT_SLOT_ALBEDO], &tv) == SU_OK && tv.header->format != SU_TEX_RGBA8_UNORM);
    }
    su_asset_pak_close(p);
}
static void test_scene_a(void) {
    SuAssetCookReport rep;
    uint32_t st;
    char msg[256];
    SuAssetPak *p;
    SuAssetMeshView mv;
    SuAssetMaterial m;
    SuAssetTextureView tv;
    SuAssetModelView model;
    uint8_t *b1 = NULL;
    uint8_t *b2 = NULL;
    size_t l1 = 0;
    size_t l2 = 0;
    uint32_t i;
    write_scene_a(-1.0f);
    p = cook("scene_a.gltf", "a", &rep, &st, msg);
    CHECK(st == SU_OK && p != NULL);
    if (p == NULL) {
        fprintf(stderr, "scene a: %s\n", msg);
        return;
    }
    CHECK(rep.meshes == 1u && rep.materials == 1u && rep.textures == 1u && rep.model_nodes == 2u && rep.warnings == 0u);
    CHECK(su_asset_pak_mesh(p, su_asset_id("a/mesh/panel"), &mv) == SU_OK);
    CHECK(mv.header->vertex_count == 4u && mv.header->index_count == 6u && mv.header->submesh_count == 1u);
    CHECK(mv.submeshes[0].material == su_asset_id("a/mat/hull"));
    /* generated tangents: u grows along +x, v grows along -y (glTF UV origin top-left): tangent +x, sign +1 */
    for (i = 0; i < 4u; i++) {
        const SuAssetVertex *v = &mv.vertices[i];
        CHECK(fabsf(v->tangent[0] - 1.0f) < 1e-5f && fabsf(v->tangent[1]) < 1e-5f && fabsf(v->tangent[2]) < 1e-5f);
        CHECK(v->tangent[3] == 1.0f || v->tangent[3] == -1.0f);
    }
    /* bitangent = cross(n, t) * w = (0,1,0) * w must point along dP/dv = -y -> w = -1 */
    CHECK(mv.vertices[0].tangent[3] == -1.0f);
    CHECK(su_asset_pak_material(p, su_asset_id("a/mat/hull"), &m) == SU_OK);
    CHECK(m.base_color[1] == 0.5f && m.metallic == 0.75f && m.roughness == 0.375f && m.emissive[1] == 0.5f &&
          m.emissive_strength == 4.0f && m.alpha_mode == SU_ASSET_ALPHA_MASK && m.alpha_cutoff == 0.25f &&
          m.double_sided == 1u);
    CHECK(m.texture[SU_ASSET_SLOT_BASE_COLOR] == su_asset_id("a/tex/checker.png.srgb"));
    CHECK(m.tex_info[SU_ASSET_SLOT_BASE_COLOR] == SU_ASSET_TEX_INFO(0, SU_ASSET_WRAP_CLAMP, SU_ASSET_WRAP_MIRROR));
    CHECK(su_asset_pak_texture(p, m.texture[0], &tv) == SU_OK);
    CHECK(tv.header->width == 4u && tv.header->height == 4u && tv.header->mip_count == 3u &&
          tv.header->format == SU_TEX_RGBA8_SRGB);
    CHECK(tv.mip_data[0][0] == 255u && tv.mip_data[0][1] == 0u && tv.mip_data[0][5] == 255u);
    {
        /* 2x2 average of red and green in linear light is (0.5, 0.5, 0): sRGB 188 each, not 128 */
        const uint8_t *q = tv.mip_data[1];
        CHECK(q[0] == 188u && q[1] == 188u && q[2] == 0u && q[3] == 255u);
        CHECK(fabsf(srgb_to_linear((float)q[0] / 255.0f) - 0.5f) < 0.01f);
        CHECK(tv.mip_data[2][0] == 188u);
    }
    CHECK(su_asset_pak_model(p, su_asset_id("a/model"), &model) == SU_OK);
    CHECK(model.header->node_count == 2u);
    if (model.header->node_count == 2u) {
        /* root: translation 5, scale 2; child: translation (0,1,0) inside the root -> world (5,2,0), scale 2 */
        CHECK(model.nodes[0].transform[0] == 2.0f && model.nodes[0].transform[12] == 5.0f);
        CHECK(model.nodes[1].transform[12] == 5.0f && model.nodes[1].transform[13] == 2.0f &&
              model.nodes[1].transform[5] == 2.0f);
        CHECK(model.nodes[0].lod_count == 1u && model.nodes[0].lod_mesh[0] == mv.id);
        CHECK(model.nodes[1].name_hash == (uint32_t)su_asset_id("child"));
    }
    su_asset_pak_close(p);

    /* determinism and stable ids */
    {
        SuAssetCookOptions o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.name = "a";
        CHECK(su_asset_cook_gltf(path_of("scene_a.gltf"), &o, &b1, &l1, NULL, msg) == SU_OK);
        CHECK(su_asset_cook_gltf(path_of("scene_a.gltf"), &o, &b2, &l2, NULL, msg) == SU_OK);
        CHECK(l1 == l2 && b1 != NULL && b2 != NULL && memcmp(b1, b2, l1) == 0);
        free(b2);
        b2 = NULL;
        write_scene_a(-1.5f); /* move one vertex: same ids, different mesh hash */
        CHECK(su_asset_cook_gltf(path_of("scene_a.gltf"), &o, &b2, &l2, NULL, msg) == SU_OK);
        if (b1 != NULL && b2 != NULL) {
            SuAssetPak *p1 = NULL;
            SuAssetPak *p2 = NULL;
            uint32_t k1 = 0;
            uint32_t k2 = 0;
            SuAssetEntry e1;
            SuAssetEntry e2;
            CHECK(su_asset_pak_open(b1, l1, 0, &p1, msg) == SU_OK && su_asset_pak_open(b2, l2, 0, &p2, msg) == SU_OK);
            CHECK(su_asset_pak_find(p1, su_asset_id("a/mesh/panel"), SU_ASSET_MESH, &k1) == SU_OK &&
                  su_asset_pak_find(p2, su_asset_id("a/mesh/panel"), SU_ASSET_MESH, &k2) == SU_OK);
            (void)su_asset_pak_entry(p1, k1, &e1);
            (void)su_asset_pak_entry(p2, k2, &e2);
            CHECK(memcmp(e1.sha256, e2.sha256, 32) != 0);
            CHECK(su_asset_pak_find(p2, su_asset_id("a/mat/hull"), SU_ASSET_MATERIAL, &k2) == SU_OK);
            su_asset_pak_close(p1);
            su_asset_pak_close(p2);
        }
        free(b1);
        free(b2);
    }
}

/* ---------- scene B: GLB, embedded PNG normal map, no normals (flat), strip, fan ---------- */

static void test_scene_b(void) {
    G *g = calloc(1, sizeof *g);
    static const float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    static const float strip[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    static const float fan[15] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, -1, 1, 0};
    uint8_t px[2 * 2 * 4];
    size_t png_len = 0;
    uint8_t *png;
    int img_view;
    char rest[4096];
    char *json;
    uint8_t *glb;
    size_t glb_len = 0;
    SuAssetCookReport rep;
    uint32_t st;
    char msg[256];
    SuAssetPak *p;
    SuAssetMeshView mv;
    SuAssetMaterial m;
    SuAssetTextureView tv;
    int a0;
    int a1;
    int a2;
    uint32_t i;
    /* a normal map: tilted normals that average to +z */
    px[0] = 200; px[1] = 128; px[2] = 200; px[3] = 255;
    px[4] = 56; px[5] = 128; px[6] = 200; px[7] = 255;
    px[8] = 128; px[9] = 200; px[10] = 200; px[11] = 255;
    px[12] = 128; px[13] = 56; px[14] = 200; px[15] = 255;
    png = make_png(px, 2, 2, &png_len);
    a0 = pos_acc(g, tri, 3);
    a1 = pos_acc(g, strip, 4);
    a2 = pos_acc(g, fan, 5);
    img_view = add_view(g, png, png_len);
    free(png);
    snprintf(rest, sizeof rest,
             "\"meshes\":[{\"name\":\"bits\",\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"material\":0},"
             "{\"attributes\":{\"POSITION\":%d},\"mode\":5},{\"attributes\":{\"POSITION\":%d},\"mode\":6},"
             "{\"attributes\":{\"POSITION\":%d},\"mode\":1}]}],"
             "\"materials\":[{\"normalTexture\":{\"index\":0,\"scale\":0.5}}],"
             "\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":%d,\"mimeType\":\"image/png\"}],"
             "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
             a0, a1, a2, a0, img_view);
    json = gltf_json(g, NULL, rest);
    glb = make_glb(json, g->bin, g->n, &glb_len);
    write_file("scene_b.glb", glb, glb_len);
    p = cook("scene_b.glb", "b", &rep, &st, msg);
    CHECK(st == SU_OK && p != NULL);
    if (p != NULL) {
        CHECK(rep.warnings == 1u && strstr(rep.first_warning, "lines") != NULL); /* the mode 1 primitive */
        CHECK(su_asset_pak_mesh(p, su_asset_id("b/mesh/bits"), &mv) == SU_OK);
        /* flat normals: 1 + 2 + 3 triangles, each unwelded into 3 vertices, all facing +z (counter-clockwise) */
        CHECK(mv.header->submesh_count == 3u && mv.header->index_count == 18u && mv.header->vertex_count == 18u);
        for (i = 0; i < mv.header->vertex_count; i++) {
            CHECK(fabsf(mv.vertices[i].normal[2] - 1.0f) < 1e-6f);
        }
        CHECK(mv.submeshes[1].material == 0u && mv.submeshes[0].material == su_asset_id("b/mat/#0"));
        CHECK(su_asset_pak_material(p, mv.submeshes[0].material, &m) == SU_OK);
        CHECK(m.normal_scale == 0.5f && m.texture[SU_ASSET_SLOT_NORMAL] == su_asset_id("b/tex/#0.normal"));
        CHECK(su_asset_pak_texture(p, m.texture[SU_ASSET_SLOT_NORMAL], &tv) == SU_OK);
        CHECK(tv.header->format == SU_TEX_RGBA8_UNORM && (tv.header->flags & SU_ASSET_TEX_FLAG_NORMAL_MAP) != 0u &&
              tv.header->mip_count == 2u);
        /* the 1x1 mip of four opposing tilts averages to (128, 128, 200), which is renormalized to +z (128, 128, 255) */
        CHECK(tv.mip_data[1][0] == 128u && tv.mip_data[1][1] == 128u && tv.mip_data[1][2] == 255u);
        su_asset_pak_close(p);
    }
    /* the same file cooked with SU_ASSET_COOK_BC: the normal map becomes BC5 (2 x 2 -> one block per level) */
    {
        SuAssetCookOptions o;
        uint8_t *b = NULL;
        size_t bl = 0;
        SuAssetPak *bp = NULL;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.name = "b";
        o.flags = SU_ASSET_COOK_BC;
        CHECK(su_asset_cook_gltf(path_of("scene_b.glb"), &o, &b, &bl, &rep, msg) == SU_OK);
        CHECK(b != NULL && su_asset_pak_open(b, bl, 0, &bp, msg) == SU_OK);
        if (bp != NULL) {
            CHECK(su_asset_pak_texture(bp, su_asset_id("b/tex/#0.normal"), &tv) == SU_OK);
            CHECK(tv.header->format == SU_TEX_BC5_UNORM && tv.header->mip_count == 2u && tv.mip_size[0] == 16u &&
                  (tv.header->flags & SU_ASSET_TEX_FLAG_NORMAL_MAP) != 0u);
            CHECK(rep.texture_bytes == 32u);
            su_asset_pak_close(bp);
        }
        free(b);
    }
    /* truncated GLB */
    write_file("scene_b_cut.glb", glb, glb_len - 40u);
    p = cook("scene_b_cut.glb", "b", &rep, &st, msg);
    CHECK(st == SU_E_PARSE && p == NULL);
    free(glb);
    free(json);
    free(g);
}

/* ---------- scene C: LOD names, mirrored node, data-URI buffer ---------- */

static void test_scene_c(void) {
    G *g = calloc(1, sizeof *g);
    char *rest = malloc(1 << 15);
    char *json;
    char *uri;
    char *b64;
    int ap;
    int an;
    int ai;
    SuAssetCookReport rep;
    uint32_t st;
    char msg[256];
    SuAssetPak *p;
    SuAssetModelView model;
    SuAssetMeshView mv;
    SuAssetMeshView mm = {0};
    uint32_t i;
    ap = pos_acc(g, QUAD_POS, 4);
    an = add_acc(g, QUAD_NRM, sizeof QUAD_NRM, 5126, 4, "VEC3", NULL);
    ai = add_acc(g, QUAD_IDX, sizeof QUAD_IDX, 5123, 6, "SCALAR", NULL);
    snprintf(rest, 1 << 15,
             "\"meshes\":[{\"name\":\"hi\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d}]},"
             "{\"name\":\"mid\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d}]},"
             "{\"name\":\"lo\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d}]}],"
             "\"nodes\":[{\"name\":\"Rock_LOD2\",\"mesh\":2},{\"name\":\"Rock_LOD0\",\"mesh\":0,\"translation\":[0,0,-50]},"
             "{\"name\":\"Rock_LOD1\",\"mesh\":1},{\"name\":\"Mirror\",\"mesh\":0,\"scale\":[-1,1,1]},"
             "{\"name\":\"Plain\",\"mesh\":1},{\"name\":\"Rock_LOD0\",\"mesh\":0,\"translation\":[9,0,0]}],"
             "\"scenes\":[{\"nodes\":[0,1,2,3,4,5]}]",
             ap, an, ai, ap, an, ai, ap, an, ai);
    b64 = base64(g->bin, g->n);
    uri = malloc(strlen(b64) + 64u);
    sprintf(uri, "data:application/octet-stream;base64,%s", b64);
    json = gltf_json(g, uri, rest);
    write_file("scene_c.gltf", json, strlen(json));
    p = cook("scene_c.gltf", "c", &rep, &st, msg);
    CHECK(st == SU_OK && p != NULL);
    if (p != NULL) {
        CHECK(su_asset_pak_model(p, su_asset_id("c/model"), &model) == SU_OK);
        /* Rock group (LOD 0, 1, 2), Mirror, Plain, the second Rock_LOD0 placement */
        CHECK(model.header->node_count == 4u && rep.lod_groups == 1u);
        if (model.header->node_count == 4u) {
            const SuAssetModelNode *rock = &model.nodes[0];
            CHECK(rock->lod_count == 3u && rock->lod_mesh[0] == su_asset_id("c/mesh/hi") &&
                  rock->lod_mesh[1] == su_asset_id("c/mesh/mid") && rock->lod_mesh[2] == su_asset_id("c/mesh/lo"));
            CHECK(rock->transform[14] == -50.0f); /* the group takes LOD 0's placement */
            CHECK(rock->lod_screen[0] == 1.0f && rock->lod_screen[1] == 0.25f && rock->lod_screen[2] == 0.08f);
            CHECK(rock->name_hash == (uint32_t)su_asset_id("Rock"));
            CHECK(model.nodes[1].lod_mesh[0] == su_asset_id("c/mesh/hi#mirrored") && model.nodes[1].transform[0] == -1.0f);
            CHECK(model.nodes[2].lod_count == 1u && model.nodes[2].lod_mesh[0] == su_asset_id("c/mesh/mid"));
            CHECK(model.nodes[3].lod_count == 1u && model.nodes[3].transform[12] == 9.0f);
        }
        CHECK(su_asset_pak_mesh(p, su_asset_id("c/mesh/hi"), &mv) == SU_OK &&
              su_asset_pak_mesh(p, su_asset_id("c/mesh/hi#mirrored"), &mm) == SU_OK);
        for (i = 0; i < 6u; i += 3u) {
            CHECK(mm.indices[i] == mv.indices[i] && mm.indices[i + 1u] == mv.indices[i + 2u] &&
                  mm.indices[i + 2u] == mv.indices[i + 1u]);
        }
        CHECK(mm.vertices[0].tangent[3] == -mv.vertices[0].tangent[3]);
        su_asset_pak_close(p);
    }
    free(json);
    free(uri);
    free(b64);
    free(rest);
    free(g);
}

/* ---------- scene D: KTX2 textures ---------- */

static void test_scene_d(void) {
    G *g = calloc(1, sizeof *g);
    uint8_t rgba[4 * 2 * 4];
    uint8_t bc0[2 * 1 * 8];
    uint8_t bc1[8];
    const uint8_t *lv[2];
    size_t sz[2];
    size_t len = 0;
    uint8_t *k;
    char rest[4096];
    char *json;
    int ap;
    int au;
    int ai;
    SuAssetCookReport rep;
    uint32_t st;
    char msg[256];
    SuAssetPak *p;
    SuAssetMaterial m;
    SuAssetTextureView tv;
    int i;
    for (i = 0; i < (int)sizeof rgba; i++) {
        rgba[i] = (uint8_t)(i * 9);
    }
    for (i = 0; i < (int)sizeof bc0; i++) {
        bc0[i] = (uint8_t)(0xA0 + i);
    }
    memset(bc1, 0x5C, sizeof bc1);
    lv[0] = rgba;
    sz[0] = sizeof rgba;
    k = make_ktx2(43, 4, 2, 1, lv, sz, 0, &len); /* R8G8B8A8_SRGB, one level: mips generated */
    write_file("base.ktx2", k, len);
    free(k);
    lv[0] = bc0;
    lv[1] = bc1;
    sz[0] = sizeof bc0;
    sz[1] = sizeof bc1;
    k = make_ktx2(134, 8, 4, 2, lv, sz, 0, &len); /* BC1_RGBA_SRGB 8x4 with 2 levels, kept as stored */
    write_file("glow.ktx2", k, len);
    free(k);
    k = make_ktx2(134, 8, 4, 2, lv, sz, 1, &len); /* Basis-supercompressed: refused */
    write_file("basis.ktx2", k, len);
    free(k);
    ap = pos_acc(g, QUAD_POS, 4);
    au = add_acc(g, QUAD_UV, sizeof QUAD_UV, 5126, 4, "VEC2", NULL);
    ai = add_acc(g, QUAD_IDX, sizeof QUAD_IDX, 5123, 6, "SCALAR", NULL);
    snprintf(rest, sizeof rest,
             "\"extensionsUsed\":[\"KHR_texture_basisu\"],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"TEXCOORD_0\":%d},\"indices\":%d,\"material\":0}]}],"
             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
             "\"metallicRoughnessTexture\":{\"index\":2}},\"emissiveTexture\":{\"index\":1},\"emissiveFactor\":[1,1,1]}],"
             "\"textures\":[{\"source\":0},{\"extensions\":{\"KHR_texture_basisu\":{\"source\":1}}},{\"source\":2}],"
             "\"images\":[{\"uri\":\"base.ktx2\",\"mimeType\":\"image/ktx2\"},{\"uri\":\"glow.ktx2\",\"mimeType\":\"image/ktx2\"},"
             "{\"uri\":\"basis.ktx2\",\"mimeType\":\"image/ktx2\"}],"
             "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
             ap, au, ai);
    json = gltf_json(g, "scene_d.bin", rest);
    write_file("scene_d.bin", g->bin, g->n);
    write_file("scene_d.gltf", json, strlen(json));
    p = cook("scene_d.gltf", "d", &rep, &st, msg);
    CHECK(st == SU_OK && p != NULL);
    if (p != NULL) {
        CHECK(su_asset_pak_material(p, su_asset_id("d/mat/#0"), &m) == SU_OK);
        CHECK(su_asset_pak_texture(p, m.texture[SU_ASSET_SLOT_BASE_COLOR], &tv) == SU_OK);
        CHECK(tv.header->format == SU_TEX_RGBA8_SRGB && tv.header->width == 4u && tv.header->mip_count == 3u);
        CHECK(memcmp(tv.mip_data[0], rgba, sizeof rgba) == 0);
        CHECK(su_asset_pak_texture(p, m.texture[SU_ASSET_SLOT_EMISSIVE], &tv) == SU_OK);
        CHECK(tv.header->format == SU_TEX_BC1_SRGB && tv.header->mip_count == 2u && tv.mip_size[0] == 16u &&
              memcmp(tv.mip_data[1], bc1, 8) == 0);
        /* the supercompressed file: a warning, and the slot stays empty */
        CHECK(m.texture[SU_ASSET_SLOT_METAL_ROUGH] == 0u);
        CHECK(rep.warnings >= 1u && strstr(rep.first_warning, "supercompressed") != NULL);
        su_asset_pak_close(p);
    }
    free(json);
    free(g);
}

/* ---------- malformed inputs ---------- */

static void test_errors(void) {
    G *g = calloc(1, sizeof *g);
    static const uint16_t bad_idx[3] = {0, 1, 9};
    char rest[2048];
    char *json;
    SuAssetCookReport rep;
    uint32_t st;
    char msg[256];
    SuAssetPak *p;
    int ap;
    int ai;
    ap = pos_acc(g, QUAD_POS, 4);
    ai = add_acc(g, bad_idx, sizeof bad_idx, 5123, 3, "SCALAR", NULL);
    snprintf(rest, sizeof rest,
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d,\"material\":0}]}],"
             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
             "\"textures\":[{\"source\":0}],\"images\":[{\"uri\":\"../outside.png\"}],"
             "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
             ap, ai);
    json = gltf_json(g, "err.bin", rest);
    write_file("err.bin", g->bin, g->n);
    write_file("err_index.gltf", json, strlen(json));
    free(json);
    p = cook("err_index.gltf", "e", &rep, &st, msg);
    CHECK(st == SU_E_PARSE && p == NULL && strstr(msg, "out of range") != NULL); /* cgltf_validate bounds indices */
    /* an accessor past the end of its buffer view */
    write_file("err_short.bin", g->bin, 8);
    {
        char *j2 = gltf_json(g, "err_short.bin", rest);
        char *at = strstr(j2, "\"byteLength\":");
        if (at != NULL) {
            memcpy(at, "\"byteLength\":8 ", 15); /* buffer claims 8 bytes; views reach further */
            {
                char *q = at + 15;
                while (*q >= '0' && *q <= '9') {
                    *q++ = ' ';
                }
            }
        }
        write_file("err_short.gltf", j2, strlen(j2));
        free(j2);
    }
    p = cook("err_short.gltf", "e", &rep, &st, msg);
    CHECK(st != SU_OK && p == NULL);
    p = cook("does_not_exist.gltf", "e", &rep, &st, msg);
    CHECK(st == SU_E_IO && p == NULL);
    write_file("garbage.gltf", "{\"asset\":", 9);
    p = cook("garbage.gltf", "e", &rep, &st, msg);
    CHECK(st == SU_E_PARSE && p == NULL);
    /* a path escaping the source directory is not read: the cook succeeds with a warning and an empty slot */
    {
        G *g2 = calloc(1, sizeof *g2);
        char *j;
        int a2 = pos_acc(g2, QUAD_POS, 4);
        int i2 = add_acc(g2, QUAD_IDX, sizeof QUAD_IDX, 5123, 6, "SCALAR", NULL);
        snprintf(rest, sizeof rest,
                 "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d,\"material\":0}]}],"
                 "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                 "\"textures\":[{\"source\":0}],\"images\":[{\"uri\":\"../outside.png\"}],"
                 "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
                 a2, i2);
        j = gltf_json(g2, "esc.bin", rest);
        write_file("esc.bin", g2->bin, g2->n);
        write_file("esc.gltf", j, strlen(j));
        free(j);
        free(g2);
        p = cook("esc.gltf", "e", &rep, &st, msg);
        CHECK(st == SU_OK && p != NULL && rep.warnings == 1u && rep.textures == 0u);
        if (p != NULL) {
            SuAssetMaterial m;
            CHECK(su_asset_pak_material(p, su_asset_id("e/mat/#0"), &m) == SU_OK && m.texture[0] == 0u);
            su_asset_pak_close(p);
        }
    }
    free(g);
}

static double wall_s(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_loader(void) {
    SuAssetLoader *l = NULL;
    int got = 0;
    double t0 = wall_s();
    CHECK(su_asset_loader_create(su_asset_cook_gltf, &l) == SU_OK);
    CHECK(su_asset_loader_request(l, path_of("scene_b.glb"), 7) == SU_OK);
    while (!got && wall_s() - t0 < 20.0) {
        SuAssetLoadResult r;
        if (su_asset_loader_poll(l, &r)) {
            got = 1;
            CHECK(r.user == 7u && r.status == SU_OK && r.pak != NULL);
            if (r.pak != NULL) {
                SuAssetMeshView mv;
                /* default name: the file name without its extension */
                CHECK(su_asset_pak_mesh(r.pak, idf("%s/mesh/bits", "scene_b"), &mv) == SU_OK);
                su_asset_pak_close(r.pak);
            }
        }
    }
    CHECK(got);
    su_asset_loader_destroy(l);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_asset_gltf WORKDIR\n");
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    (void)MKDIR(g_dir);
    crc_init();
    CHECK(su_asset_import_available() == 1);
    test_scene_a();
    test_scene_paint();
    {
        /* scene A with SU_ASSET_COOK_BC: the opaque base colour becomes BC1 sRGB with the same mip count */
        SuAssetCookOptions o;
        SuAssetCookReport rep;
        uint8_t *b = NULL;
        size_t bl = 0;
        SuAssetPak *bp = NULL;
        SuAssetTextureView tv;
        char msg[256];
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.name = "a";
        o.flags = SU_ASSET_COOK_BC;
        CHECK(su_asset_cook_gltf(path_of("scene_a.gltf"), &o, &b, &bl, &rep, msg) == SU_OK);
        CHECK(b != NULL && su_asset_pak_open(b, bl, 0, &bp, msg) == SU_OK);
        if (bp != NULL) {
            CHECK(su_asset_pak_texture(bp, su_asset_id("a/tex/checker.png.srgb"), &tv) == SU_OK);
            CHECK(tv.header->format == SU_TEX_BC1_SRGB && tv.header->mip_count == 3u && tv.mip_size[0] == 8u &&
                  tv.mip_size[2] == 8u);
            su_asset_pak_close(bp);
        }
        free(b);
    }
    test_scene_b();
    test_scene_c();
    test_scene_d();
    test_errors();
    test_loader();
    printf("test_asset_gltf: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
