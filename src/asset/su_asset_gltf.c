/* glTF 2.0 importer and cook step (sutekh/asset.h, su_asset_cook_gltf). Parsing is cgltf's; decoding PNG/JPEG is
 * stb_image's; everything else (KTX2 reading, flattening, LOD grouping, normals, tangents, mips, the pack) is here.
 * Built only when tools/bootstrap_asset_deps.py has fetched the pinned libraries. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgltf.h"
#include "stb_image.h"
#include "sutekh/asset.h"

#define MAX_NAME 512

typedef struct Cook {
    cgltf_data *data;
    const char *path;
    char prefix[256];
    SuAssetWriter *w;
    SuAssetCookReport *rep;
    const SuAssetCookOptions *opt;
    uint32_t max_tex;
    float lod_screen[SU_ASSET_MAX_LODS];
    /* per glTF mesh: 0 none yet, 1 written, 2 has no triangles; same for the mirrored variant */
    uint8_t *mesh_state;
    uint8_t *mirror_state;
    /* per image x role (0 srgb, 1 linear, 2 normal): 0 not yet, 1 written, 2 failed */
    uint8_t *tex_state;
    uint8_t *mat_done;
    char *msg;
} Cook;

static void msgf(char *msg, const char *fmt, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(msg, 256, fmt, ap);
        va_end(ap);
    }
}

static void warn(Cook *c, const char *fmt, ...) {
    c->rep->warnings++;
    if (c->rep->warnings == 1u) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(c->rep->first_warning, sizeof c->rep->first_warning, fmt, ap);
        va_end(ap);
    }
}

int su_asset_import_available(void) {
    return 1;
}

/* ---------- naming ---------- */

static int name_unique(const char *name, size_t index, const char *(*get)(const cgltf_data *, size_t), const cgltf_data *d,
                       size_t count) {
    size_t i;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const char *o = get(d, i);
        if (i != index && o != NULL && strcmp(o, name) == 0) {
            return 0;
        }
    }
    return 1;
}

static const char *mesh_name(const cgltf_data *d, size_t i) {
    return d->meshes[i].name;
}
static const char *mat_name(const cgltf_data *d, size_t i) {
    return d->materials[i].name;
}
static const char *img_name(const cgltf_data *d, size_t i) {
    return d->images[i].name != NULL ? d->images[i].name : d->images[i].uri;
}

static uint64_t res_id(const Cook *c, const char *kind, const char *name, size_t index, const char *suffix) {
    char buf[MAX_NAME];
    if (name != NULL && strncmp(name, "data:", 5) == 0) {
        name = NULL; /* never name a resource after inline data */
    }
    if (name != NULL) {
        snprintf(buf, sizeof buf, "%s/%s/%s%s", c->prefix, kind, name, suffix);
    } else {
        snprintf(buf, sizeof buf, "%s/%s/#%u%s", c->prefix, kind, (unsigned)index, suffix);
    }
    return su_asset_id(buf);
}

static uint64_t mesh_id(const Cook *c, size_t mi, int mirrored) {
    const cgltf_data *d = c->data;
    const char *n = name_unique(d->meshes[mi].name, mi, mesh_name, d, d->meshes_count) ? d->meshes[mi].name : NULL;
    return res_id(c, "mesh", n, mi, mirrored ? "#mirrored" : "");
}

static uint64_t mat_id(const Cook *c, size_t i) {
    const cgltf_data *d = c->data;
    const char *n = name_unique(d->materials[i].name, i, mat_name, d, d->materials_count) ? d->materials[i].name : NULL;
    return res_id(c, "mat", n, i, "");
}

static const char *ROLE_SUFFIX[3] = {".srgb", ".linear", ".normal"};

static uint64_t tex_id(const Cook *c, size_t i, int role) {
    const cgltf_data *d = c->data;
    const char *n = name_unique(img_name(d, i), i, img_name, d, d->images_count) ? img_name(d, i) : NULL;
    return res_id(c, "tex", n, i, ROLE_SUFFIX[role]);
}

/* ---------- image bytes ---------- */

static int b64v(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+' || ch == '-') {
        return 62;
    }
    if (ch == '/' || ch == '_') {
        return 63;
    }
    return -1;
}

static uint8_t *base64_decode(const char *s, size_t *out_len) {
    size_t n = strlen(s);
    uint8_t *out = malloc(n / 4u * 3u + 3u);
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t i;
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        int v = b64v((unsigned char)s[i]);
        if (s[i] == '=') {
            break;
        }
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = o;
    return out;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)n + 1u);
    if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* Returns the image's encoded bytes; *owned says whether the caller frees them. */
static const uint8_t *image_bytes(Cook *c, const cgltf_image *img, size_t *len, int *owned) {
    *owned = 0;
    if (img->buffer_view != NULL) {
        const uint8_t *p = cgltf_buffer_view_data(img->buffer_view);
        *len = img->buffer_view->size;
        return p;
    }
    if (img->uri == NULL) {
        return NULL;
    }
    if (strncmp(img->uri, "data:", 5) == 0) {
        const char *comma = strchr(img->uri, ',');
        uint8_t *b;
        if (comma == NULL || strstr(img->uri, ";base64,") == NULL) {
            return NULL;
        }
        b = base64_decode(comma + 1, len);
        *owned = b != NULL;
        return b;
    }
    {
        char full[1024];
        char uri[512];
        const char *slash = strrchr(c->path, '/');
        const char *bslash = strrchr(c->path, '\\');
        size_t dir;
        uint8_t *b;
        if (bslash != NULL && (slash == NULL || bslash > slash)) {
            slash = bslash;
        }
        dir = slash != NULL ? (size_t)(slash - c->path) + 1u : 0u;
        if (strlen(img->uri) >= sizeof uri || dir + strlen(img->uri) >= sizeof full || strstr(img->uri, "..") != NULL ||
            strchr(img->uri, ':') != NULL || img->uri[0] == '/' || img->uri[0] == '\\') {
            return NULL; /* only plain relative paths next to the source; no escapes */
        }
        snprintf(uri, sizeof uri, "%s", img->uri);
        cgltf_decode_uri(uri);
        memcpy(full, c->path, dir);
        snprintf(full + dir, sizeof full - dir, "%s", uri);
        b = read_file(full, len);
        *owned = b != NULL;
        return b;
    }
}

/* ---------- textures ---------- */

static float srgb_to_linear(float s) {
    return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
}

/* Linear value at which the sRGB code k + 1 starts (rounding of the encoded value): code(l) is the number of these
 * thresholds that l reaches. Equals rounding srgb(l) * 255 (ties aside) without a pow per texel. */
static float g_srgb_up[255];

static void srgb_thresholds(void) {
    int k;
    if (g_srgb_up[254] != 0.0f) {
        return;
    }
    for (k = 0; k < 255; k++) {
        float s = ((float)k + 0.5f) / 255.0f;
        g_srgb_up[k] = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
    }
}

static uint8_t linear_to_srgb8(float l) {
    int lo = 0;
    int hi = 255; /* answer in [0, 255]: the count of thresholds <= l */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_srgb_up[mid] <= l) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (uint8_t)lo;
}

