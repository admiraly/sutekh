/* Sutekh presentation (M5 first slice, generation 1): a minimal Vulkan renderer for diagnostic scenes.
 *
 * Boundary (docs/RENDERING_AND_ASSETS.md section 1): the renderer draws what the host gives it each frame and
 * never reads or writes simulation state. A presentation host reads committed state through sutekh/host.h,
 * interpolates between the last two ticks with the clock's alpha (sutekh/clock.h), and passes plain float
 * instances and lines here. Floating point is fine on this side: nothing here feeds back into the simulation.
 *
 * Contents
 *   - Portable helpers (no GPU): vector/matrix math for cameras (Vulkan clip space: y down, depth 0..1),
 *     a chase camera and a free camera, interpolation, procedural meshes (box, sphere, arrow ship), and a
 *     debug-line builder (lines, vectors, planes, ellipses, boxes, axes).
 *   - The renderer: Vulkan 1.2 or later, loaded at run time (no link-time dependency on a Vulkan library), a
 *     Win32 window swapchain or an offscreen target with readback, instanced meshes, depth, debug lines, and CPU
 *     and GPU frame times (timestamp queries when the queue supports them). Without Vulkan every call fails with
 *     a status and a plain-language reason; nothing crashes.
 *
 * Library: su_render (static). Built only when Vulkan headers are available (see cmake/SutekhPresentation.cmake);
 * headless builds never depend on Vulkan. Threading: one renderer is used from one thread. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_RENDER_H
#define SUTEKH_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/asset.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_RENDER_API
#define SU_RENDER_API_VERSION 2u

/* ---- math (column-major 4x4, right-handed world, +y up, cameras look down -z) ---- */

typedef struct SuVec3f {
    float x, y, z;
} SuVec3f;

typedef struct SuQuatf {
    float x, y, z, w;
} SuQuatf;

typedef struct SuMat4f {
    float m[16]; /* m[col * 4 + row] */
} SuMat4f;

SU_RENDER_API SuMat4f su_mat4f_identity(void);
SU_RENDER_API SuMat4f su_mat4f_mul(SuMat4f a, SuMat4f b);
/* View matrix looking from eye toward target. */
SU_RENDER_API SuMat4f su_mat4f_look_at(SuVec3f eye, SuVec3f target, SuVec3f up);
/* Perspective projection to Vulkan clip space (x right, y down, depth 0 at near and 1 at far). */
SU_RENDER_API SuMat4f su_mat4f_perspective(float fov_y_radians, float aspect, float near_z, float far_z);
/* Applies m to the point p (w = 1) and divides by w. */
SU_RENDER_API SuVec3f su_mat4f_transform_point(SuMat4f m, SuVec3f p, float *w_out);
SU_RENDER_API SuVec3f su_quatf_rotate(SuQuatf q, SuVec3f v);
/* Normalized linear interpolation; takes the shorter arc. */
SU_RENDER_API SuQuatf su_quatf_nlerp(SuQuatf a, SuQuatf b, float t);
SU_RENDER_API SuVec3f su_vec3f_lerp(SuVec3f a, SuVec3f b, float t);

/* Chase camera: follows a target from behind and above, with exponential smoothing that does not depend on the
 * frame rate. */
typedef struct SuChaseCamera {
    SuVec3f offset;        /* in the target's frame: behind is +z (the target faces -z), above is +y */
    SuVec3f look_ahead;    /* point looked at, in the target's frame */
    float stiffness;      /* 1/s; larger follows more tightly */
    SuVec3f eye;           /* state */
    SuVec3f at;
    int initialized;
} SuChaseCamera;
SU_RENDER_API SuMat4f su_chase_camera_update(SuChaseCamera *c, SuVec3f target_pos, SuQuatf target_rot, float dt_s);

/* Free camera: position and yaw/pitch, moved by the host (for example from an input record's axes). */
typedef struct SuFreeCamera {
    SuVec3f pos;
    float yaw;    /* radians, 0 looks down -z, positive turns left */
    float pitch;  /* radians, clamped to +-1.5 */
} SuFreeCamera;
/* move: (right, up, forward) in camera axes, units; turn: (yaw, pitch) radians. */
SU_RENDER_API SuMat4f su_free_camera_update(SuFreeCamera *c, SuVec3f move, float turn_yaw, float turn_pitch);

