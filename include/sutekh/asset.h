/* Sutekh assets (M5): cooked, hash-addressed resource packs, a glTF 2.0 importer that cooks them, and a background
 * loader.
 *
 * Pipeline
 *   source (.gltf / .glb with PNG, JPEG or KTX2 images)
 *     -> su_asset_cook_gltf (offline, or on the background loader; library su_asset_import, built when the pinned
 *        cgltf and stb_image are present: tools/bootstrap_asset_deps.py)
 *     -> pack bytes (format below; written to disk by tools such as examples/asset/asset_cook)
 *     -> su_asset_pak_open (library su_asset, always built, no third-party code): full validation, then zero-copy
 *        views of meshes, textures, materials and models
 *     -> the renderer uploads them asynchronously (sutekh/render.h, su_render_upload_pak).
 *
 * Pack format, version 1 (all integers little-endian; floats IEEE-754 binary32):
 *   header, 64 bytes: magic "SUAPAK\0\1", u32 version, u32 entry_count, u64 total_size,
 *                     u8 content_sha256[32] (SHA-256 of every byte after the header), u64 reserved (0)
 *   entry table, entry_count x 64 bytes, sorted by id, ids unique and nonzero:
 *                     u64 id, u32 type, u32 type_version, u64 offset, u64 size, u8 sha256[32] (of the payload)
 *   payloads at 16-byte aligned offsets, in table order, not overlapping, inside total_size.
 * The pack is addressed by content_sha256 (su_asset_pak_digest); each resource by its stable id, which comes from
 * a name (su_asset_id), not from its content, so a re-cooked source keeps its ids while their hashes change, and by
 * its payload hash (content-addressed caching of single resources).
 * su_asset_pak_open checks everything above, the content hash (which covers every payload and the table), every
 * payload's internal layout (counts, ranges, index bounds, finite positions, mip sizes) and every cross-reference
 * (materials -> textures, meshes -> materials, models -> meshes); with SU_ASSET_PAK_VERIFY_ENTRIES also every
 * payload hash (a second pass over the bytes). A pack that opens is safe to hand to the renderer.
 *
 * Payload layouts (version 1 of each type) are the structs below, written as-is.
 *
 * Threading: a pack is immutable after open and may be read from any thread. The loader runs one worker thread. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_ASSET_H
#define SUTEKH_ASSET_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_ASSET_PAK_VERSION 1u

#define SU_ASSET_MESH 1u
#define SU_ASSET_TEXTURE 2u
#define SU_ASSET_MATERIAL 3u
#define SU_ASSET_MODEL 4u

/* Stable resource id of a name: the first 8 bytes (little-endian) of SHA-256("sutekh.asset.v1:" + name), with 0
 * mapped to 1 (0 means "none"). The importer names resources "<asset>/mesh/<mesh>", "<asset>/tex/<image>.<srgb|
 * linear>", "<asset>/mat/<material>", "<asset>/model" (glTF names when present and unique, else "#<index>"). */
uint64_t su_asset_id(const char *name);

/* ---- payload types ---- */

/* 56 bytes. Tangent w is the bitangent sign (glTF convention: bitangent = cross(normal, tangent.xyz) * w). */
typedef struct SuAssetVertex {
    float pos[3];
    float normal[3];
    float tangent[4];
    float uv0[2];
    float uv1[2];
} SuAssetVertex;

/* 32 bytes: a contiguous index range drawn with one material (a glTF primitive). */
typedef struct SuAssetSubmesh {
    uint32_t first_index;
    uint32_t index_count;
    uint64_t material;     /* material id, or 0 for the renderer's default material */
    float center[3];       /* bounding sphere of the range, mesh space */
    float radius;
} SuAssetSubmesh;

/* 64 bytes, followed by submesh_count SuAssetSubmesh, vertex_count SuAssetVertex, index_count u32. */
typedef struct SuAssetMeshHeader {
    uint32_t vertex_count;
    uint32_t index_count;      /* a multiple of 3: triangle list, counter-clockwise front faces */
    uint32_t submesh_count;
    uint32_t flags;            /* 0 */
    float aabb_min[3];
    float aabb_max[3];
    float center[3];           /* bounding sphere of the whole mesh */
    float radius;
    uint32_t reserved[2];
} SuAssetMeshHeader;