/* Halves an RGBA8 image (2x2 box; odd edges reuse the last texel). role 0 sRGB, 1 linear, 2 normal map. */
static uint8_t *halve(const uint8_t *src, uint32_t w, uint32_t h, int role, const float *lut, uint32_t *ow, uint32_t *oh) {
    uint32_t nw = w > 1u ? w / 2u : 1u;
    uint32_t nh = h > 1u ? h / 2u : 1u;
    uint8_t *dst = malloc((size_t)nw * nh * 4u);
    uint32_t x;
    uint32_t y;
    if (dst == NULL) {
        return NULL;
    }
    for (y = 0; y < nh; y++) {
        for (x = 0; x < nw; x++) {
            uint32_t xs[2] = {x * 2u < w ? x * 2u : w - 1u, x * 2u + 1u < w ? x * 2u + 1u : w - 1u};
            uint32_t ys[2] = {y * 2u < h ? y * 2u : h - 1u, y * 2u + 1u < h ? y * 2u + 1u : h - 1u};
            float acc[4] = {0, 0, 0, 0};
            int i;
            int k;
            uint8_t *o = dst + ((size_t)y * nw + x) * 4u;
            for (i = 0; i < 4; i++) {
                const uint8_t *p = src + ((size_t)ys[i >> 1] * w + xs[i & 1]) * 4u;
                for (k = 0; k < 4; k++) {
                    acc[k] += role == 0 && k < 3 ? lut[p[k]] : (float)p[k] / 255.0f;
                }
            }
            for (k = 0; k < 4; k++) {
                acc[k] *= 0.25f;
            }
            if (role == 2) {
                float nx = acc[0] * 2.0f - 1.0f;
                float ny = acc[1] * 2.0f - 1.0f;
                float nz = acc[2] * 2.0f - 1.0f;
                float l = sqrtf(nx * nx + ny * ny + nz * nz);
                if (l > 1e-6f) {
                    acc[0] = nx / l * 0.5f + 0.5f;
                    acc[1] = ny / l * 0.5f + 0.5f;
                    acc[2] = nz / l * 0.5f + 0.5f;
                }
            }
            for (k = 0; k < 4; k++) {
                o[k] = role == 0 && k < 3 ? linear_to_srgb8(acc[k]) : (uint8_t)(acc[k] * 255.0f + 0.5f);
            }
        }
    }
    *ow = nw;
    *oh = nh;
    return dst;
}