/* ---- procedural meshes ---- */

#define SU_MESH_BOX 0u      /* unit cube, -0.5..0.5 */
#define SU_MESH_SPHERE 1u   /* radius 0.5, 24 x 16 segments */
#define SU_MESH_SHIP 2u     /* arrow-shaped hull pointing down -z, about 1 unit long */
#define SU_MESH_COUNT 3u

typedef struct SuMeshVertex {
    float pos[3];
    float normal[3];
} SuMeshVertex;

/* Vertex and index counts of a built-in mesh; with non-NULL arrays, also writes them (triangle list,
 * counter-clockwise front faces seen from outside). SU_E_INVALID_ARGUMENT for an unknown mesh or small arrays. */
SU_RENDER_API uint32_t su_mesh_build(uint32_t mesh, SuMeshVertex *vertices, uint32_t vertex_cap, uint32_t *vertex_count,
                                     uint16_t *indices, uint32_t index_cap, uint32_t *index_count);

/* ---- per-frame scene ---- */

typedef struct SuRenderInstance {
    float pos[3];
    float scale;
    float rot[4];      /* quaternion x, y, z, w */
    float color[4];    /* linear RGBA */
    uint32_t mesh;     /* SU_MESH_* */
    uint32_t reserved[3];
} SuRenderInstance;

typedef struct SuRenderLineVertex {
    float pos[3];
    uint32_t rgba;     /* 0xAABBGGRR */
} SuRenderLineVertex;

/* Debug-line builder over a caller-owned array. Lines past the capacity are counted in dropped, not written. */
typedef struct SuDebugLines {
    SuRenderLineVertex *v;
    uint32_t cap;      /* vertices (two per line) */
    uint32_t count;
    uint32_t dropped;  /* lines that did not fit */
} SuDebugLines;

SU_RENDER_API void su_debug_line(SuDebugLines *d, SuVec3f a, SuVec3f b, uint32_t rgba);
/* A vector from origin with a small arrow head. */
SU_RENDER_API void su_debug_vector(SuDebugLines *d, SuVec3f origin, SuVec3f vec, uint32_t rgba);
/* An ellipse center + cos(t) * u + sin(t) * v, with segments lines. */
SU_RENDER_API void su_debug_ellipse(SuDebugLines *d, SuVec3f center, SuVec3f u, SuVec3f v, uint32_t segments, uint32_t rgba);
/* A grid on the plane center + a * u + b * v for a, b in [-1, 1], cells x cells, plus the normal u x v. */
SU_RENDER_API void su_debug_plane(SuDebugLines *d, SuVec3f center, SuVec3f u, SuVec3f v, uint32_t cells, uint32_t rgba);
SU_RENDER_API void su_debug_box(SuDebugLines *d, SuVec3f lo, SuVec3f hi, uint32_t rgba);
SU_RENDER_API void su_debug_axes(SuDebugLines *d, SuVec3f origin, SuQuatf rot, float length);

/* ---- large worlds: camera-relative rendering from Q32.32 positions ----
 *
 * The simulation keeps positions in Q32.32 (int64, 1.0 = 1 << 32). Converting a world position of tens of km to
 * float32 loses millimetres (float32 has 24 bits of mantissa: at 50 km the spacing is about 4 mm), which shows as
 * jitter. The camera-relative path subtracts in integers first and converts only the (small) difference. */

typedef struct SuWorldPos {
    int64_t x, y, z;   /* Q32.32 */
} SuWorldPos;

SU_RENDER_API SuWorldPos su_wpos(double x, double y, double z);
/* (a - origin) in float. The subtraction is exact (int64, wrapping is the caller's problem beyond 2^31 units). */
SU_RENDER_API SuVec3f su_wpos_delta(SuWorldPos a, SuWorldPos origin);
/* a + (b - a) * t, with the interpolation done on the difference (exact endpoints for t = 0 and 1). */
SU_RENDER_API SuWorldPos su_wpos_lerp(SuWorldPos a, SuWorldPos b, float t);
SU_RENDER_API SuWorldPos su_wpos_add(SuWorldPos a, SuVec3f d);