typedef struct SuAssetMeshView {
    uint64_t id;
    const SuAssetMeshHeader *header;
    const SuAssetSubmesh *submeshes;
    const SuAssetVertex *vertices;
    const uint32_t *indices;
} SuAssetMeshView;

/* Texture formats. BCn are block-compressed 4x4 (BC1/BC4: 8 bytes per block, others 16). */
#define SU_TEX_RGBA8_UNORM 1u
#define SU_TEX_RGBA8_SRGB 2u
#define SU_TEX_BC1_UNORM 3u
#define SU_TEX_BC1_SRGB 4u
#define SU_TEX_BC3_UNORM 5u
#define SU_TEX_BC3_SRGB 6u
#define SU_TEX_BC4_UNORM 7u
#define SU_TEX_BC5_UNORM 8u
#define SU_TEX_BC7_UNORM 9u
#define SU_TEX_BC7_SRGB 10u
#define SU_TEX_BC2_UNORM 11u       /* 16 bytes per block: 4-bit explicit alpha, then BC1 colour */
#define SU_TEX_BC2_SRGB 12u
#define SU_TEX_BC6H_UF16 13u       /* HDR, unsigned half-float RGB, 16 bytes per block */
#define SU_TEX_BC6H_SF16 14u       /* HDR, signed half-float RGB */
#define SU_TEX_RGBA16F 15u         /* 8 bytes per texel, linear half floats (BC6H fallback, HDR sprites) */
#define SU_TEX_FORMAT_COUNT 16u

#define SU_ASSET_MAX_MIPS 16u

/* 64 + 16 * mip_count bytes of header, then the mip data. */
typedef struct SuAssetTextureHeader {
    uint32_t width;
    uint32_t height;
    uint32_t format;           /* SU_TEX_* */
    uint32_t mip_count;        /* 1..16; level i is max(1, width >> i) x max(1, height >> i) */
    uint32_t flags;            /* SU_ASSET_TEX_FLAG_* */
    uint32_t reserved[11];
} SuAssetTextureHeader;
#define SU_ASSET_TEX_FLAG_NORMAL_MAP 0x1u

typedef struct SuAssetMip {
    uint64_t offset;           /* from the start of the payload */
    uint64_t size;
} SuAssetMip;

typedef struct SuAssetTextureView {
    uint64_t id;
    const SuAssetTextureHeader *header;
    const uint8_t *mip_data[SU_ASSET_MAX_MIPS];
    uint64_t mip_size[SU_ASSET_MAX_MIPS];
} SuAssetTextureView;

/* Block-compresses one RGBA8 image (bytes as stored: sRGB-encoded for the _SRGB formats) into SU_TEX_BC1_*,
 * SU_TEX_BC3_* (with alpha), SU_TEX_BC4_UNORM (red) or SU_TEX_BC5_UNORM (red and green); out holds
 * su_asset_texture_level_size(format, width, height) bytes. A simple encoder (principal-axis endpoints, one
 * least-squares refinement for colour), not a quality-tuned one. BC7 is not encoded. */
uint32_t su_asset_bc_encode(uint32_t format, const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *out);

/* Bytes of one mip level of a format, or 0 for an unknown format. */
uint64_t su_asset_texture_level_size(uint32_t format, uint32_t width, uint32_t height);
const char *su_asset_texture_format_name(uint32_t format);

#define SU_ASSET_ALPHA_OPAQUE 0u
#define SU_ASSET_ALPHA_MASK 1u
#define SU_ASSET_ALPHA_BLEND 2u

#define SU_ASSET_SLOT_BASE_COLOR 0u   /* sRGB */
#define SU_ASSET_SLOT_METAL_ROUGH 1u  /* linear; roughness in G, metallic in B (glTF) */
#define SU_ASSET_SLOT_NORMAL 2u       /* linear, tangent space */
#define SU_ASSET_SLOT_EMISSIVE 3u     /* sRGB */
#define SU_ASSET_SLOT_OCCLUSION 4u    /* linear, R */
#define SU_ASSET_SLOT_COUNT 5u

/* Texture slot info: bits 0-7 the UV set (0 or 1), bits 8-11 wrap S, bits 12-15 wrap T. */
#define SU_ASSET_WRAP_REPEAT 0u
#define SU_ASSET_WRAP_CLAMP 1u
#define SU_ASSET_WRAP_MIRROR 2u
#define SU_ASSET_TEX_INFO(uv, wrap_s, wrap_t) ((uint32_t)(uv) | ((uint32_t)(wrap_s) << 8) | ((uint32_t)(wrap_t) << 12))