/* Writes an RGBA8 texture with a generated mip chain; takes ownership of base. */
static uint32_t put_rgba8(Cook *c, uint64_t id, uint8_t *base, uint32_t w, uint32_t h, int role) {
    uint8_t *levels[SU_ASSET_MAX_MIPS];
    uint32_t n = 0;
    uint32_t i;
    uint32_t st;
    uint32_t fmt = role == 0 ? SU_TEX_RGBA8_SRGB : SU_TEX_RGBA8_UNORM;
    float lut[256];
    srgb_thresholds();
    for (i = 0; i < 256u; i++) {
        lut[i] = srgb_to_linear((float)i / 255.0f);
    }
    while (w > c->max_tex || h > c->max_tex) {
        uint32_t nw;
        uint32_t nh;
        uint8_t *s = halve(base, w, h, role, lut, &nw, &nh);
        free(base);
        if (s == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        base = s;
        w = nw;
        h = nh;
    }
    levels[n++] = base;
    if ((c->opt == NULL || (c->opt->flags & SU_ASSET_COOK_NO_MIPS) == 0u)) {
        uint32_t lw = w;
        uint32_t lh = h;
        while ((lw > 1u || lh > 1u) && n < SU_ASSET_MAX_MIPS) {
            uint32_t nw;
            uint32_t nh;
            uint8_t *s = halve(levels[n - 1u], lw, lh, role, lut, &nw, &nh);
            if (s == NULL) {
                for (i = 0; i < n; i++) {
                    free(levels[i]);
                }
                return SU_E_RESOURCE_LIMIT;
            }
            levels[n++] = s;
            lw = nw;
            lh = nh;
        }
    }
    if (c->opt != NULL && (c->opt->flags & SU_ASSET_COOK_BC) != 0u) {
        /* block-compress every level in place of the RGBA8 one */
        int alpha = 0;
        uint64_t k;
        for (k = 0; k < (uint64_t)w * h && role == 0; k++) {
            alpha |= levels[0][k * 4u + 3u] != 255u;
        }
        fmt = role == 2 ? SU_TEX_BC5_UNORM : role == 1 ? SU_TEX_BC1_UNORM : alpha ? SU_TEX_BC3_SRGB : SU_TEX_BC1_SRGB;
        for (i = 0; i < n; i++) {
            uint32_t lw = w >> i ? w >> i : 1u;
            uint32_t lh = h >> i ? h >> i : 1u;
            uint8_t *bc = malloc((size_t)su_asset_texture_level_size(fmt, lw, lh));
            if (bc == NULL || su_asset_bc_encode(fmt, levels[i], lw, lh, bc) != SU_OK) {
                uint32_t q;
                free(bc);
                for (q = 0; q < n; q++) {
                    free(levels[q]);
                }
                return SU_E_RESOURCE_LIMIT;
            }
            free(levels[i]);
            levels[i] = bc;
        }
    }
    st = su_asset_writer_add_texture(c->w, id, w, h, fmt, role == 2 ? SU_ASSET_TEX_FLAG_NORMAL_MAP : 0u, n,
                                     (const uint8_t *const *)levels);
    for (i = 0; i < n; i++) {
        c->rep->texture_bytes += su_asset_texture_level_size(fmt, w >> i ? w >> i : 1u, h >> i ? h >> i : 1u);
        free(levels[i]);
    }
    return st;
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static const uint8_t KTX2_ID[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

static uint32_t ktx2_format(uint32_t vk) {
    switch (vk) {
    case 37: return SU_TEX_RGBA8_UNORM;   /* VK_FORMAT_R8G8B8A8_UNORM */
    case 43: return SU_TEX_RGBA8_SRGB;    /* VK_FORMAT_R8G8B8A8_SRGB */
    case 131: case 133: return SU_TEX_BC1_UNORM; /* BC1_RGB / BC1_RGBA UNORM */
    case 132: case 134: return SU_TEX_BC1_SRGB;
    case 137: return SU_TEX_BC3_UNORM;
    case 138: return SU_TEX_BC3_SRGB;
    case 139: return SU_TEX_BC4_UNORM;
    case 141: return SU_TEX_BC5_UNORM;
    case 145: return SU_TEX_BC7_UNORM;
    case 146: return SU_TEX_BC7_SRGB;
    default: return 0;
    }
}

/* Reads a KTX2 file (2D, one layer, one face, no supercompression, RGBA8 or BCn). */
static uint32_t put_ktx2(Cook *c, uint64_t id, const uint8_t *b, size_t len, int role, const char *what) {
    uint32_t vk;
    uint32_t w;
    uint32_t h;
    uint32_t levels;
    uint32_t fmt;
    const uint8_t *mips[SU_ASSET_MAX_MIPS];
    uint32_t i;
    if (len < 80u || memcmp(b, KTX2_ID, 12) != 0) {
        msgf(c->msg, "%s: not a KTX2 file", what);
        return SU_E_PARSE;
    }
    vk = rd32le(b + 12);
    w = rd32le(b + 20);
    h = rd32le(b + 24);
    levels = rd32le(b + 40);
    if (rd32le(b + 28) > 1u || rd32le(b + 32) > 1u || rd32le(b + 36) != 1u) {
        msgf(c->msg, "%s: KTX2 depth, array layers or cube faces are not supported", what);
        return SU_E_PARSE;
    }
    if (rd32le(b + 44) != 0u) {
        msgf(c->msg, "%s: supercompressed KTX2 (scheme %u, e.g. Basis Universal or zstd) is not supported; use "
                     "uncompressed RGBA8 or BCn",
             what, (unsigned)rd32le(b + 44));
        return SU_E_PARSE;
    }
    fmt = ktx2_format(vk);
    if (fmt == 0u) {
        msgf(c->msg, "%s: KTX2 vkFormat %u is not supported (RGBA8, BC1/3/4/5/7)", what, (unsigned)vk);
        return SU_E_PARSE;
    }
    if (levels == 0u) {
        levels = 1u;
    }
    if (w == 0u || h == 0u || w > 16384u || h > 16384u || levels > SU_ASSET_MAX_MIPS || 80u + 24u * (size_t)levels > len) {
        msgf(c->msg, "%s: KTX2 header out of range", what);
        return SU_E_PARSE;
    }
    for (i = 0; i < levels; i++) {
        uint64_t off = rd64le(b + 80 + 24 * i);
        uint64_t sz = rd64le(b + 80 + 24 * i + 8);
        uint32_t lw = w >> i ? w >> i : 1u;
        uint32_t lh = h >> i ? h >> i : 1u;
        if (sz != su_asset_texture_level_size(fmt, lw, lh) || off > len || sz > len - off) {
            msgf(c->msg, "%s: KTX2 level %u has a wrong size or range", what, (unsigned)i);
            return SU_E_PARSE;
        }
        mips[i] = b + off;
    }
    if ((role == 0) != (fmt == SU_TEX_RGBA8_SRGB || fmt == SU_TEX_BC1_SRGB || fmt == SU_TEX_BC3_SRGB ||
                        fmt == SU_TEX_BC7_SRGB)) {
        warn(c, "%s: KTX2 colour space does not match its material slot; kept as stored", what);
    }
    if ((fmt == SU_TEX_RGBA8_UNORM || fmt == SU_TEX_RGBA8_SRGB) && levels == 1u) {
        uint8_t *copy = malloc((size_t)w * h * 4u);
        if (copy == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        memcpy(copy, mips[0], (size_t)w * h * 4u);
        return put_rgba8(c, id, copy, w, h, role);
    }
    for (i = 0; i < levels; i++) {
        c->rep->texture_bytes += su_asset_texture_level_size(fmt, w >> i ? w >> i : 1u, h >> i ? h >> i : 1u);
    }
    return su_asset_writer_add_texture(c->w, id, w, h, fmt, role == 2 ? SU_ASSET_TEX_FLAG_NORMAL_MAP : 0u, levels, mips);
}

/* Cooks texture (image, role) once; returns its id, or 0 when the image cannot be used (a warning). */
static uint64_t cook_texture(Cook *c, const cgltf_texture *t, int role, uint32_t *st) {
    const cgltf_image *img;
    size_t ii;
    uint64_t id;
    const uint8_t *bytes;
    size_t len = 0;
    int owned = 0;
    char what[300];
    if (t == NULL) {
        return 0;
    }
    img = t->image != NULL ? t->image : t->has_basisu ? t->basisu_image : NULL;
    if (img == NULL) {
        warn(c, "texture without a usable image skipped");
        return 0;
    }
    ii = cgltf_image_index(c->data, img);
    id = tex_id(c, ii, role);
    if (c->tex_state[ii * 3u + (size_t)role] == 1u) {
        return id;
    }
    if (c->tex_state[ii * 3u + (size_t)role] == 2u) {
        return 0;
    }
    snprintf(what, sizeof what, "image %u (%s)", (unsigned)ii,
             img->uri != NULL && strncmp(img->uri, "data:", 5) != 0 ? img->uri : img->name != NULL ? img->name : "embedded");
    bytes = image_bytes(c, img, &len, &owned);
    if (bytes == NULL) {
        warn(c, "%s: bytes could not be read; slot left empty", what);
        c->tex_state[ii * 3u + (size_t)role] = 2u;
        return 0;
    }
    if (len >= 12u && memcmp(bytes, KTX2_ID, 12) == 0) {
        char why[256];
        char *keep = c->msg;
        why[0] = '\0';
        c->msg = why;
        *st = put_ktx2(c, id, bytes, len, role, what);
        c->msg = keep;
        if (*st == SU_E_PARSE) {
            warn(c, "%s; slot left empty", why);
            *st = SU_OK;
            if (owned) {
                free((void *)bytes);
            }
            c->tex_state[ii * 3u + (size_t)role] = 2u;
            return 0;
        }
    } else {
        int w = 0;
        int h = 0;
        int comp = 0;
        uint8_t *px = len <= 0x7FFFFFFFu ? stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4) : NULL;
        if (px == NULL || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            warn(c, "%s: not a decodable PNG/JPEG (%s); slot left empty", what, px == NULL ? stbi_failure_reason() : "size");
            stbi_image_free(px);
            if (owned) {
                free((void *)bytes);
            }
            c->tex_state[ii * 3u + (size_t)role] = 2u;
            return 0;
        }
        {
            uint8_t *copy = malloc((size_t)w * (size_t)h * 4u);
            if (copy == NULL) {
                *st = SU_E_RESOURCE_LIMIT;
            } else {
                memcpy(copy, px, (size_t)w * (size_t)h * 4u);
                *st = put_rgba8(c, id, copy, (uint32_t)w, (uint32_t)h, role);
            }
        }
        stbi_image_free(px);
    }
    if (owned) {
        free((void *)bytes);
    }
    if (*st != SU_OK) {
        return 0;
    }
    c->tex_state[ii * 3u + (size_t)role] = 1u;
    c->rep->textures++;
    return id;
}

static uint32_t wrap_of(int gl) {
    return gl == cgltf_wrap_mode_clamp_to_edge ? SU_ASSET_WRAP_CLAMP
           : gl == cgltf_wrap_mode_mirrored_repeat ? SU_ASSET_WRAP_MIRROR
                                                   : SU_ASSET_WRAP_REPEAT;
}

static void slot(Cook *c, SuAssetMaterial *m, uint32_t s, const cgltf_texture_view *v, int role, uint32_t *st) {
    if (v->texture == NULL || *st != SU_OK) {
        return;
    }
    if (v->has_transform) {
        warn(c, "KHR_texture_transform is ignored");
    }
    if (v->texcoord > 1) {
        warn(c, "texture slot uses TEXCOORD_%d; only 0 and 1 are kept", v->texcoord);
        return;
    }
    m->texture[s] = cook_texture(c, v->texture, role, st);
    if (m->texture[s] != 0u) {
        const cgltf_sampler *sm = v->texture->sampler;
        m->tex_info[s] = SU_ASSET_TEX_INFO((uint32_t)v->texcoord, sm != NULL ? wrap_of(sm->wrap_s) : 0u,
                                           sm != NULL ? wrap_of(sm->wrap_t) : 0u);
    }
}

/* ---------- SUTEKH_materials_layered_paint (see docs/RENDERING_AND_ASSETS.md) ---------- */

/* A tiny reader for the extension's JSON (already validated by cgltf as a whole document). */
static const char *j_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    return p;
}

/* Past the value at p (string, object, array or scalar). */
static const char *j_skip(const char *p) {
    int depth = 0;
    p = j_ws(p);
    do {
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                p += *p == '\\' && p[1] != '\0' ? 2 : 1;
            }
            if (*p == '"') {
                p++;
            }
        } else if (*p == '{' || *p == '[') {
            depth++;
            p++;
        } else if (*p == '}' || *p == ']') {
            depth--;
            p++;
        } else if (*p == '\0') {
            return p;
        } else if (*p == ',' && depth == 0) {
            return p;
        } else if (*p == ',') {
            p++;
        } else {
            while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' &&
                   *p != '"' && *p != '{' && *p != '[') {
                p++;
            }
        }
        p = j_ws(p);
    } while (depth > 0);
    return p;
}