SU_RENDER_API SuQuatf su_quatf_mul(SuQuatf a, SuQuatf b);
SU_RENDER_API SuQuatf su_quatf_axis_angle(SuVec3f axis, float radians);
/* Rotation matrix of q (column-major, no translation). */
SU_RENDER_API SuMat4f su_mat4f_from_quat(SuQuatf q);
/* Reversed-Z perspective with an infinite far plane: depth 1 at near_z, approaching 0 at infinity (Vulkan clip
 * space, y down). With a 32-bit float depth buffer this keeps precision from metres to hundreds of km. */
SU_RENDER_API SuMat4f su_mat4f_perspective_rz(float fov_y_radians, float aspect, float near_z);

/* Frustum culling. Planes point inward: a point p is inside when dot(n, p) + d >= 0 for every plane. */
typedef struct SuFrustum {
    float planes[6][4];
    uint32_t count;      /* 5 for infinite-far projections (no far plane), else 6 */
} SuFrustum;
/* Extracts the planes of a view-projection matrix (Vulkan depth 0..1). infinite_far: skip the far plane. */
SU_RENDER_API void su_frustum_from_matrix(SuFrustum *f, SuMat4f view_proj, int infinite_far);
/* 1 when the sphere touches or is inside the frustum, 0 when it is entirely outside. */
SU_RENDER_API int su_frustum_sphere(const SuFrustum *f, SuVec3f center, float radius);

/* Distance LOD: projected diameter of a sphere as a fraction of the viewport height is radius * proj_y / distance
 * (proj_y = 1 / tan(fov_y / 2)). Returns the highest LOD i with fraction < lod_screen[i] (lod_screen decreasing,
 * lod_screen[0] = 1), 0 when the camera is inside the sphere. */
SU_RENDER_API uint32_t su_lod_select(const float *lod_screen, uint32_t lod_count, float radius, float distance, float proj_y);
SU_RENDER_API float su_lod_screen_fraction(float radius, float distance, float proj_y);

/* ---- effects geometry (presentation only): additive camera-facing billboards and ribbon trails ----
 * Built on the CPU into triangles in camera-relative space; the renderer draws them additively into the HDR target
 * (depth-tested against the scene, no depth writes). Colours are linear and may exceed 1 (HDR glow). */

typedef struct SuFxVertex {
    float pos[3];      /* camera-relative */
    float uv[2];       /* u across, v along: the shader fades toward u = 0 and 1 (soft edge) and v = 1 */
    float color[4];    /* linear RGB intensity, alpha scales it */
} SuFxVertex;

typedef struct SuFxBuffer {
    SuFxVertex *v;
    uint32_t cap;      /* vertices; 3 per triangle */
    uint32_t count;
    uint32_t dropped;  /* triangles that did not fit */
} SuFxBuffer;

/* A square quad of half-size size at pos (camera-relative), facing the camera, rotated by rotation radians around
 * the view axis. view is the camera's rotation-only view matrix (su_mat4f_from_quat of the inverse rotation). The
 * shader makes a round soft spot of it. */
SU_RENDER_API void su_fx_billboard(SuFxBuffer *b, SuVec3f pos, float size, float rotation, const float color[4], SuMat4f view);
/* A beam from a to b (camera-relative) of the given width, facing the camera. */
SU_RENDER_API void su_fx_beam(SuFxBuffer *b, SuVec3f a, SuVec3f c, float width, const float color[4]);

typedef struct SuTrailPoint {
    SuWorldPos pos;
    double time_s;
} SuTrailPoint;

/* Ribbon trail over caller-owned storage: a ring of points pushed from interpolated transforms. */
typedef struct SuTrail {
    SuTrailPoint *p;
    uint32_t cap;
    uint32_t head;      /* index of the newest point */
    uint32_t count;
    float lifetime_s;   /* points older than this are dropped */
    float spacing;      /* minimum distance (units) between stored points */
} SuTrail;