/* 128 bytes. Metallic-roughness PBR (glTF core plus KHR_materials_emissive_strength). */
typedef struct SuAssetMaterial {
    float base_color[4];       /* linear RGBA factor */
    float emissive[3];         /* linear RGB factor */
    float emissive_strength;   /* multiplier (1 without the extension) */
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;        /* SU_ASSET_ALPHA_MASK */
    uint32_t alpha_mode;       /* SU_ASSET_ALPHA_* */
    uint32_t double_sided;
    uint32_t unlit;            /* KHR_materials_unlit */
    uint64_t texture[SU_ASSET_SLOT_COUNT];   /* texture ids, 0 = none */
    uint32_t tex_info[SU_ASSET_SLOT_COUNT];  /* SU_ASSET_TEX_INFO */
    uint32_t reserved[1];
} SuAssetMaterial;

/* Default factors: white, no emission, metallic 1, roughness 1 (the glTF defaults), opaque. */
void su_asset_material_default(SuAssetMaterial *m);

/* ---- layered paint material (optional second material model) ----
 *
 * A material entry of type_version 2 is a SuAssetMaterial followed by a SuAssetPaint. The first 128 bytes keep
 * their glTF meaning where the paint model uses them (see render.h for the exact shading and blend):
 *   base_color     multiplies the albedo map (rgb) and its alpha
 *   metallic       multiplies the metalness read from the material map (map absent or channel NONE: the value)
 *   roughness      multiplies the roughness read from the roughness map (same rule)
 *   normal_scale   scales the tangent-space x/y of the normal map
 *   occlusion_strength  blend of the AO map into ambient light: ao = mix(1, map, strength)
 *   emissive, emissive_strength   the base glow colour (linear RGB) and intensity: glow = glow map x colour x intensity
 *   alpha_*, double_sided   as for PBR (unlit is ignored)
 * Its texture[] / tex_info[] are unused (zero); the paint model has its own eight slots below.
 * A version 1 material is plain PBR-lite, exactly as before; a pack of only version 1 materials is unchanged. */

/* Paint texture slots. Colour space is part of the slot and is checked: SRGB slots take *_SRGB texture formats,
 * LINEAR slots take UNORM formats (BC4 / BC5 included). A texture of the wrong kind makes the material (and a pack
 * containing it) invalid. The glow slot takes either: its format chooses an sRGB colour map or a linear mask. */
#define SU_PAINT_SLOT_ALBEDO 0u     /* sRGB   rgb albedo, a coverage */
#define SU_PAINT_SLOT_NORMAL 1u     /* linear two-channel tangent-space normal (BC5 style: r = x, g = y, z rebuilt) */
#define SU_PAINT_SLOT_ROUGHNESS 2u  /* linear one channel, selected by channel[SU_PAINT_CH_ROUGHNESS] */
#define SU_PAINT_SLOT_MATERIAL 3u   /* linear, channels meant by channel[SU_PAINT_CH_METAL] and [SU_PAINT_CH_SPECULAR] */
#define SU_PAINT_SLOT_MASK 4u       /* linear, r g b a = weight of layer 0 1 2 3 */
#define SU_PAINT_SLOT_GLOW 5u       /* sRGB rgb glow map, or linear: a one-channel glow mask (BC4 style), selected by
                                     * channel[SU_PAINT_CH_GLOW] (see render.h) */
#define SU_PAINT_SLOT_DIRT 6u       /* linear one channel, selected by channel[SU_PAINT_CH_DIRT] */
#define SU_PAINT_SLOT_AO 7u         /* linear one channel, selected by channel[SU_PAINT_CH_AO] */
#define SU_PAINT_SLOT_COUNT 8u

/* Which texel channel a one-channel input reads: 0 r, 1 g, 2 b, 3 a; SU_PAINT_CH_NONE: do not read the map. */
#define SU_PAINT_CH_ROUGHNESS 0u
#define SU_PAINT_CH_METAL 1u
#define SU_PAINT_CH_SPECULAR 2u
#define SU_PAINT_CH_DIRT 3u
#define SU_PAINT_CH_AO 4u
#define SU_PAINT_CH_GLOW 5u          /* used only when the glow slot holds a linear texture (a mask); default 0 = r */
#define SU_PAINT_CH_COUNT 6u
#define SU_PAINT_CH_NONE 4u