/* Value of a key in the object at obj (NULL when absent). */
static const char *j_get(const char *obj, const char *key) {
    size_t kl = strlen(key);
    const char *p = j_ws(obj);
    if (*p != '{') {
        return NULL;
    }
    p = j_ws(p + 1);
    while (*p == '"') {
        const char *ks = p + 1;
        const char *ke = ks;
        while (*ke != '\0' && *ke != '"') {
            ke += *ke == '\\' && ke[1] != '\0' ? 2 : 1;
        }
        p = j_ws(ke + (*ke == '"' ? 1 : 0));
        if (*p != ':') {
            return NULL;
        }
        p = j_ws(p + 1);
        if ((size_t)(ke - ks) == kl && strncmp(ks, key, kl) == 0) {
            return p;
        }
        p = j_ws(j_skip(p));
        if (*p == ',') {
            p = j_ws(p + 1);
        }
    }
    return NULL;
}

static int j_num(const char *v, float *out) {
    char *end = NULL;
    double d;
    if (v == NULL) {
        return 0;
    }
    d = strtod(v, &end);
    if (end == v || !isfinite(d)) {
        return 0;
    }
    *out = (float)d;
    return 1;
}

static int j_floats(const char *v, float *out, int n) {
    int i;
    if (v == NULL || *j_ws(v) != '[') {
        return 0;
    }
    v = j_ws(v) + 1;
    for (i = 0; i < n; i++) {
        v = j_ws(v);
        if (!j_num(v, &out[i])) {
            return 0;
        }
        v = j_ws(j_skip(v));
        if (*v == ',') {
            v++;
        }
    }
    return 1;
}

static int j_is_str(const char *v, const char *s) {
    size_t n = strlen(s);
    return v != NULL && v[0] == '"' && strncmp(v + 1, s, n) == 0 && v[1 + n] == '"';
}

/* A textureInfo object {"index": n, "texCoord": k}: fills the paint slot (role: 0 sRGB, 1 linear, 2 normal). */
static void paint_slot_view(Cook *c, SuAssetPaint *p, uint32_t s, const cgltf_texture_view *v, int role, uint32_t *st) {
    if (v->texture == NULL || *st != SU_OK) {
        return;
    }
    if (v->texcoord > 1) {
        warn(c, "paint texture slot %u uses TEXCOORD_%d; only 0 and 1 are kept", (unsigned)s, v->texcoord);
        return;
    }
    p->texture[s] = cook_texture(c, v->texture, role, st);
    if (p->texture[s] != 0u) {
        const cgltf_sampler *sm = v->texture->sampler;
        p->tex_info[s] = SU_ASSET_TEX_INFO((uint32_t)v->texcoord, sm != NULL ? wrap_of(sm->wrap_s) : 0u,
                                           sm != NULL ? wrap_of(sm->wrap_t) : 0u);
    }
}

static void paint_slot_json(Cook *c, SuAssetPaint *p, uint32_t s, const char *ext, const char *key, int role, uint32_t *st) {
    const char *o = j_get(ext, key);
    float idx = -1.0f;
    float tc = 0.0f;
    cgltf_texture_view v;
    if (o == NULL) {
        return;
    }
    memset(&v, 0, sizeof v);
    if (!j_num(j_get(o, "index"), &idx) || idx < 0.0f || (size_t)idx >= c->data->textures_count) {
        warn(c, "%s: texture index invalid; slot left empty", key);
        return;
    }
    (void)j_num(j_get(o, "texCoord"), &tc);
    v.texture = &c->data->textures[(size_t)idx];
    v.texcoord = (int)tc;
    paint_slot_view(c, p, s, &v, role, st);
}

static void paint_channel(Cook *c, SuAssetPaint *p, const char *chs, const char *key, uint32_t which) {
    float v = 0.0f;
    if (chs != NULL && j_num(j_get(chs, key), &v)) {
        if (v < 0.0f) {
            p->channel[which] = SU_PAINT_CH_NONE;
        } else if (v <= 3.0f) {
            p->channel[which] = (uint8_t)v;
        } else {
            warn(c, "channels.%s out of range; default kept", key);
        }
    }
}

/* Fills m (factors) and p from the glTF material and its SUTEKH_materials_layered_paint object. The core textures map:
 * baseColorTexture -> albedo (sRGB), normalTexture -> normal, emissiveTexture -> glow (sRGB), occlusionTexture -> ao. */
static void cook_paint(Cook *c, const cgltf_material *gm, const char *ext, SuAssetPaint *p, uint32_t *st) {
    const char *v;
    const char *chs = j_get(ext, "channels");
    float f = 0.0f;
    uint32_t n = 0;
    su_asset_paint_default(p);
    if (gm->has_pbr_metallic_roughness) {
        paint_slot_view(c, p, SU_PAINT_SLOT_ALBEDO, &gm->pbr_metallic_roughness.base_color_texture, 0, st);
    }
    paint_slot_view(c, p, SU_PAINT_SLOT_NORMAL, &gm->normal_texture, 2, st);
    paint_slot_view(c, p, SU_PAINT_SLOT_GLOW, &gm->emissive_texture, 0, st);
    paint_slot_view(c, p, SU_PAINT_SLOT_AO, &gm->occlusion_texture, 1, st);
    paint_slot_json(c, p, SU_PAINT_SLOT_ROUGHNESS, ext, "roughnessTexture", 1, st);
    paint_slot_json(c, p, SU_PAINT_SLOT_MATERIAL, ext, "materialTexture", 1, st);
    paint_slot_json(c, p, SU_PAINT_SLOT_MASK, ext, "maskTexture", 1, st);
    paint_slot_json(c, p, SU_PAINT_SLOT_DIRT, ext, "dirtTexture", 1, st);
    paint_channel(c, p, chs, "roughness", SU_PAINT_CH_ROUGHNESS);
    paint_channel(c, p, chs, "metal", SU_PAINT_CH_METAL);
    paint_channel(c, p, chs, "specular", SU_PAINT_CH_SPECULAR);
    paint_channel(c, p, chs, "dirt", SU_PAINT_CH_DIRT);
    paint_channel(c, p, chs, "ao", SU_PAINT_CH_AO);
    if (j_num(j_get(ext, "specular"), &f) && f >= 0.0f) {
        p->specular = f;
    }
    v = j_get(ext, "dirt");
    if (v != NULL) {
        if (j_num(j_get(v, "amount"), &f)) {
            p->dirt_amount = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        }
        if (j_num(j_get(v, "darken"), &f)) {
            p->dirt_darken = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        }
        if (j_num(j_get(v, "roughen"), &f)) {
            p->dirt_roughen = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        }
    }
    v = j_get(ext, "layers");
    if (v != NULL && *j_ws(v) == '[') {
        const char *e = j_ws(v) + 1;
        while (*j_ws(e) == '{') {
            e = j_ws(e);
            if (n == SU_PAINT_MAX_LAYERS) {
                warn(c, "more than %u paint layers; the rest are ignored", SU_PAINT_MAX_LAYERS);
                break;
            }
            {
                SuAssetPaintLayer *l = &p->layer[n];
                const char *b = j_get(e, "blend");
                const char *gc = j_get(e, "glowColor");
                (void)j_floats(j_get(e, "color"), l->color, 3);
                (void)j_num(j_get(e, "roughness"), &l->roughness);
                (void)j_num(j_get(e, "metallic"), &l->metallic);
                (void)j_num(j_get(e, "opacity"), &l->opacity);
                l->flags = j_is_str(b, "multiply") ? SU_PAINT_LAYER_MULTIPLY : 0u;
                if (b != NULL && !j_is_str(b, "multiply") && !j_is_str(b, "lerp")) {
                    warn(c, "paint layer %u: unknown blend; lerp used", (unsigned)n);
                }
                if (gc != NULL && j_floats(gc, l->glow_color, 3)) {
                    l->flags |= SU_PAINT_LAYER_GLOW_TINT;
                    (void)j_num(j_get(e, "glowStrength"), &l->glow_strength);
                }
                l->roughness = l->roughness < 0.0f ? 0.0f : l->roughness > 1.0f ? 1.0f : l->roughness;
                l->metallic = l->metallic < 0.0f ? 0.0f : l->metallic > 1.0f ? 1.0f : l->metallic;
                l->opacity = l->opacity < 0.0f ? 0.0f : l->opacity > 1.0f ? 1.0f : l->opacity;
                l->glow_strength = l->glow_strength < 0.0f ? 0.0f : l->glow_strength;
            }
            n++;
            e = j_ws(j_skip(e));
            if (*e == ',') {
                e++;
            }
        }
    }
    p->layer_count = n;
    v = j_get(ext, "order");
    if (v != NULL && n > 0u) {
        float o[4] = {0, 0, 0, 0};
        uint32_t seen = 0;
        uint32_t i;
        int ok = j_floats(v, o, (int)n);
        for (i = 0; ok && i < n; i++) {
            ok = o[i] >= 0.0f && o[i] < (float)n && (seen & (1u << (uint32_t)o[i])) == 0u;
            seen |= ok ? 1u << (uint32_t)o[i] : 0u;
        }
        if (ok) {
            for (i = 0; i < n; i++) {
                p->layer_order[i] = (uint8_t)o[i];
            }
        } else {
            warn(c, "paint layer order is not a permutation of the layers; identity used");
        }
    }
}

