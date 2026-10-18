/* Offline cook step and pack inspector (sutekh/asset.h).
 *
 *   asset_cook cook IN.gltf|IN.glb OUT.supak [--name NAME] [--max-texture N] [--no-mips] [--bc]
 *       imports the glTF file, writes the pack, prints the report and the pack's content SHA-256 (its address)
 *   asset_cook info PACK.supak
 *       validates a pack and lists its resources
 *
 * Exit status: 0 success, 1 failure, 2 usage, 3 built without the importer (cook only). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/asset.h"

static const char *type_name(uint32_t t) {
    switch (t) {
    case SU_ASSET_MESH:
        return "mesh";
    case SU_ASSET_TEXTURE:
        return "texture";
    case SU_ASSET_MATERIAL:
        return "material";
    case SU_ASSET_MODEL:
        return "model";
    default:
        return "?";
    }
}

static void hex32(const uint8_t *d, char *out) {
    static const char x[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = x[d[i] >> 4];
        out[i * 2 + 1] = x[d[i] & 15];
    }
    out[64] = '\0';
}

static double wall_ms(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int info(const char *path) {
    SuAssetPak *p = NULL;
    char msg[256];
    uint8_t d[32];
    char hex[65];
    uint32_t i;
    size_t len = 0;
    uint32_t st;
    FILE *fh = fopen(path, "rb");
    uint8_t *raw = NULL;
    long n = -1;
    if (fh != NULL && fseek(fh, 0, SEEK_END) == 0 && (n = ftell(fh)) >= 0 && fseek(fh, 0, SEEK_SET) == 0) {
        raw = malloc((size_t)n + 1u);
        if (raw != NULL && fread(raw, 1, (size_t)n, fh) != (size_t)n) {
            free(raw);
            raw = NULL;
        }
    }
    if (fh != NULL) {
        fclose(fh);
    }
    if (raw == NULL) {
        fprintf(stderr, "%s: cannot read\n", path);
        return 1;
    }
    /* info also checks every payload hash */
    st = su_asset_pak_open(raw, (size_t)n, SU_ASSET_PAK_VERIFY_ENTRIES, &p, msg);
    free(raw);
    if (st != SU_OK) {
        fprintf(stderr, "%s: %s\n", path, msg);
        return 1;
    }
    su_asset_pak_digest(p, d);
    hex32(d, hex);
    (void)su_asset_pak_bytes(p, &len);
    printf("pack %s: %zu bytes, %u resources, content sha256 %s\n", path, len, (unsigned)su_asset_pak_entry_count(p), hex);
    for (i = 0; i < su_asset_pak_entry_count(p); i++) {
        SuAssetEntry e;
        (void)su_asset_pak_entry(p, i, &e);
        printf("  %016llx %-8s %10llu bytes", (unsigned long long)e.id, type_name(e.type), (unsigned long long)e.size);
        if (e.type == SU_ASSET_MESH) {
            SuAssetMeshView m;
            (void)su_asset_pak_mesh(p, e.id, &m);
            printf("  %u vertices, %u triangles, %u submeshes, radius %.3f", (unsigned)m.header->vertex_count,
                   (unsigned)(m.header->index_count / 3u), (unsigned)m.header->submesh_count, (double)m.header->radius);
        } else if (e.type == SU_ASSET_TEXTURE) {
            SuAssetTextureView t;
            (void)su_asset_pak_texture(p, e.id, &t);
            printf("  %ux%u %s, %u mips%s", (unsigned)t.header->width, (unsigned)t.header->height,
                   su_asset_texture_format_name(t.header->format), (unsigned)t.header->mip_count,
                   (t.header->flags & SU_ASSET_TEX_FLAG_NORMAL_MAP) != 0u ? ", normal map" : "");
        } else if (e.type == SU_ASSET_MODEL) {
            SuAssetModelView m;
            (void)su_asset_pak_model(p, e.id, &m);
            printf("  %u nodes, radius %.3f", (unsigned)m.header->node_count, (double)m.header->radius);
        }
        printf("\n");
    }
    su_asset_pak_close(p);
    return 0;
}

static int cook(int argc, char **argv) {
    SuAssetCookOptions o;
    SuAssetCookReport rep;
    uint8_t *pak = NULL;
    size_t len = 0;
    char msg[256];
    int i;
    uint32_t st;
    double t0;
    FILE *f;
    SuAssetPak *p = NULL;
    uint8_t d[32];
    char hex[65];
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    for (i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            o.name = argv[++i];
        } else if (strcmp(argv[i], "--max-texture") == 0 && i + 1 < argc) {
            o.max_texture_size = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-mips") == 0) {
            o.flags |= SU_ASSET_COOK_NO_MIPS;
        } else if (strcmp(argv[i], "--bc") == 0) {
            o.flags |= SU_ASSET_COOK_BC;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!su_asset_import_available()) {
        fprintf(stderr, "built without the glTF importer (run tools/bootstrap_asset_deps.py and reconfigure)\n");
        return 3;
    }
    t0 = wall_ms();
    st = su_asset_cook_gltf(argv[2], &o, &pak, &len, &rep, msg);
    if (st != SU_OK) {
        fprintf(stderr, "cook failed (status %u): %s\n", (unsigned)st, msg);
        return 1;
    }
    printf("cooked %s in %.1f ms: %u meshes (%u submeshes, %llu vertices, %llu triangles), %u textures (%llu bytes), "
           "%u materials, %u model nodes (%u LOD groups), %u warnings%s%s\n",
           argv[2], wall_ms() - t0, (unsigned)rep.meshes, (unsigned)rep.submeshes, (unsigned long long)rep.vertices,
           (unsigned long long)rep.triangles, (unsigned)rep.textures, (unsigned long long)rep.texture_bytes,
           (unsigned)rep.materials, (unsigned)rep.model_nodes, (unsigned)rep.lod_groups, (unsigned)rep.warnings,
           rep.warnings ? "; first: " : "", rep.first_warning);
    f = fopen(argv[3], "wb");
    if (f == NULL || fwrite(pak, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        free(pak);
        return 1;
    }
    if (su_asset_pak_open(pak, len, 0, &p, msg) != SU_OK) {
        fprintf(stderr, "the cooked pack does not validate: %s\n", msg);
        free(pak);
        return 1;
    }
    su_asset_pak_digest(p, d);
    hex32(d, hex);
    printf("wrote %s: %zu bytes, content sha256 %s\n", argv[3], len, hex);
    su_asset_pak_close(p);
    free(pak);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "cook") == 0) {
        return cook(argc, argv);
    }
    if (argc == 3 && strcmp(argv[1], "info") == 0) {
        return info(argv[2]);
    }
    fprintf(stderr, "usage: asset_cook cook IN.gltf|IN.glb OUT.supak [--name NAME] [--max-texture N] [--no-mips] [--bc]\n"
                    "       asset_cook info PACK.supak\n");
    return 2;
}