SU_RENDER_API void su_trail_init(SuTrail *t, SuTrailPoint *storage, uint32_t cap, float lifetime_s, float spacing);
SU_RENDER_API void su_trail_reset(SuTrail *t);
/* Records the emitter position at time_s: replaces the newest point while it is closer than spacing to the one
 * before it (so the head follows the emitter smoothly), otherwise appends; drops points older than lifetime_s. */
SU_RENDER_API void su_trail_push(SuTrail *t, SuWorldPos pos, double time_s);
/* Builds a camera-facing ribbon: width and colour interpolate from the head (age 0) to the tail (age lifetime). */
SU_RENDER_API void su_trail_build(const SuTrail *t, double now_s, SuWorldPos camera, float width_head, float width_tail,
                                  const float color_head[4], const float color_tail[4], SuFxBuffer *out);

/* ---- HUD: immediate-mode screen-space quads and text ----
 * Pixels, origin top-left. Text uses the built-in 5x7 bitmap font (project-authored, ASCII 32..126) in 8x8 cells
 * of a 128x64 R8 atlas; scale multiplies the 8-pixel cell (integer scales stay crisp). The HUD is drawn after
 * tonemapping, in sRGB output space, alpha-blended. */

typedef struct SuHudVertex {
    float pos[2];
    float uv[2];
    uint32_t rgba;     /* 0xAABBGGRR, sRGB-encoded colour, straight alpha */
} SuHudVertex;

typedef struct SuHud {
    SuHudVertex *v;
    uint32_t cap;      /* vertices; 6 per quad */
    uint32_t count;
    uint32_t dropped;  /* quads that did not fit */
    float width, height;
} SuHud;

#define SU_FONT_ATLAS_W 128u
#define SU_FONT_ATLAS_H 64u
#define SU_FONT_CELL 8u

/* Fills a 128 x 64 R8 coverage atlas (glyph c at cell (c - 32) % 16, (c - 32) / 16; the last cell is solid white,
 * used by rectangles). */
SU_RENDER_API void su_font_atlas(uint8_t atlas[SU_FONT_ATLAS_W * SU_FONT_ATLAS_H]);
/* 7 rows of 5-bit glyph bitmap (bit 4 = leftmost column) of an ASCII character; '?' for unknown ones. */
SU_RENDER_API const uint8_t *su_font_glyph(char c);

SU_RENDER_API void su_hud_begin(SuHud *h, SuHudVertex *storage, uint32_t cap, uint32_t width, uint32_t height);
SU_RENDER_API void su_hud_rect(SuHud *h, float x, float y, float w, float hgt, uint32_t rgba);
/* Draws text (newlines start a new line) and returns the width of the widest line in pixels. */
SU_RENDER_API float su_hud_text(SuHud *h, float x, float y, float scale, uint32_t rgba, const char *text);
SU_RENDER_API float su_hud_textf(SuHud *h, float x, float y, float scale, uint32_t rgba, const char *fmt, ...);
/* Size of text in pixels at scale. */
SU_RENDER_API void su_hud_measure(const char *text, float scale, float *w, float *hgt);

typedef struct SuRenderView {
    SuMat4f view;
    SuMat4f proj;
    float clear[4];      /* linear RGBA */
    SuVec3f light_dir;    /* direction the light travels, world space */
    float ambient;
} SuRenderView;

/* ---- the renderer ---- */

typedef struct SuRenderer SuRenderer;

#define SU_RENDER_DEVICE_OTHER 0u
#define SU_RENDER_DEVICE_INTEGRATED 1u
#define SU_RENDER_DEVICE_DISCRETE 2u
#define SU_RENDER_DEVICE_VIRTUAL 3u
#define SU_RENDER_DEVICE_CPU 4u    /* a software rasterizer: works, but is not representative for timings */