static uint64_t cook_material(Cook *c, const cgltf_material *gm, uint32_t *st) {
    size_t i;
    SuAssetMaterial m;
    SuAssetPaint paint;
    const char *pext = NULL;
    size_t k;
    uint64_t id;
    if (gm == NULL) {
        return 0;
    }
    i = cgltf_material_index(c->data, gm);
    id = mat_id(c, i);
    if (c->mat_done[i]) {
        return id;
    }
    su_asset_material_default(&m);
    for (k = 0; k < gm->extensions_count; k++) {
        if (gm->extensions[k].name != NULL && strcmp(gm->extensions[k].name, "SUTEKH_materials_layered_paint") == 0 &&
            gm->extensions[k].data != NULL) {
            pext = gm->extensions[k].data;
        }
    }
    if (gm->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness *p = &gm->pbr_metallic_roughness;
        memcpy(m.base_color, p->base_color_factor, sizeof m.base_color);
        m.metallic = p->metallic_factor;
        m.roughness = p->roughness_factor;
        if (pext == NULL) {
            slot(c, &m, SU_ASSET_SLOT_BASE_COLOR, &p->base_color_texture, 0, st);
            slot(c, &m, SU_ASSET_SLOT_METAL_ROUGH, &p->metallic_roughness_texture, 1, st);
        }
    } else if (gm->has_pbr_specular_glossiness) {
        warn(c, "material %u: KHR_materials_pbrSpecularGlossiness approximated by its diffuse factor", (unsigned)i);
        memcpy(m.base_color, gm->pbr_specular_glossiness.diffuse_factor, sizeof m.base_color);
        m.metallic = 0.0f;
        m.roughness = 1.0f - gm->pbr_specular_glossiness.glossiness_factor;
    }
    memcpy(m.emissive, gm->emissive_factor, sizeof m.emissive);
    if (gm->has_emissive_strength) {
        m.emissive_strength = gm->emissive_strength.emissive_strength;
    }
    m.normal_scale = gm->normal_texture.texture != NULL ? gm->normal_texture.scale : 1.0f;
    m.occlusion_strength = gm->occlusion_texture.texture != NULL ? gm->occlusion_texture.scale : 1.0f;
    if (pext == NULL) {
        slot(c, &m, SU_ASSET_SLOT_NORMAL, &gm->normal_texture, 2, st);
        slot(c, &m, SU_ASSET_SLOT_EMISSIVE, &gm->emissive_texture, 0, st);
        slot(c, &m, SU_ASSET_SLOT_OCCLUSION, &gm->occlusion_texture, 1, st);
    } else {
        cook_paint(c, gm, pext, &paint, st);
    }
    m.alpha_mode = gm->alpha_mode == cgltf_alpha_mode_mask    ? SU_ASSET_ALPHA_MASK
                   : gm->alpha_mode == cgltf_alpha_mode_blend ? SU_ASSET_ALPHA_BLEND
                                                              : SU_ASSET_ALPHA_OPAQUE;
    m.alpha_cutoff = gm->alpha_cutoff;
    m.double_sided = gm->double_sided ? 1u : 0u;
    m.unlit = gm->unlit ? 1u : 0u;
    if (*st != SU_OK) {
        return 0;
    }
    *st = pext != NULL ? su_asset_writer_add_paint_material(c->w, id, &m, &paint) : su_asset_writer_add_material(c->w, id, &m);
    if (*st != SU_OK) {
        msgf(c->msg, "material %u could not be written", (unsigned)i);
        return 0;
    }
    c->mat_done[i] = 1u;
    c->rep->materials++;
    return id;
}

/* ---------- meshes ---------- */

typedef struct Build {
    SuAssetVertex *v;
    uint32_t nv;
    uint32_t cv;
    uint32_t *idx;
    uint32_t ni;
    uint32_t ci;
    SuAssetSubmesh *sm;
    uint32_t ns;
    uint32_t cs;
} Build;

static int grow(void **p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need > *cap) {
        uint64_t nc = *cap ? *cap : 64u;
        void *np;
        while (nc < need) {
            nc *= 2u;
        }
        if (nc > 0xFFFFFFFFu || nc * elem > (uint64_t)SIZE_MAX / 2u) {
            return 0;
        }
        np = realloc(*p, (size_t)nc * elem);
        if (np == NULL) {
            return 0;
        }
        *p = np;
        *cap = (uint32_t)nc;
    }
    return 1;
}

static const cgltf_accessor *attr(const cgltf_primitive *p, cgltf_attribute_type type, int index) {
    size_t i;
    for (i = 0; i < p->attributes_count; i++) {
        if (p->attributes[i].type == type && p->attributes[i].index == index) {
            return p->attributes[i].data;
        }
    }
    return NULL;
}