#define SU_PAINT_MAX_LAYERS 4u
#define SU_PAINT_LAYER_MULTIPLY 0x1u     /* blend: multiply instead of lerp (see render.h) */
#define SU_PAINT_LAYER_GLOW_TINT 0x2u    /* the layer recolours the glow it covers with glow_color x glow_strength */
#define SU_PAINT_LAYER_TINT 0x4u         /* blend: tint the raw albedo map by the layer colour (see render.h); not with MULTIPLY */

/* 48 bytes. Layer k is driven by channel k of the mask map. */
typedef struct SuAssetPaintLayer {
    float color[3];            /* linear RGB */
    float roughness;           /* 0..1 */
    float metallic;            /* 0..1 */
    float opacity;             /* scales the mask weight, 0..1 */
    float glow_color[3];       /* linear RGB, used with SU_PAINT_LAYER_GLOW_TINT */
    float glow_strength;       /* >= 0 */
    uint32_t flags;            /* SU_PAINT_LAYER_* */
    uint32_t reserved;
} SuAssetPaintLayer;

/* 336 bytes. */
typedef struct SuAssetPaint {
    uint64_t texture[SU_PAINT_SLOT_COUNT];   /* texture ids, 0 = none (a neutral default is used, see render.h) */
    uint32_t tex_info[SU_PAINT_SLOT_COUNT];  /* SU_ASSET_TEX_INFO */
    uint32_t layer_count;      /* 0..4; layers 0..layer_count-1 exist */
    uint8_t layer_order[4];    /* application order: entry i is the layer applied i-th; a permutation of 0..layer_count-1 */
    uint8_t channel[8];        /* SU_PAINT_CH_* index -> 0..3 or SU_PAINT_CH_NONE */
    float specular;            /* dielectric specular scale: F0 = 0.08 * specular * map (0.5 = the usual 0.04) */
    float dirt_amount;         /* 0..1, scales the dirt map (no map: uniform dirt) */
    float dirt_darken;         /* 0..1, albedo *= 1 - dirt * darken */
    float dirt_roughen;        /* 0..1, roughness = mix(roughness, 1, dirt * roughen) */
    uint32_t reserved[4];
    SuAssetPaintLayer layer[SU_PAINT_MAX_LAYERS];
} SuAssetPaint;

/* Zero layers, identity order, channels: roughness r, metal r, specular none (constant), dirt r, ao r, glow r; specular 0.5,
 * dirt amount 0 (darken 0.6, roughen 0.5). Layer defaults: white, roughness 1, metallic 0, opacity 1, lerp. */
void su_asset_paint_default(SuAssetPaint *p);

#define SU_ASSET_MAX_LODS 4u

/* 128 bytes: a placed mesh. lod_mesh[0] is the full-detail mesh; lod_mesh[i] is used while the object's projected
 * bounding-sphere diameter is below lod_screen[i] of the viewport height (lod_screen strictly decreasing, in (0, 1];
 * lod_screen[0] is unused and 1). */
typedef struct SuAssetModelNode {
    float transform[16];       /* model space from mesh space, column-major */
    uint64_t lod_mesh[SU_ASSET_MAX_LODS];
    float lod_screen[SU_ASSET_MAX_LODS];
    uint32_t lod_count;        /* 1..4 */
    uint32_t name_hash;        /* low 32 bits of su_asset_id(node name), for lookups by the host; 0 if unnamed */
    uint32_t reserved[2];
} SuAssetModelNode;

/* 64 bytes, followed by node_count SuAssetModelNode. */
typedef struct SuAssetModelHeader {
    uint32_t node_count;
    uint32_t flags;
    float center[3];           /* bounding sphere of every node's LOD 0, model space */
    float radius;
    float aabb_min[3];
    float aabb_max[3];
    uint32_t reserved[4];
} SuAssetModelHeader;

typedef struct SuAssetModelView {
    uint64_t id;
    const SuAssetModelHeader *header;
    const SuAssetModelNode *nodes;
} SuAssetModelView;

/* ---- reading packs ---- */

typedef struct SuAssetPak SuAssetPak;

typedef struct SuAssetEntry {
    uint64_t id;
    uint32_t type;
    uint32_t type_version;
    uint64_t offset;
    uint64_t size;
    uint8_t sha256[32];
} SuAssetEntry;