typedef struct SuRenderDeviceInfo {
    char name[256];
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t type;                 /* SU_RENDER_DEVICE_* */
    uint32_t api_version;          /* VK_MAKE_API_VERSION layout */
    uint32_t driver_version;       /* vendor-specific encoding */
    uint32_t meets_baseline;       /* 1: Vulkan 1.2+, a graphics queue, the formats the renderer needs */
    uint32_t timestamps;           /* 1: the graphics queue supports timestamp queries */
    float timestamp_period_ns;
    uint64_t device_local_bytes;   /* largest device-local heap */
    char why_not[160];             /* reason when meets_baseline is 0 */
} SuRenderDeviceInfo;

/* Lists the Vulkan physical devices. *count receives the number found (all of them, even beyond cap).
 * SU_E_UNSUPPORTED_METHOD when no Vulkan loader or no Vulkan 1.2 instance is available; message (optional, 256
 * bytes) says why. */
SU_RENDER_API uint32_t su_render_enumerate(SuRenderDeviceInfo *out, uint32_t cap, uint32_t *count, char message[256]);

#define SU_RENDER_FLAG_VALIDATION 0x1u   /* enable VK_LAYER_KHRONOS_validation when installed; errors are counted */
#define SU_RENDER_FLAG_NO_VSYNC 0x2u     /* prefer MAILBOX, then IMMEDIATE, over FIFO */

typedef struct SuRenderDesc {
    uint32_t struct_size;          /* sizeof(SuRenderDesc), or SU_RENDER_DESC_V1_SIZE for the first-slice fields */
    uint32_t flags;                /* SU_RENDER_FLAG_* */
    void *hwnd;                    /* Windows HWND to present to, or NULL for an offscreen target */
    uint32_t width, height;        /* offscreen size, or the initial window size */
    uint32_t max_instances;        /* debug instances per frame; 0 = 16384 */
    uint32_t max_line_vertices;    /* per frame; 0 = 131072 */
    int32_t device_index;          /* index from su_render_enumerate, or -1 for the best device */
    /* ---- fields after this line need struct_size == sizeof(SuRenderDesc) ---- */
    const char *pipeline_cache_path; /* load at create and save at destroy (NULL: no persistence) */
    uint32_t upload_budget_bytes;  /* staging bytes copied per frame (each frame slot owns this much); 0 = 16 MiB */
    uint32_t max_resources;        /* meshes + textures + materials + models alive at once; 0 = 16384 */
    uint32_t max_draw_instances;   /* object-node-submesh instances per frame after culling; 0 = 65536 */
    uint32_t max_fx_vertices;      /* per frame; 0 = 196608 */
    uint32_t max_hud_vertices;     /* per frame; 0 = 49152 */
} SuRenderDesc;
#define SU_RENDER_DESC_V1_SIZE ((uint32_t)offsetof(SuRenderDesc, pipeline_cache_path))

/* Creates the renderer. SU_E_UNSUPPORTED_METHOD without a usable Vulkan device (message says why), SU_E_IO for
 * a surface or swapchain the platform refuses, SU_E_RESOURCE_LIMIT for out-of-memory. Every pipeline is created
 * here (through the pipeline cache when one is configured), never during a frame. */
SU_RENDER_API uint32_t su_render_create(const SuRenderDesc *desc, SuRenderer **out, char message[256]);
/* Waits for the device, saves the pipeline cache (when configured) and frees everything. */
SU_RENDER_API void su_render_destroy(SuRenderer *r);
SU_RENDER_API uint32_t su_render_device(const SuRenderer *r, SuRenderDeviceInfo *out);
/* Window resized (also detected from the swapchain). A zero size pauses presentation. */
SU_RENDER_API void su_render_resize(SuRenderer *r, uint32_t width, uint32_t height);

/* ---- GPU resources: asynchronous upload, generational handles, deferred retirement ----
 *
 * Creating a resource records it and returns at once; its bytes are copied to the GPU by later frame calls, at
 * most upload_budget_bytes per frame, through per-frame staging buffers and copy commands recorded ahead of that
 * frame's rendering (no queue wait, no blocking transfer in the frame path). Until a resource is READY it is not
 * drawn: a model node whose mesh is pending is skipped, a material whose textures are pending samples the
 * built-in defaults (white; flat normal). The data passed in must stay valid and unchanged until the resource is
 * READY or FAILED, unless SU_RENDER_UPLOAD_COPY is given (then it is copied at creation).
 *
 * su_render_release retires a handle: it becomes invalid at once (its generation is bumped, so stale handles are
 * refused), and the GPU objects are destroyed only after every frame that could have used them has completed.
 * Handles are never part of simulation state. */