static void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void norm3(float *v) {
    float l = sqrtf(dot3(v, v));
    if (l > 1e-20f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static void any_perp(const float *n, float *t) {
    float a[3] = {fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
    float b[3];
    cross3(n, a, b);
    norm3(b);
    t[0] = b[0];
    t[1] = b[1];
    t[2] = b[2];
}

/* Tangents from UV0 for vertices [v0, nv) over indices [i0, ni). */
static void gen_tangents(Build *b, uint32_t v0, uint32_t i0) {
    uint32_t nv = b->nv - v0;
    float *tan = calloc((size_t)nv * 6u, sizeof(float));
    uint32_t i;
    if (tan == NULL) {
        for (i = v0; i < b->nv; i++) {
            any_perp(b->v[i].normal, b->v[i].tangent);
            b->v[i].tangent[3] = 1.0f;
        }
        return;
    }
    for (i = i0; i + 2u < b->ni; i += 3u) {
        const SuAssetVertex *a = &b->v[b->idx[i]];
        const SuAssetVertex *bb = &b->v[b->idx[i + 1u]];
        const SuAssetVertex *cc = &b->v[b->idx[i + 2u]];
        float e1[3] = {bb->pos[0] - a->pos[0], bb->pos[1] - a->pos[1], bb->pos[2] - a->pos[2]};
        float e2[3] = {cc->pos[0] - a->pos[0], cc->pos[1] - a->pos[1], cc->pos[2] - a->pos[2]};
        float du1 = bb->uv0[0] - a->uv0[0];
        float dv1 = bb->uv0[1] - a->uv0[1];
        float du2 = cc->uv0[0] - a->uv0[0];
        float dv2 = cc->uv0[1] - a->uv0[1];
        float det = du1 * dv2 - du2 * dv1;
        float r;
        float t[3];
        float s[3];
        int k;
        int j;
        if (fabsf(det) < 1e-20f) {
            continue;
        }
        r = 1.0f / det;
        for (k = 0; k < 3; k++) {
            t[k] = (e1[k] * dv2 - e2[k] * dv1) * r;
            s[k] = (e2[k] * du1 - e1[k] * du2) * r;
        }
        for (j = 0; j < 3; j++) {
            float *dst = tan + (size_t)(b->idx[i + (uint32_t)j] - v0) * 6u;
            for (k = 0; k < 3; k++) {
                dst[k] += t[k];
                dst[3 + k] += s[k];
            }
        }
    }
    for (i = 0; i < nv; i++) {
        SuAssetVertex *v = &b->v[v0 + i];
        float *t = tan + (size_t)i * 6u;
        float d = dot3(v->normal, t);
        float o[3] = {t[0] - v->normal[0] * d, t[1] - v->normal[1] * d, t[2] - v->normal[2] * d};
        float cr[3];
        if (dot3(o, o) < 1e-20f) {
            any_perp(v->normal, o);
        }
        norm3(o);
        cross3(v->normal, o, cr);
        v->tangent[0] = o[0];
        v->tangent[1] = o[1];
        v->tangent[2] = o[2];
        v->tangent[3] = dot3(cr, t + 3) < 0.0f ? -1.0f : 1.0f;
    }
    free(tan);
}

/* Appends one primitive as a submesh. Returns SU_OK, or SU_E_PARSE / SU_E_RESOURCE_LIMIT. Skipped primitives
 * (non-triangle) return SU_OK and add nothing. */
static uint32_t add_primitive(Cook *c, Build *b, const cgltf_primitive *p, size_t mi, size_t pi, uint32_t *st_mat) {
    const cgltf_accessor *pos = attr(p, cgltf_attribute_type_position, 0);
    const cgltf_accessor *nrm = attr(p, cgltf_attribute_type_normal, 0);
    const cgltf_accessor *tng = attr(p, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor *uv0 = attr(p, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor *uv1 = attr(p, cgltf_attribute_type_texcoord, 1);
    uint32_t nv;
    uint32_t nsrc;
    uint32_t ntri;
    uint32_t *tri = NULL;
    uint32_t v0 = b->nv;
    uint32_t i0 = b->ni;
    uint32_t i;
    int flat = nrm == NULL;
    SuAssetSubmesh s;
    if (p->type != cgltf_primitive_type_triangles && p->type != cgltf_primitive_type_triangle_strip &&
        p->type != cgltf_primitive_type_triangle_fan) {
        warn(c, "mesh %u primitive %u: points/lines skipped", (unsigned)mi, (unsigned)pi);
        return SU_OK;
    }
    if (p->has_draco_mesh_compression) {
        warn(c, "mesh %u primitive %u: Draco compression is not supported; skipped", (unsigned)mi, (unsigned)pi);
        return SU_OK;
    }
    if (p->targets_count > 0u) {
        warn(c, "mesh %u primitive %u: morph targets ignored", (unsigned)mi, (unsigned)pi);
    }
    if (pos == NULL || pos->type != cgltf_type_vec3 || pos->count == 0u || pos->count > 0x3FFFFFFFu) {
        msgf(c->msg, "mesh %u primitive %u: POSITION missing or not VEC3", (unsigned)mi, (unsigned)pi);
        return SU_E_PARSE;
    }
    nv = (uint32_t)pos->count;
    if ((nrm != NULL && (nrm->count != nv || nrm->type != cgltf_type_vec3)) ||
        (tng != NULL && (tng->count != nv || tng->type != cgltf_type_vec4)) ||
        (uv0 != NULL && (uv0->count != nv || uv0->type != cgltf_type_vec2)) ||
        (uv1 != NULL && (uv1->count != nv || uv1->type != cgltf_type_vec2))) {
        msgf(c->msg, "mesh %u primitive %u: attribute counts or types disagree", (unsigned)mi, (unsigned)pi);
        return SU_E_PARSE;
    }
    nsrc = p->indices != NULL ? (uint32_t)(p->indices->count <= 0x3FFFFFFFu ? p->indices->count : 0u) : nv;
    if (p->indices != NULL && p->indices->count > 0x3FFFFFFFu) {
        msgf(c->msg, "mesh %u primitive %u: too many indices", (unsigned)mi, (unsigned)pi);
        return SU_E_PARSE;
    }
    ntri = p->type == cgltf_primitive_type_triangles ? nsrc / 3u : nsrc >= 3u ? nsrc - 2u : 0u;
    if (ntri == 0u) {
        warn(c, "mesh %u primitive %u: no triangles", (unsigned)mi, (unsigned)pi);
        return SU_OK;
    }
    tri = malloc((size_t)ntri * 3u * sizeof *tri);
    if (tri == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < ntri; i++) {
        uint32_t a;
        uint32_t bb;
        uint32_t cc;
        if (p->type == cgltf_primitive_type_triangles) {
            a = i * 3u;
            bb = a + 1u;
            cc = a + 2u;
        } else if (p->type == cgltf_primitive_type_triangle_strip) {
            a = i;
            bb = i + 1u + (i & 1u);
            cc = i + 2u - (i & 1u);
        } else {
            a = i + 1u;
            bb = i + 2u;
            cc = 0u;
        }
        tri[i * 3u] = p->indices != NULL ? (uint32_t)cgltf_accessor_read_index(p->indices, a) : a;
        tri[i * 3u + 1u] = p->indices != NULL ? (uint32_t)cgltf_accessor_read_index(p->indices, bb) : bb;
        tri[i * 3u + 2u] = p->indices != NULL ? (uint32_t)cgltf_accessor_read_index(p->indices, cc) : cc;
        if (tri[i * 3u] >= nv || tri[i * 3u + 1u] >= nv || tri[i * 3u + 2u] >= nv) {
            free(tri);
            msgf(c->msg, "mesh %u primitive %u: index out of range", (unsigned)mi, (unsigned)pi);
            return SU_E_PARSE;
        }
    }
    {
        uint32_t out_v = flat ? ntri * 3u : nv;
        if ((uint64_t)b->nv + out_v > 0xFFFFFFFFu || (uint64_t)b->ni + ntri * 3u > 0xFFFFFFFFu ||
            !grow((void **)&b->v, &b->cv, b->nv + out_v, sizeof *b->v) ||
            !grow((void **)&b->idx, &b->ci, b->ni + ntri * 3u, sizeof *b->idx) ||
            !grow((void **)&b->sm, &b->cs, b->ns + 1u, sizeof *b->sm)) {
            free(tri);
            return SU_E_RESOURCE_LIMIT;
        }
    }
    /* source vertices */
    {
        SuAssetVertex *src = flat ? malloc((size_t)nv * sizeof *src) : b->v + b->nv;
        if (src == NULL) {
            free(tri);
            return SU_E_RESOURCE_LIMIT;
        }
        memset(src, 0, (size_t)nv * sizeof *src);
        for (i = 0; i < nv; i++) {
            (void)cgltf_accessor_read_float(pos, i, src[i].pos, 3);
            if (nrm != NULL) {
                (void)cgltf_accessor_read_float(nrm, i, src[i].normal, 3);
                norm3(src[i].normal);
            }
            if (tng != NULL) {
                (void)cgltf_accessor_read_float(tng, i, src[i].tangent, 4);
            }
            if (uv0 != NULL) {
                (void)cgltf_accessor_read_float(uv0, i, src[i].uv0, 2);
            }
            if (uv1 != NULL) {
                (void)cgltf_accessor_read_float(uv1, i, src[i].uv1, 2);
            }
        }
        if (flat) {
            for (i = 0; i < ntri; i++) {
                float e1[3];
                float e2[3];
                float n[3];
                int k;
                int j;
                for (k = 0; k < 3; k++) {
                    e1[k] = src[tri[i * 3u + 1u]].pos[k] - src[tri[i * 3u]].pos[k];
                    e2[k] = src[tri[i * 3u + 2u]].pos[k] - src[tri[i * 3u]].pos[k];
                }
                cross3(e1, e2, n);
                norm3(n);
                for (j = 0; j < 3; j++) {
                    SuAssetVertex v = src[tri[i * 3u + (uint32_t)j]];
                    memcpy(v.normal, n, sizeof n);
                    b->v[b->nv + i * 3u + (uint32_t)j] = v;
                    b->idx[b->ni + i * 3u + (uint32_t)j] = b->nv + i * 3u + (uint32_t)j - v0;
                }
            }
            free(src);
            b->nv += ntri * 3u;
        } else {
            for (i = 0; i < ntri * 3u; i++) {
                b->idx[b->ni + i] = tri[i];
            }
            b->nv += nv;
        }
    }
    free(tri);
    /* indices are local to the primitive so far; rebase */
    for (i = b->ni; i < b->ni + ntri * 3u; i++) {
        b->idx[i] += v0;
    }
    b->ni += ntri * 3u;
    for (i = v0; i < b->nv; i++) {
        if (!(b->v[i].normal[0] != 0.0f || b->v[i].normal[1] != 0.0f || b->v[i].normal[2] != 0.0f)) {
            b->v[i].normal[1] = 1.0f; /* degenerate input normal */
        }
    }
    if (tng == NULL) {
        gen_tangents(b, v0, i0);
    }
    memset(&s, 0, sizeof s);
    s.first_index = i0;
    s.index_count = ntri * 3u;
    s.material = cook_material(c, p->material, st_mat);
    b->sm[b->ns++] = s;
    c->rep->triangles += ntri;
    return *st_mat;
}

/* Writes mesh mi (or its winding-flipped variant); *id_out = 0 when it has no triangles. */
static uint32_t cook_mesh(Cook *c, size_t mi, int mirrored, uint64_t *id_out) {
    uint8_t *state = mirrored ? c->mirror_state : c->mesh_state;
    const cgltf_mesh *m = &c->data->meshes[mi];
    Build b;
    uint32_t st = SU_OK;
    size_t pi;
    *id_out = 0;
    if (state[mi] == 1u) {
        *id_out = mesh_id(c, mi, mirrored);
        return SU_OK;
    }
    if (state[mi] == 2u) {
        return SU_OK;
    }
    memset(&b, 0, sizeof b);
    for (pi = 0; pi < m->primitives_count && st == SU_OK; pi++) {
        st = add_primitive(c, &b, &m->primitives[pi], mi, pi, &st);
    }
    if (st == SU_OK && b.ns == 0u) {
        state[mi] = 2u;
    } else if (st == SU_OK) {
        uint32_t i;
        if (mirrored) {
            for (i = 0; i < b.ni; i += 3u) {
                uint32_t t = b.idx[i + 1u];
                b.idx[i + 1u] = b.idx[i + 2u];
                b.idx[i + 2u] = t;
            }
            for (i = 0; i < b.nv; i++) {
                b.v[i].tangent[3] = -b.v[i].tangent[3];
            }
        }
        *id_out = mesh_id(c, mi, mirrored);
        st = su_asset_writer_add_mesh(c->w, *id_out, b.v, b.nv, b.idx, b.ni, b.sm, b.ns);
        if (st != SU_OK) {
            msgf(c->msg, "mesh %u could not be written (status %u)", (unsigned)mi, (unsigned)st);
            *id_out = 0;
        } else {
            state[mi] = 1u;
            c->rep->meshes++;
            c->rep->submeshes += b.ns;
            c->rep->vertices += b.nv;
        }
    }
    free(b.v);
    free(b.idx);
    free(b.sm);
    return st;
}

/* ---------- nodes, LOD groups, model ---------- */

typedef struct Placed {
    const cgltf_node *node;
    char base[256];
    int lod;            /* -1: not a LOD-suffixed name */
} Placed;

static void split_lod(const char *name, char *base, size_t cap, int *lod) {
    size_t n = name != NULL ? strlen(name) : 0u;
    size_t i = n;
    *lod = -1;
    snprintf(base, cap, "%s", name != NULL ? name : "");
    while (i > 0u && isdigit((unsigned char)name[i - 1u])) {
        i--;
    }
    if (i < n && i >= 4u && n - i <= 3u && name[i - 4u] == '_' && toupper((unsigned char)name[i - 3u]) == 'L' &&
        toupper((unsigned char)name[i - 2u]) == 'O' && toupper((unsigned char)name[i - 1u]) == 'D') {
        *lod = atoi(name + i);
        if (i - 4u < cap) {
            base[i - 4u] = '\0';
        }
    }
}

static int collect(const cgltf_node *n, Placed **list, uint32_t *count, uint32_t *cap, int depth) {
    size_t i;
    if (depth > 256) {
        return 0;
    }
    if (n->mesh != NULL) {
        if (!grow((void **)list, cap, *count + 1u, sizeof **list)) {
            return 0;
        }
        (*list)[*count].node = n;
        split_lod(n->name, (*list)[*count].base, sizeof (*list)[*count].base, &(*list)[*count].lod);
        (*count)++;
    }
    for (i = 0; i < n->children_count; i++) {
        if (!collect(n->children[i], list, count, cap, depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static float det3(const float *m) {
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

static uint32_t cook_model(Cook *c) {
    const cgltf_data *d = c->data;
    Placed *list = NULL;
    uint32_t count = 0;
    uint32_t cap = 0;
    SuAssetModelNode *nodes = NULL;
    uint32_t nn = 0;
    uint8_t *used = NULL;
    uint32_t st = SU_OK;
    uint32_t i;
    size_t k;
    const cgltf_scene *scene = d->scene != NULL ? d->scene : d->scenes_count > 0u ? &d->scenes[0] : NULL;
    char name[MAX_NAME];
    if (scene != NULL) {
        for (k = 0; k < scene->nodes_count; k++) {
            if (!collect(scene->nodes[k], &list, &count, &cap, 0)) {
                st = SU_E_RESOURCE_LIMIT;
            }
        }
    } else {
        for (k = 0; k < d->nodes_count; k++) {
            if (d->nodes[k].parent == NULL && !collect(&d->nodes[k], &list, &count, &cap, 0)) {
                st = SU_E_RESOURCE_LIMIT;
            }
        }
    }
    if (st == SU_OK && count == 0u) {
        msgf(c->msg, "the scene places no mesh");
        st = SU_E_PARSE;
    }
    if (st == SU_OK) {
        nodes = calloc(count, sizeof *nodes);
        used = calloc(count, 1);
        if (nodes == NULL || used == NULL) {
            st = SU_E_RESOURCE_LIMIT;
        }
    }
    for (i = 0; i < count && st == SU_OK; i++) {
        /* A group: this entry plus later unused entries with the same base and a LOD suffix (or the plain base
         * name as LOD 0). Each LOD level keeps the lowest-numbered entry. */
        uint32_t members[SU_ASSET_MAX_LODS];
        int levels[SU_ASSET_MAX_LODS];
        uint32_t nm = 0;
        uint32_t j;
        SuAssetModelNode *mn;
        float xf[16];
        int mirrored;
        if (used[i]) {
            continue;
        }
        members[0] = i;
        levels[0] = list[i].lod < 0 ? 0 : list[i].lod;
        nm = 1;
        used[i] = 1u;
        for (j = i + 1u; j < count && list[i].base[0] != '\0'; j++) {
            int lvl = list[j].lod < 0 ? 0 : list[j].lod;
            uint32_t q;
            if (used[j] || strcmp(list[j].base, list[i].base) != 0 || (list[i].lod < 0 && list[j].lod < 0)) {
                continue;
            }
            for (q = 0; q < nm && levels[q] != lvl; q++) {
            }
            if (q < nm) {
                continue; /* level taken: another placement of the same object starts its own group */
            }
            if (nm == SU_ASSET_MAX_LODS) {
                warn(c, "%s: more than %u LODs; extra levels dropped", list[i].base, SU_ASSET_MAX_LODS);
                used[j] = 1u;
                continue;
            }
            members[nm] = j;
            levels[nm] = lvl;
            nm++;
            used[j] = 1u;
        }
        /* sort by level */
        for (j = 1; j < nm; j++) {
            uint32_t q = j;
            while (q > 0u && levels[q - 1u] > levels[q]) {
                int tl = levels[q];
                uint32_t tm = members[q];
                levels[q] = levels[q - 1u];
                members[q] = members[q - 1u];
                levels[q - 1u] = tl;
                members[q - 1u] = tm;
                q--;
            }
        }
        cgltf_node_transform_world(list[members[0]].node, xf);
        mirrored = det3(xf) < 0.0f;
        mn = &nodes[nn];
        memcpy(mn->transform, xf, sizeof xf);
        mn->lod_count = 0;
        for (j = 0; j < nm && st == SU_OK; j++) {
            uint64_t id = 0;
            st = cook_mesh(c, cgltf_mesh_index(d, list[members[j]].node->mesh), mirrored, &id);
            if (st == SU_OK && id != 0u) {
                mn->lod_mesh[mn->lod_count] = id;
                mn->lod_screen[mn->lod_count] = c->lod_screen[mn->lod_count];
                mn->lod_count++;
            }
        }
        if (st == SU_OK && mn->lod_count > 0u) {
            const char *nname = list[members[0]].node->name;
            mn->name_hash = nname != NULL && nname[0] != '\0' ? (uint32_t)su_asset_id(list[i].base[0] ? list[i].base : nname) : 0u;
            if (mn->lod_count > 1u) {
                c->rep->lod_groups++;
            }
            nn++;
        }
    }
    if (st == SU_OK && nn == 0u) {
        msgf(c->msg, "no placed mesh has triangles");
        st = SU_E_PARSE;
    }
    if (st == SU_OK) {
        snprintf(name, sizeof name, "%s/model", c->prefix);
        st = su_asset_writer_add_model(c->w, su_asset_id(name), nodes, nn);
        c->rep->model_nodes = nn;
    }
    free(list);
    free(nodes);
    free(used);
    return st;
}

static uint32_t status_of(cgltf_result r) {
    switch (r) {
    case cgltf_result_success:
        return SU_OK;
    case cgltf_result_file_not_found:
    case cgltf_result_io_error:
        return SU_E_IO;
    case cgltf_result_out_of_memory:
        return SU_E_RESOURCE_LIMIT;
    default:
        return SU_E_PARSE;
    }
}

static const char *cgltf_reason(cgltf_result r) {
    static const char *names[] = {"success", "data too short", "unknown format", "invalid JSON", "invalid glTF",
                                  "invalid options", "file not found", "I/O error", "out of memory", "legacy glTF 1.0"};
    return (size_t)r < sizeof names / sizeof names[0] ? names[r] : "error";
}

uint32_t su_asset_cook_gltf(const char *path, const SuAssetCookOptions *options, uint8_t **pak, size_t *pak_len,
                            SuAssetCookReport *report, char message[256]) {
    cgltf_options co;
    cgltf_result r;
    Cook c;
    SuAssetCookReport local;
    uint32_t st;
    int k;
    if (message != NULL) {
        message[0] = '\0';
    }
    if (path == NULL || pak == NULL || pak_len == NULL ||
        (options != NULL && options->struct_size != sizeof *options)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *pak = NULL;
    *pak_len = 0;
    memset(&c, 0, sizeof c);
    memset(&local, 0, sizeof local);
    c.rep = report != NULL ? report : &local;
    memset(c.rep, 0, sizeof *c.rep);
    c.path = path;
    c.opt = options;
    c.msg = message;
    c.max_tex = options != NULL && options->max_texture_size != 0u ? options->max_texture_size : 4096u;
    c.lod_screen[0] = 1.0f;
    c.lod_screen[1] = 0.25f;
    c.lod_screen[2] = 0.08f;
    c.lod_screen[3] = 0.02f;
    if (options != NULL && options->lod_screen[1] > 0.0f) {
        for (k = 1; k < (int)SU_ASSET_MAX_LODS; k++) {
            c.lod_screen[k] = options->lod_screen[k];
        }
    }
    if (options != NULL && options->name != NULL && options->name[0] != '\0') {
        snprintf(c.prefix, sizeof c.prefix, "%s", options->name);
    } else {
        const char *s = strrchr(path, '/');
        const char *b = strrchr(path, '\\');
        char *dot;
        s = b != NULL && (s == NULL || b > s) ? b : s;
        snprintf(c.prefix, sizeof c.prefix, "%s", s != NULL ? s + 1 : path);
        dot = strrchr(c.prefix, '.');
        if (dot != NULL && dot != c.prefix) {
            *dot = '\0';
        }
    }
    memset(&co, 0, sizeof co);
    r = cgltf_parse_file(&co, path, &c.data);
    if (r != cgltf_result_success) {
        msgf(message, "%s: %s", path, cgltf_reason(r));
        return status_of(r);
    }
    r = cgltf_load_buffers(&co, c.data, path);
    if (r != cgltf_result_success) {
        msgf(message, "%s: buffers: %s", path, cgltf_reason(r));
        cgltf_free(c.data);
        return status_of(r);
    }
    r = cgltf_validate(c.data);
    if (r != cgltf_result_success) {
        msgf(message, "%s: validation failed (cgltf: %s): an accessor, buffer view or index is out of range", path,
             cgltf_reason(r));
        cgltf_free(c.data);
        return SU_E_PARSE;
    }
    if (c.data->animations_count > 0u) {
        warn(&c, "%u animations ignored", (unsigned)c.data->animations_count);
    }
    if (c.data->skins_count > 0u) {
        warn(&c, "%u skins ignored (meshes cooked in bind pose)", (unsigned)c.data->skins_count);
    }
    if (c.data->lights_count > 0u) {
        warn(&c, "%u KHR_lights_punctual lights ignored", (unsigned)c.data->lights_count);
    }
    c.mesh_state = calloc(c.data->meshes_count + 1u, 1);
    c.mirror_state = calloc(c.data->meshes_count + 1u, 1);
    c.tex_state = calloc(c.data->images_count * 3u + 1u, 1);
    c.mat_done = calloc(c.data->materials_count + 1u, 1);
    st = c.mesh_state && c.mirror_state && c.tex_state && c.mat_done ? su_asset_writer_create(&c.w) : SU_E_RESOURCE_LIMIT;
    if (st == SU_OK) {
        st = cook_model(&c);
    }
    if (st == SU_OK) {
        st = su_asset_writer_finish(c.w, pak, pak_len);
    }
    su_asset_writer_destroy(c.w);
    free(c.mesh_state);
    free(c.mirror_state);
    free(c.tex_state);
    free(c.mat_done);
    cgltf_free(c.data);
    if (st != SU_OK && message != NULL && message[0] == '\0') {
        msgf(message, "%s: cook failed (status %u)", path, (unsigned)st);
    }
    return st;
}