/* Validates a pack (see above) and returns a handle. The bytes are copied into an owned 16-byte aligned buffer
 * unless flags has SU_ASSET_PAK_BORROW, in which case they must stay valid and unchanged until close and be 16-byte
 * aligned. Status: SU_E_PARSE (malformed, or a hash differs; message says which), SU_E_RESOURCE_LIMIT
 * (allocation), SU_E_INVALID_ARGUMENT. message (optional) is 256 bytes. */
#define SU_ASSET_PAK_BORROW 0x1u
#define SU_ASSET_PAK_VERIFY_ENTRIES 0x2u
uint32_t su_asset_pak_open(const void *bytes, size_t len, uint32_t flags, SuAssetPak **out, char message[256]);
/* Reads a whole file and opens it (owned copy). SU_E_IO when the file cannot be read. */
uint32_t su_asset_pak_open_file(const char *path, SuAssetPak **out, char message[256]);
void su_asset_pak_close(SuAssetPak *pak);

uint32_t su_asset_pak_entry_count(const SuAssetPak *pak);
uint32_t su_asset_pak_entry(const SuAssetPak *pak, uint32_t index, SuAssetEntry *out);
/* Index of id in the table; SU_E_INVALID_ARGUMENT when absent or of another type. type 0 matches any type.
 * The typed accessors below return the same status for an unknown id. */
uint32_t su_asset_pak_find(const SuAssetPak *pak, uint64_t id, uint32_t type, uint32_t *index);
void su_asset_pak_digest(const SuAssetPak *pak, uint8_t sha256[32]);
const uint8_t *su_asset_pak_bytes(const SuAssetPak *pak, size_t *len);

uint32_t su_asset_pak_mesh(const SuAssetPak *pak, uint64_t id, SuAssetMeshView *out);
uint32_t su_asset_pak_texture(const SuAssetPak *pak, uint64_t id, SuAssetTextureView *out);
uint32_t su_asset_pak_material(const SuAssetPak *pak, uint64_t id, SuAssetMaterial *out);
/* The paint part of a material entry of type_version 2 (SU_E_INVALID_ARGUMENT for a plain PBR material or unknown id).
 * su_asset_pak_material still returns the first 128 bytes of either version. */
uint32_t su_asset_pak_paint(const SuAssetPak *pak, uint64_t id, SuAssetPaint *out);
uint32_t su_asset_pak_model(const SuAssetPak *pak, uint64_t id, SuAssetModelView *out);

/* ---- writing packs (the importer uses this; hosts can cook procedural content with it too) ---- */

typedef struct SuAssetWriter SuAssetWriter;

uint32_t su_asset_writer_create(SuAssetWriter **out);
void su_asset_writer_destroy(SuAssetWriter *w);
/* SU_E_INVALID_ARGUMENT for id 0 or a duplicate id. Bounds, spheres and payload layout are computed here. */
uint32_t su_asset_writer_add_mesh(SuAssetWriter *w, uint64_t id, const SuAssetVertex *vertices, uint32_t vertex_count,
                                  const uint32_t *indices, uint32_t index_count, const SuAssetSubmesh *submeshes,
                                  uint32_t submesh_count);
/* mip_data[i] holds level i (su_asset_texture_level_size bytes). */
uint32_t su_asset_writer_add_texture(SuAssetWriter *w, uint64_t id, uint32_t width, uint32_t height, uint32_t format,
                                     uint32_t flags, uint32_t mip_count, const uint8_t *const *mip_data);
uint32_t su_asset_writer_add_material(SuAssetWriter *w, uint64_t id, const SuAssetMaterial *m);
/* A paint material (type_version 2): m and p are validated as the reader would (finite values, layer order a
 * permutation, channels, texture colour spaces are checked when the pack is opened). */
uint32_t su_asset_writer_add_paint_material(SuAssetWriter *w, uint64_t id, const SuAssetMaterial *m, const SuAssetPaint *p);
uint32_t su_asset_writer_add_model(SuAssetWriter *w, uint64_t id, const SuAssetModelNode *nodes, uint32_t node_count);
/* Builds the pack (entries sorted by id) into a malloc'd buffer the caller frees with free(). The writer is left
 * empty and reusable. */
uint32_t su_asset_writer_finish(SuAssetWriter *w, uint8_t **bytes, size_t *len);

/* ---- importing glTF 2.0 (library su_asset_import; needs the bootstrap dependencies) ---- */