typedef uint64_t SuRenderHandle;   /* 0 = none */

#define SU_RENDER_STATE_INVALID 0u /* unknown, stale or released handle */
#define SU_RENDER_STATE_PENDING 1u
#define SU_RENDER_STATE_READY 2u
#define SU_RENDER_STATE_FAILED 3u  /* see su_render_last_error; the handle still has to be released */

#define SU_RENDER_UPLOAD_COPY 0x1u

typedef struct SuRenderMeshDesc {
    const SuAssetVertex *vertices;
    uint32_t vertex_count;
    const uint32_t *indices;
    uint32_t index_count;
    const SuAssetSubmesh *submeshes;   /* ranges and bounding spheres; their material ids are ignored */
    uint32_t submesh_count;
    const SuRenderHandle *materials;   /* per submesh, 0 = the default material; NULL = all default */
    float center[3];                   /* bounding sphere of the mesh */
    float radius;
    uint32_t flags;                    /* SU_RENDER_UPLOAD_COPY */
} SuRenderMeshDesc;

typedef struct SuRenderTextureDesc {
    uint32_t width, height;
    uint32_t format;                   /* SU_TEX_* (sutekh/asset.h); BCn needs device support */
    uint32_t mip_count;
    const uint8_t *const *mip_data;    /* level i: su_asset_texture_level_size bytes */
    uint32_t flags;                    /* SU_RENDER_UPLOAD_COPY */
} SuRenderTextureDesc;

typedef struct SuRenderMaterialDesc {
    SuAssetMaterial factors;           /* its texture ids are ignored; tex_info selects UV set and wrap modes */
    SuRenderHandle textures[SU_ASSET_SLOT_COUNT]; /* 0 = none */
} SuRenderMaterialDesc;

typedef struct SuRenderModelNode {
    float transform[16];               /* model space from mesh space, column-major (any affine, mirrored too) */
    SuRenderHandle lod_mesh[SU_ASSET_MAX_LODS];
    float lod_screen[SU_ASSET_MAX_LODS];
    uint32_t lod_count;
} SuRenderModelNode;

SU_RENDER_API uint32_t su_render_mesh_create(SuRenderer *r, const SuRenderMeshDesc *d, SuRenderHandle *out);
SU_RENDER_API uint32_t su_render_texture_create(SuRenderer *r, const SuRenderTextureDesc *d, SuRenderHandle *out);
SU_RENDER_API uint32_t su_render_material_create(SuRenderer *r, const SuRenderMaterialDesc *d, SuRenderHandle *out);
/* A model references meshes by handle (they must exist; the nodes are copied). READY when every LOD 0 mesh is. */
SU_RENDER_API uint32_t su_render_model_create(SuRenderer *r, const SuRenderModelNode *nodes, uint32_t count,
                                              SuRenderHandle *out);
SU_RENDER_API uint32_t su_render_resource_state(const SuRenderer *r, SuRenderHandle h);
/* SU_E_STALE_HANDLE for an unknown or already released handle. */
SU_RENDER_API uint32_t su_render_release(SuRenderer *r, SuRenderHandle h);

/* Everything of a pack, created in dependency order (textures, materials, meshes, models). */
typedef struct SuRenderPak {
    SuRenderHandle model;              /* the requested (or first) model */
    SuRenderHandle *handles;           /* every created resource (malloc'd; su_render_pak_release frees it) */
    uint64_t *ids;                     /* their pack ids, same order */
    uint32_t count;
    uint32_t meshes, textures, materials, models;
} SuRenderPak;
/* Uploads a validated pack. model_id 0 picks the first model in the pack. The pack must stay open until
 * su_render_pak_state returns READY (or FAILED). Textures whose format the device cannot sample are reported FAILED
 * and their materials keep the defaults. */