typedef struct SuAssetCookOptions {
    uint32_t struct_size;      /* sizeof(SuAssetCookOptions) */
    const char *name;          /* resource name prefix; NULL = the file name without extension */
    uint32_t max_texture_size; /* larger RGBA8 images are halved until they fit; 0 = 4096 */
    uint32_t flags;            /* SU_ASSET_COOK_* */
    float lod_screen[SU_ASSET_MAX_LODS]; /* thresholds for name-grouped LODs; zeros = 1, 0.25, 0.08, 0.02 */
} SuAssetCookOptions;
#define SU_ASSET_COOK_NO_MIPS 0x1u        /* keep only level 0 of decoded images */
#define SU_ASSET_COOK_BC 0x2u             /* block-compress decoded images: BC1 (BC3 with alpha) for colour, BC1 for
                                           * metallic-roughness and occlusion, BC5 for normal maps (the shader
                                           * rebuilds z); KTX2 data is kept as stored */

typedef struct SuAssetCookReport {
    uint32_t meshes;           /* mesh resources written (mirrored variants included) */
    uint32_t submeshes;
    uint32_t textures;
    uint32_t materials;
    uint32_t model_nodes;
    uint32_t lod_groups;       /* model nodes with more than one LOD */
    uint64_t vertices;
    uint64_t triangles;
    uint64_t texture_bytes;
    uint32_t warnings;         /* skipped features (animation, skins, morph targets, non-triangle primitives...) */
    char first_warning[160];
} SuAssetCookReport;

/* Imports a .gltf (external or data-URI buffers and images) or .glb file and cooks it into a pack (malloc'd; free
 * with free()). Every node of the default scene (or of scene 0, or every root node when there is no scene) that
 * has a mesh becomes a model node with its flattened world transform; nodes named "<base>_LOD<n>" are grouped as
 * LODs of "<base>" (or "<base>_LOD0"); nodes with a mirroring transform use a winding-flipped copy of their mesh.
 * Missing normals are computed flat (the glTF rule), missing tangents from UVs. Images: PNG and JPEG (decoded to
 * RGBA8, mips generated with an sRGB-correct box filter for colour slots), KTX2 with RGBA8 or BCn data and no
 * supercompression. Status: SU_E_UNSUPPORTED_METHOD when built without the importer, SU_E_IO, SU_E_PARSE (with the
 * reason), SU_E_RESOURCE_LIMIT. */
uint32_t su_asset_cook_gltf(const char *path, const SuAssetCookOptions *options, uint8_t **pak, size_t *pak_len,
                            SuAssetCookReport *report, char message[256]);
/* 1 when the library was built with the glTF importer. */
int su_asset_import_available(void);

/* ---- background loading ---- */

/* Loads packs on one worker thread: reads the file, validates it (su_asset_pak_open), and hands the opened pack
 * back through su_asset_loader_poll. A file whose name ends in .gltf or .glb is cooked first when a cook function
 * is installed (pass su_asset_cook_gltf when the importer is linked). Nothing here touches a renderer; uploads are
 * the renderer's business. */
typedef uint32_t (*SuAssetCookFn)(const char *path, const SuAssetCookOptions *options, uint8_t **pak, size_t *pak_len,
                                  SuAssetCookReport *report, char message[256]);

typedef struct SuAssetLoader SuAssetLoader;

typedef struct SuAssetLoadResult {
    uint64_t user;             /* as passed to su_asset_loader_request */
    uint32_t status;           /* SU_OK, or why it failed */
    SuAssetPak *pak;           /* owned by the caller on SU_OK */
    double load_ms;            /* worker time: read, cook (if any), validate */
    char message[256];
} SuAssetLoadResult;

uint32_t su_asset_loader_create(SuAssetCookFn cook, SuAssetLoader **out);
/* Waits for the worker to finish its current item, then frees every completed but unpolled pack. */
void su_asset_loader_destroy(SuAssetLoader *l);
/* Queues a file (the path is copied). SU_E_RESOURCE_LIMIT when 64 requests are pending. */
uint32_t su_asset_loader_request(SuAssetLoader *l, const char *path, uint64_t user);
/* 1 with one finished request written to out, 0 when none is ready. Never blocks. */
int su_asset_loader_poll(SuAssetLoader *l, SuAssetLoadResult *out);
/* Requests queued or in progress. */
uint32_t su_asset_loader_pending(SuAssetLoader *l);

#ifdef __cplusplus
}
#endif

#endif