SU_RENDER_API uint32_t su_render_upload_pak(SuRenderer *r, const SuAssetPak *pak, uint64_t model_id, SuRenderPak *out);
/* READY when every resource is ready, FAILED when any failed, else PENDING. */
SU_RENDER_API uint32_t su_render_pak_state(const SuRenderer *r, const SuRenderPak *p);
SU_RENDER_API void su_render_pak_release(SuRenderer *r, SuRenderPak *p);
/* Handle created for a pack id, or 0. */
SU_RENDER_API SuRenderHandle su_render_pak_handle(const SuRenderPak *p, uint64_t id);

/* ---- the camera-relative frame ---- */

#define SU_TONEMAP_ACES 0u             /* ACES-fitted filmic curve (Narkowicz 2015) */
#define SU_TONEMAP_CLAMP 1u            /* exposure, then clamp (the debug frame uses this) */

typedef struct SuRenderCamera {
    SuWorldPos pos;                    /* Q32.32 */
    SuQuatf rot;                       /* orientation: the camera looks down its -z with +y up */
    float fov_y;                       /* radians */
    float near_z;                      /* reversed-Z, infinite far plane */
} SuRenderCamera;

typedef struct SuRenderLighting {
    SuVec3f sun_dir;                   /* direction the light travels, world space */
    float sun_intensity;               /* linear multiplier of sun_color */
    SuVec3f sun_color;
    float exposure;                    /* linear multiplier before tonemapping; 0 = 1 */
    SuVec3f sky_zenith;                /* procedural sky and ambient: colour toward +up */
    float ambient;                     /* ambient (sky) light multiplier */
    SuVec3f sky_horizon;
    float sun_disc;                    /* angular radius of the visible sun disc, radians (0 = none) */
    SuVec3f sky_nadir;                 /* colour toward -up */
    uint32_t tonemap;                  /* SU_TONEMAP_* */
    SuVec3f up;                        /* the sky up axis */
    uint32_t no_sky;                   /* 1: clear to clear_color instead of drawing the sky */
    float clear_color[4];              /* linear */
} SuRenderLighting;

typedef struct SuRenderObject {
    SuWorldPos pos;                    /* Q32.32 */
    SuQuatf rot;
    float scale;                       /* uniform, > 0 */
    float tint[4];                     /* multiplies base colour and alpha. tint[3] < 1 draws the object translucent\n                                        * whatever its material's alpha mode: in the blended pass (depth-tested, no\n                                        * depth write, back to front by node distance, after opaque and masked), with\n                                        * a masked material's cutoff not applied. tint[3] <= 0 or NaN: not drawn\n                                        * (counted in objects_culled). 1 = unchanged behaviour. */
    float emissive;                    /* multiplies emission; 1 = as authored */
    SuRenderHandle model;
    uint32_t reserved[2];
} SuRenderObject;

typedef struct SuRenderFrameDesc {
    uint32_t struct_size;              /* sizeof(SuRenderFrameDesc) */
    SuRenderCamera camera;
    SuRenderLighting light;
    const SuRenderObject *objects;     /* culled per object and per node; LOD per node */
    uint32_t object_count;
    const SuRenderInstance *debug_instances; /* procedural meshes, positions relative to debug_origin */
    uint32_t debug_instance_count;
    const SuRenderLineVertex *lines;   /* relative to debug_origin */
    uint32_t line_vertex_count;
    SuWorldPos debug_origin;
    const SuFxVertex *fx;              /* camera-relative additive triangles (su_fx_*, su_trail_build) */
    uint32_t fx_vertex_count;
    const SuHudVertex *hud;            /* screen-space triangles (su_hud_*) */
    uint32_t hud_vertex_count;
} SuRenderFrameDesc;

/* Draws one camera-relative frame: procedural sky (or clear colour), PBR-lite opaque, masked and blended object
 * meshes, debug instances and lines, additive effects into an RGBA16F target; then tonemapping to the sRGB output
 * and the HUD on top. Same return values as su_render_frame. Upload work queued by resource creation is recorded
 * at the start of the frame, within the budget. */
SU_RENDER_API uint32_t su_render_frame_ex(SuRenderer *r, const SuRenderFrameDesc *f);

/* The rotation-only view matrix of a camera (what su_fx_billboard needs). */
SU_RENDER_API SuMat4f su_render_camera_view(const SuRenderCamera *c);

typedef struct SuRenderStats {
    uint64_t frame;                /* frames submitted */
    double cpu_ms;                 /* CPU time of the last frame call, excluding wait_ms and present_ms */
    double gpu_ms;                 /* GPU time of the most recent completed frame, or -1 when unavailable */
    double wait_ms;                /* time the frame call waited for a free frame slot or an image */
    double present_ms;             /* time spent in vkQueuePresentKHR (FIFO may block here) */
    uint32_t draws;                /* draw calls of the last frame */
    uint32_t instances;            /* debug instances drawn */
    uint32_t line_vertices;        /* line vertices drawn */
    uint32_t dropped;              /* anything beyond a capacity, not drawn (instances, lines, fx, hud, draw instances) */
    uint32_t swapchain_recreations;
    uint32_t validation_enabled;   /* 1 when VK_LAYER_KHRONOS_validation was requested and found */
    uint32_t validation_errors;    /* messages of error severity from the validation layer (0 when not enabled) */
    uint32_t validation_warnings;  /* messages of warning severity */
    uint32_t present_mode;         /* VkPresentModeKHR in use (window), or UINT32_MAX offscreen */
    uint32_t width, height;
    /* ---- scene (su_render_frame_ex) ---- */
    uint32_t objects;              /* submitted */
    uint32_t objects_culled;       /* whole objects outside the frustum */
    uint32_t nodes_drawn;          /* object nodes drawn (after node culling) */
    uint32_t nodes_culled;
    uint32_t nodes_pending;        /* skipped because their mesh is not READY */
    uint32_t lod_nodes[SU_ASSET_MAX_LODS]; /* nodes drawn at each LOD */
    uint32_t draw_instances;       /* instance records written (node x submesh) */
    uint32_t fx_vertices;
    uint32_t hud_vertices;
    double scene_cpu_ms;           /* culling, LOD and batching, inside cpu_ms */
    /* ---- resources ---- */
    uint32_t resources;            /* alive (not released) */
    uint32_t uploads_pending;      /* resources waiting for upload */
    uint64_t upload_bytes_frame;   /* bytes copied by the last frame */
    uint64_t upload_bytes_total;
    uint32_t retired_pending;      /* released, waiting for their frames to complete */
    uint32_t pipeline_cache;       /* 0 none, 1 loaded and used, 2 file present but rejected (other device or
                                    * driver, or corrupt; started empty), 3 no file yet (started empty) */
    double pipeline_create_ms;     /* time to create every pipeline at su_render_create */
} SuRenderStats;

/* Draws one debug frame: instances (any order; sorted by mesh internally) and line-list vertices with the given
 * view and projection (a standard 0..1 depth projection such as su_mat4f_perspective; the renderer converts it to
 * its reversed-Z depth), no sky, clamp tonemapping. Returns SU_OK, or SU_E_RESOURCE_LIMIT when the window is
 * minimized (nothing drawn), or another status with the reason in su_render_last_error. */
SU_RENDER_API uint32_t su_render_frame(SuRenderer *r, const SuRenderView *view, const SuRenderInstance *instances,
                                       uint32_t instance_count, const SuRenderLineVertex *lines, uint32_t line_vertex_count);
SU_RENDER_API uint32_t su_render_stats(const SuRenderer *r, SuRenderStats *out);
SU_RENDER_API const char *su_render_last_error(const SuRenderer *r);

/* Offscreen only: copies the last frame's colour (RGBA8, rows top to bottom, width * height * 4 bytes). */
SU_RENDER_API uint32_t su_render_readback(SuRenderer *r, uint8_t *rgba, size_t cap);
/* Writes RGBA8 pixels as a binary PPM (P6, alpha dropped). */
SU_RENDER_API uint32_t su_render_write_ppm(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif
