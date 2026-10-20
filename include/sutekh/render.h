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
#include "sutekh/dds.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_RENDER_API
#define SU_RENDER_API_VERSION 8u

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
/* Optional cheap glare for very bright points (a sun, an engine core): a soft core billboard of half-size size plus
 * a horizontal and a vertical streak of half-length streak (in the camera plane; 0 = 8 * size) and a faint halo
 * billboard. About a dozen triangles; the host decides which sources get it (the flag is whether it calls this).
 * color is the core's linear colour; streaks and halo use fractions of it. view as for su_fx_billboard. */
SU_RENDER_API void su_fx_glare(SuFxBuffer *b, SuVec3f pos, float size, float streak, const float color[4], SuMat4f view);

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

/* ---- textured effects: sprites, axis-aligned sprites, textured ribbons ----
 *
 * The same camera-relative triangles (SuFxVertex), but uv is a real texture coordinate and a texture is bound per
 * batch: a SuFxBatch names a range of vertices of SuRenderFrameDesc.fx_tex and the texture they sample. Draw order is
 * the batch order, after the untextured fx, depth tested against the scene and never writing depth.
 *
 * Shading, exactly (t = texture sample, c = vertex colour; all linear, HDR: c.rgb may exceed 1 and feeds bloom):
 *   SU_FX_BLEND_ADD      result = dst + t.rgb * t.a * c.rgb * c.a
 *   SU_FX_BLEND_PREMUL   result = t.rgb * c.rgb * c.a  +  dst * (1 - t.a * c.a)     (t is premultiplied by its alpha)
 * Colour space is the texture's format: *_SRGB formats are decoded to linear on sampling, the others are read as
 * stored. Sprites use mip-mapped linear filtering; wrap modes apply per batch (SU_FX_WRAP_*), so a trail texture can
 * repeat along its length while an atlas clamps. A batch whose texture is not READY (or not a 2D texture) is skipped and
 * counted in SuRenderStats.fx_batches_skipped.
 *
 * Sprite uv conventions: a camera-facing sprite maps uv_rect (u0, v0, u1, v1) with (u0, v0) at the top left as an
 * image is stored. An axis sprite has U across and V along the axis (v0 at the tail, v1 at the head). A textured trail
 * has U along its length and V across it (see SuTrailTex).
 *
 * Soft particles (SuFxBatch.soft_distance > 0): the sprite fades out as it nears the scene surface behind it, so it does
 * not show a hard line where it cuts through geometry. With d = (view depth of the scene at the pixel) - (view depth of
 * the sprite fragment), both along the camera axis in world units, c.a is multiplied by clamp(d / soft_distance, 0, 1)
 * (both blend modes). Background pixels (sky, clear) count as infinitely far. 0 = hard intersections, as before.
 *
 * Sorting (SU_FX_BATCH_SORT): the quads (6 vertices each, as su_fx_sprite and su_fx_sprite_axis write them) of every
 * batch with this flag are sorted together back to front by the distance of their centre from the camera and drawn
 * with their own batch's texture, blend, wrap and softness, at the position of the first sorted batch in the batch
 * order (unsorted batches before and after it keep their order). So many premultiplied explosions with different
 * textures interleave correctly. Ties keep submission order. A sorted batch whose vertex_count is not a multiple of 6 is
 * drawn unsorted in place. Trails (su_trail_build_tex) are not quads: do not flag them. */
typedef uint64_t SuRenderHandle;   /* a renderer resource (see "GPU resources" below); 0 = none */

#define SU_FX_BLEND_ADD 0u
#define SU_FX_BLEND_PREMUL 1u
#define SU_FX_WRAP_CLAMP 0u
#define SU_FX_WRAP_REPEAT 1u
#define SU_FX_WRAP_MIRROR 2u
#define SU_FX_BATCH_SORT 0x1u              /* sort this batch's quads back to front with the other sorted batches */

typedef struct SuFxUvTransform {
    float scale[2];
    float offset[2];
    float speed[2];
} SuFxUvTransform;

typedef struct SuFxLayers {
    SuRenderHandle textures[3];
    SuFxUvTransform uv[3];
    uint32_t atlas_cols, atlas_rows, atlas_frame;
    float angular_fade;
} SuFxLayers;

enum {
    SU_DISTORT_NONE = 0,
    SU_DISTORT_NORMAL = 1,
    SU_DISTORT_VORTEX = 2,
    SU_DISTORT_RING = 3
};

typedef struct SuFxDistortion {
    float strength;
    float falloff;
    float chromatic_split;
    float ring_radius;
    float ring_width;
    uint32_t mode;
} SuFxDistortion;

typedef struct SuFxBatch {
    SuRenderHandle texture;            /* a READY 2D texture */
    uint32_t blend;                    /* SU_FX_BLEND_* */
    uint32_t wrap_u, wrap_v;           /* SU_FX_WRAP_* */
    uint32_t first_vertex;             /* into SuRenderFrameDesc.fx_tex; a multiple of 3 */
    uint32_t vertex_count;             /* a multiple of 3 */
    float soft_distance;               /* world units; > 0: soft particles (see above); 0 = hard, as before */
    uint32_t flags;                    /* SU_FX_BATCH_*; 0 = drawn in batch order, as before */
    SuFxLayers layers;
    SuFxDistortion distortion;
} SuFxBatch;

typedef struct SuRenderFxMaterialDesc {
    SuFxLayers layers;
    uint32_t blend;
    uint32_t two_sided;
    uint32_t wrap_u, wrap_v;
    float soft_distance;
    SuFxDistortion distortion;
} SuRenderFxMaterialDesc;

/* A batch covering the vertices written to buffer since first_vertex (take first_vertex = buffer->count before
 * adding the sprites of one texture). soft_distance and flags are 0; set them on the result. */
SU_RENDER_API SuFxBatch su_fx_batch(SuRenderHandle texture, uint32_t blend, uint32_t wrap_u, uint32_t wrap_v,
                                    uint32_t first_vertex, const SuFxBuffer *buffer_now);

/* UV rectangle (u0, v0, u1, v1) of frame number frame in a cols x rows atlas, frames numbered row by row from the
 * top left; frame is taken modulo cols * rows (so a looping animation just counts up). cols or rows of 0 mean 1. */
SU_RENDER_API void su_fx_atlas_rect(uint32_t cols, uint32_t rows, uint32_t frame, float rect[4]);

/* A camera-facing sprite of half-extents half_w x half_h at pos (camera-relative), rotated by rotation radians around
 * the view axis, with per-sprite colour (linear RGB intensity, alpha scales it) and uv_rect (NULL = 0,0,1,1). view as
 * for su_fx_billboard. */
SU_RENDER_API void su_fx_sprite(SuFxBuffer *b, SuVec3f pos, float half_w, float half_h, float rotation, const float color[4],
                                const float uv_rect[4], SuMat4f view);
/* An axis-aligned (cylindrical) sprite: a quad centred on pos (camera-relative) stretched along the world direction
 * axis (half_length each way) and rotated around that axis to face the camera (half_width across). It stays aligned to
 * the axis whatever the camera does; looking straight along the axis it degenerates to a sliver. Engine flames and
 * plumes: put pos at the middle of the flame, axis along the exhaust. */
SU_RENDER_API void su_fx_sprite_axis(SuFxBuffer *b, SuVec3f pos, SuVec3f axis, float half_length, float half_width,
                                     const float color[4], const float uv_rect[4]);

#define SU_TRAIL_U_AGE 0u                  /* u = u_offset + (age / lifetime) * u_scale */
#define SU_TRAIL_U_DISTANCE 1u             /* u = u_offset + (distance from the head along the trail) * u_scale */
typedef struct SuTrailTex {
    uint32_t mode;                     /* SU_TRAIL_U_* */
    float u_scale;                     /* 0 = 1. AGE: texture repeats across the whole trail; DISTANCE: repeats per unit^-1 */
    float u_offset;                    /* scroll: add speed * time each frame (use SU_FX_WRAP_REPEAT) */
    float v0, v1;                      /* v at the two edges (side vector of the plain ribbon at v0); 0 and 1 for the whole texture */
} SuTrailTex;
/* su_trail_build with uv: u along the length (by age, or by distance) with a scrolling offset, v across. The geometry
 * and colours are exactly those of su_trail_build. */
SU_RENDER_API void su_trail_build_tex(const SuTrail *t, double now_s, SuWorldPos camera, float width_head, float width_tail,
                                      const float color_head[4], const float color_tail[4], const SuTrailTex *tex,
                                      SuFxBuffer *out);

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
    uint32_t bloom;                /* 1: the HDR format can be sampled with linear filtering, which bloom and auto
                                    * exposure need. 0: SuRenderPost requests are ignored (plain tonemap). */
    uint32_t bc_textures;          /* 1: BC1..BC7 (BC6H included) textures are sampled natively; 0: decode on the CPU
                                    * (su_bc6h_decode_block, su_dds_decode_rgba32f) and upload RGBA16F */
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
    uint32_t max_fx_tex_vertices;  /* textured fx per frame; 0 = 98304 */
    uint32_t max_fx_batches;       /* textured fx batches per frame; 0 = 256 */
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
    const uint8_t *const *mip_data;    /* mip_data[mip * layers + layer]: su_asset_texture_level_size bytes each */
    uint32_t flags;                    /* SU_RENDER_UPLOAD_COPY */
    uint32_t layers;                   /* 0 or 1: a 2D texture; 6: a cube map (width == height; faces +X -X +Y -Y +Z -Z),
                                        * sampled through a cube view; the full mip chain is uploaded as given */
    uint32_t depth;
} SuRenderTextureDesc;

typedef struct SuRenderMaterialDesc {
    SuAssetMaterial factors;           /* its texture ids are ignored; tex_info selects UV set and wrap modes */
    SuRenderHandle textures[SU_ASSET_SLOT_COUNT]; /* 0 = none */
    uint32_t overlay;
} SuRenderMaterialDesc;

/* ---- layered paint material ----
 *
 * A second material model beside PBR-lite, for craft painted through a mask. A paint material is created with
 * su_render_paint_material_create (or comes from a pack, material entries of type_version 2, see sutekh/asset.h);
 * meshes reference it like any other material, and PBR-lite materials, goldens and every existing path are untouched.
 *
 * Texture slots (SU_PAINT_SLOT_*, colour space per slot, enforced at creation: SRGB slots refuse non-sRGB formats and
 * LINEAR slots refuse sRGB ones, SU_E_INVALID_ARGUMENT):
 *   albedo sRGB | normal linear (BC5 or RG; z rebuilt) | roughness linear | material linear | mask linear |
 *   glow sRGB or linear | dirt linear | ao linear.
 * The glow slot's format picks its meaning: an sRGB texture is an RGB glow map (as always); a linear one (BC4, R8,
 * RGBA8 UNORM...) is a one-channel intensity mask read from channel[SU_PAINT_CH_GLOW] (0..3; NONE reads 1), so the
 * colour comes from the material: glow map.rgb below becomes vec3(mask channel).
 * A slot without a texture (or whose texture is still uploading) reads: albedo, roughness, material, glow, dirt, ao
 * white (so the factor alone decides), normal flat, mask all zero (no layer shows).
 *
 * The surface, exactly (all values linear; mix(x, y, w) = x + (y - x) * w; clamp to 0..1 where written):
 *   A = albedo.rgb * base_color.rgb            alpha = albedo.a * base_color.a * instance tint.a
 *   R = roughness_factor * (roughness map channel, or 1)        M = metallic_factor * (material map channel, or 1)
 *   S = specular * (material map specular channel, or 1)        T = glow colour (emissive), I = glow intensity
 *       (emissive_strength)
 *   for i = 0 .. layer_count - 1, k = layer_order[i]:           (later layers sit on top of earlier ones)
 *       w = clamp(mask[k]) * clamp(layer[k].opacity)            mask[k] is channel k (r g b a) of the mask map
 *       lerp:      A = mix(A, C_k, w)    R = mix(R, r_k, w)    M = mix(M, m_k, w)
 *       multiply:  A = mix(A, A * C_k, w) R = mix(R, R * r_k, w) M = mix(M, M * m_k, w)
 *       tint:      A = mix(A, albedo.rgb * C_k, w)   R, M as lerp   (albedo.rgb: the raw albedo map, without base_color
 *                  or instance tint, so each layer is an absolute tint of the same detail map; MULTIPLY | TINT is invalid)
 *       glow tint (flag): T = mix(T, G_k, w)   I = mix(I, s_k, w)     (G_k glow_color, s_k glow_strength)
 *   dirt:  d = clamp(dirt_amount * (dirt map channel, or 1))
 *          A = A * (1 - d * dirt_darken)       R = mix(R, 1, d * dirt_roughen)      (metalness and glow unchanged)
 *   R = clamp(R, 0.04, 1)   M = clamp(M)
 *   F0 = mix(0.08 * S, A, M)  (S = 0.5 gives the PBR-lite value 0.04)
 *   ao = mix(1, ao map channel, occlusion_strength)  (1 without a map)
 *   direct light: the same GGX / Smith / Schlick / Lambert terms as PBR-lite, with A, R, M, F0 above
 *   ambient = (diffuse_env(n) * A * (1 - M) + specular_env(reflect) * (F0 * ab.x + ab.y)) * light.ambient * ao
 *   glow    = glow map.rgb * T * I * object.emissive      (HDR, so bloom picks it up; AO, dirt and light do not touch it)
 * With runtime extensions disabled, layer_count 0 and dirt_amount 0 give the PBR-lite result for the same A, R, M.
 * (ab.x, ab.y) is the split-sum environment BRDF scale and bias, Karis' analytic fit (the same as PBR-lite).
 *
 * Image-based ambient: with SuRenderFrameDesc.env set, diffuse_env is the cube's roughest mip (a GGX alpha = 1
 * prefilter, close to a cosine-weighted irradiance; the cheap documented alternative to spherical harmonics) and
 * specular_env is the cube sampled at lod = roughness * (mips - 1) from the prefiltered chain (each mip filtered with
 * GGX importance sampling at roughness = mip / (mips - 1)). Without it both come from the procedural sky exactly as
 * PBR-lite does. Both are multiplied by env_intensity (0 = 1).
 *
 * object.emissive multiplies the glow (a per-instance glow multiplier: pulse lights and engines with it); 0 = no glow. */
#define SU_PAINT_SLOT_SPECULAR 8u
#define SU_PAINT_SLOT_DETAIL_MATERIAL 9u
#define SU_PAINT_SLOT_DETAIL_NORMAL 10u
#define SU_PAINT_SLOT_DETAIL_MASK 11u
#define SU_RENDER_PAINT_SLOT_COUNT 12u
#define SU_PAINT_NORMAL_Z_ONE 1u
#define SU_PAINT_DETAIL_MATERIAL 2u
#define SU_PAINT_DETAIL_NORMAL_DARKEN 4u

typedef struct SuRenderPaintDesc {
    SuAssetMaterial factors;           /* base_color, metallic, roughness, normal_scale, occlusion_strength, emissive,
                                        * emissive_strength, alpha_mode, alpha_cutoff, double_sided; texture ids unused */
    SuAssetPaint paint;                /* texture ids unused; tex_info selects UV set and wrap modes */
    SuRenderHandle textures[SU_RENDER_PAINT_SLOT_COUNT]; /* 0 = none */
    uint32_t extra_tex_info[4];
    float specular_factor[3];
    uint32_t specular_channels;
    uint32_t flags;
    float detail_strength;
    float detail_uv_scale[2];
    float detail_rotation;
    uint32_t detail_layer_mask;
    uint32_t overlay;
} SuRenderPaintDesc;

/* ---- image-based lighting: a cubemap with prefiltered specular mips ----
 *
 * Faces are +X -X +Y -Y +Z -Z in world axes (the Vulkan / OpenGL cube convention: the texel at (x, y) of a face
 * is seen along the direction the cube-map sampling rules give). Texel data is linear HDR, 4 floats per texel
 * (alpha ignored), faces contiguous: 6 * size * size * 4 floats. size is a power of two, 8..1024. The renderer builds
 * the chain on the CPU (su_env_prefilter, deterministic) and uploads it as RGBA16F with the other resources: down
 * to 4 x 4 texels, so mips = log2(size) (size 8 gives 2 mips). A host cubemap from a nebula texture is baked to faces
 * by the host and passed here; su_render_env_from_sky bakes the procedural sky (gradient and sun glow, no stars and no
 * disc) instead. */
SU_RENDER_API uint32_t su_env_mip_count(uint32_t size);
/* Floats of the half-float chain (RGBA16F, as uint16_t) are 4 per texel: elements of out for su_env_prefilter. */
SU_RENDER_API uint64_t su_env_half_count(uint32_t size);
/* Prefilters faces (see above) into out: mip-major, then face, 4 half floats per texel. 0 on bad arguments. */
SU_RENDER_API uint32_t su_env_prefilter(uint32_t size, const float *faces, uint16_t *out);
/* Direction of the centre of texel (x, y) on a face, and the inverse: face and texel of a direction. */
SU_RENDER_API SuVec3f su_env_texel_dir(uint32_t face, uint32_t size, uint32_t x, uint32_t y);
SU_RENDER_API void su_env_dir_texel(SuVec3f dir, uint32_t size, uint32_t *face, uint32_t *x, uint32_t *y);

typedef struct SuRenderModelNode {
    float transform[16];               /* model space from mesh space, column-major (any affine, mirrored too) */
    SuRenderHandle lod_mesh[SU_ASSET_MAX_LODS];
    float lod_screen[SU_ASSET_MAX_LODS];
    uint32_t lod_count;
} SuRenderModelNode;

SU_RENDER_API uint32_t su_render_mesh_create(SuRenderer *r, const SuRenderMeshDesc *d, SuRenderHandle *out);
SU_RENDER_API uint32_t su_render_texture_create(SuRenderer *r, const SuRenderTextureDesc *d, SuRenderHandle *out);
SU_RENDER_API uint32_t su_render_material_create(SuRenderer *r, const SuRenderMaterialDesc *d, SuRenderHandle *out);
SU_RENDER_API uint32_t su_render_fx_material_create(SuRenderer *r, const SuRenderFxMaterialDesc *d, SuRenderHandle *out);
/* A layered paint material (see above). SU_E_INVALID_ARGUMENT for an unknown texture handle, a texture of the wrong
 * colour space for its slot, or invalid paint parameters (layer count, order, channels, non-finite values). */
SU_RENDER_API uint32_t su_render_paint_material_create(SuRenderer *r, const SuRenderPaintDesc *d, SuRenderHandle *out);
/* An environment cubemap with prefiltered mips (see "image-based lighting"); READY when uploaded. Release with
 * su_render_release. SU_E_UNSUPPORTED_METHOD when the device cannot filter RGBA16F cube images. */
SU_RENDER_API uint32_t su_render_env_create(SuRenderer *r, uint32_t size, const float *faces, SuRenderHandle *out);
/* GPU prefiltering: when the device can blit and render RGBA16F (nearly all), su_render_env_create (and env_from_sky,
 * env_from_dds) prefilter on the GPU instead: the same chain (box-filtered source mips, GGX importance sampling with 96
 * samples per texel and the same level selection) rendered by fragment passes in one submission that the call waits
 * for, READY on return. It matches su_env_prefilter within half-float and filtering differences (the GPU filters
 * across cube faces where the CPU clamps at face edges) and turns a 1024 cube from seconds into milliseconds.
 * SuRenderStats.env_prefilter_ms and env_prefilter_gpu report the last one. SU_ENV_CPU forces the CPU reference. */
#define SU_ENV_CPU 0x1u
SU_RENDER_API uint32_t su_render_env_create_ex(SuRenderer *r, uint32_t size, const float *faces, uint32_t flags,
                                               SuRenderHandle *out);
/* A texture straight from a parsed DDS (sutekh/dds.h): 2D, or a cube with a cube image view (6 layers). The format
 * (and so sRGB vs linear) comes from the SuDdsImage; choose it at su_dds_parse. BCn, BC6H included, is uploaded
 * natively when the device supports it; otherwise the handle is FAILED (see SuRenderDeviceInfo.bc_textures). The
 * image data must stay valid until READY unless SU_RENDER_UPLOAD_COPY. A cube can be drawn as the sky background
 * (SuRenderFrameDesc.sky_cube) without any prefiltering. */
SU_RENDER_API uint32_t su_render_texture_create_dds(SuRenderer *r, const SuDdsImage *img, uint32_t flags, SuRenderHandle *out);
/* An environment cube (as su_render_env_create) from a DDS cube: HDR (BC6H, RGBA16F) or LDR (RGBA8, BC1..BC5, BC7; read
 * as linear or sRGB by the colour space chosen at su_dds_parse). Decoded and prefiltered on the CPU as
 * su_render_env_create does (the GPU does not prefilter here). The size is the largest square power-of-two mip in
 * 8..1024; when every such mip is larger than 1024 (a 2048 cube with a single mip), the smallest larger one is decoded
 * and box-downsampled to 1024. SU_E_INVALID_ARGUMENT for a 2D file, or no square power-of-two mip of at least 8;
 * SU_E_UNSUPPORTED_METHOD for a format without a CPU decoder. Same as su_render_env_from_dds_ex(r, img, 0, out). */
SU_RENDER_API uint32_t su_render_env_from_dds(SuRenderer *r, const SuDdsImage *img, SuRenderHandle *out);
/* As su_render_env_from_dds with a size cap: max_size (a power of two in 8..1024; 0 = 1024) replaces 1024 above, so a
 * 2048 nebula can become a cheap 256 environment. Prefiltering cost grows with size squared: 256 is a good default for
 * ambient light; 1024 takes seconds. SU_E_INVALID_ARGUMENT for another max_size. */
SU_RENDER_API uint32_t su_render_env_from_dds_ex(SuRenderer *r, const SuDdsImage *img, uint32_t max_size,
                                                 SuRenderHandle *out);
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

/* The procedural sky of a lighting block (gradient along up, sun glow; ambient and exposure not applied) as cube faces
 * for su_env_prefilter (see "image-based lighting"). */
SU_RENDER_API void su_env_sky_faces(uint32_t size, const SuRenderLighting *light, float *faces);
/* su_render_env_create of su_env_sky_faces(size, light): the procedural sky as the environment. */
SU_RENDER_API uint32_t su_render_env_from_sky(SuRenderer *r, uint32_t size, const SuRenderLighting *light,
                                              SuRenderHandle *out);

typedef struct SuRenderObject {
    SuWorldPos pos;                    /* Q32.32 */
    SuQuatf rot;
    float scale;                       /* uniform, > 0 */
    float tint[4];                     /* multiplies base colour and alpha. tint[3] < 1 draws the object translucent\n                                        * whatever its material's alpha mode: in the blended pass (depth-tested, no\n                                        * depth write, back to front by node distance, after opaque and masked), with\n                                        * a masked material's cutoff not applied. tint[3] <= 0 or NaN: not drawn\n                                        * (counted in objects_culled). 1 = unchanged behaviour. */
    float emissive;                    /* multiplies emission; 1 = as authored */
    SuRenderHandle model;
    uint32_t reserved[2];
    float scale3[3];                   /* per-axis scale in the object's own frame, multiplying scale: the model is
                                        * scaled by scale * scale3 along its local x, y, z, then rotated by rot. All
                                        * zero = (1, 1, 1), uniform as before; otherwise each must be > 0. Normals use
                                        * the inverse transpose (correct shading under stretch); culling and LOD use the
                                        * largest axis scale for the bounding spheres. */
    uint32_t flags;                    /* SU_RENDER_OBJECT_*; 0 = default (casts sun shadows when they are on) */
} SuRenderObject;

#define SU_RENDER_OBJECT_NO_SHADOW 0x1u /* never casts a sun shadow (effects, tiny objects); it still receives them */

/* Directional sun shadows (optional, off by default): one shadow map fitted each frame in the sun's frame.
 *
 * Fit: the receivers are the objects that pass view culling (and, when distance > 0, the slice of the view frustum
 * from the near plane to distance, so close-ups keep detail on huge hulls); the map covers their bounding spheres
 * across the light, snapped to whole texels in world space (no shimmer while the camera moves). Casters are the
 * objects without SU_RENDER_OBJECT_NO_SHADOW, fully opaque (tint[3] >= 1), whose bounding sphere reaches into that
 * box from the sun's side, drawn at the LOD the camera selects. Opaque and masked materials cast (masked ones as
 * solid), blended ones do not; debug instances, effects and the sky neither cast nor receive.
 * Receiving: PBR-lite and paint materials multiply the sun's direct light (never the ambient or glow) by
 * 1 - strength * (1 - lit), where lit is a 3 x 3 percentage-closer filter of the map (hardware-filtered when the depth
 * format allows). Points outside the map are lit.
 * Cost: one extra depth-only pass over the casters plus nine shadow taps per lit pixel; size 2048 is 16 MiB, allocated on
 * the first frame that asks for it (changing size later waits for the device once). Cascades (SuRenderShadow.cascades)
 * keep near detail over long view distances: one depth pass and one size x size layer per cascade (4 x 2048 = 64 MiB),
 * the same nine taps per pixel in the cascade its view depth falls in. */
#define SU_SHADOW_ON 0x1u

typedef struct SuRenderShadow {
    uint32_t flags;                    /* SU_SHADOW_ON; 0 = no shadows: nothing extra is drawn, the image is as before */
    uint32_t size;                     /* texels per side, a power of two in 256..8192; 0 = 2048 */
    float distance;                    /* > 0: fit to the view frustum slice up to this distance (units) as well; 0 = fit
                                        * every visible object */
    float strength;                    /* 0..1, the fraction of the sun a full shadow removes; 0 = 1 */
    float bias;                        /* receiver depth bias in texels of the map's width; 0 = 1.5 */
    float normal_offset;               /* receiver offset along its normal in texels; 0 = 1.5 */
    uint32_t cascades;                 /* 0 or 1: the single fitted map above. 2..4: cascaded shadow maps, one size x size
                                        * layer each, splitting the view from the near plane to distance (0: to the
                                        * farthest visible receiver); each cascade is fitted by its slice's bounding
                                        * sphere and snapped to world texels, so it neither swims nor shimmers */
    float split_lambda;                /* cascade split scheme: 0..1 blend of uniform (0) and logarithmic (1) splits;
                                        * 0 = 0.75 */
} SuRenderShadow;

/* Post processing on the HDR scene (RGBA16F), before the tonemap. Zero-initialised = everything off, so existing
 * frames render exactly as before.
 *
 * Bloom: the scene is downsampled through a mip chain (13-tap filter, the first level thresholded with a soft knee
 * and Karis-weighted), then upsampled with a tent filter, and the result is added to the scene before exposure.
 * Threshold is in scene-linear units (before exposure). The chain is energy preserving: intensity 1 adds back about
 * the energy that passed the threshold, spread by the radius. Intensity 0 gives exactly the no-bloom image.
 *
 * Auto exposure is stateless and therefore deterministic: the log-average luminance of the frame (read from the
 * smallest level of the same chain) sets exposure = key / average, clamped to [exposure_min, exposure_max], times
 * light.exposure. There is no temporal adaptation (nothing is carried between frames); smooth the manual
 * light.exposure from the host if a slow adaptation is wanted. */
#define SU_POST_BLOOM 0x1u              /* run the bloom chain */
#define SU_POST_AUTO_EXPOSURE 0x2u      /* exposure from the frame's average luminance */

typedef struct SuRenderPost {
    uint32_t flags;                    /* SU_POST_* */
    float bloom_threshold;             /* scene-linear brightness (max of r, g, b) where bloom starts; 0 = 1.0 */
    float bloom_knee;                  /* soft knee width as a fraction of the threshold, 0..1 (0 = hard) */
    float bloom_intensity;             /* multiplier of the bloom added back; 0 = no bloom contribution (not "default") */
    float bloom_radius;                /* tent filter tap spacing in texels of each level; 0 = 1.0, larger = wider glow */
    uint32_t bloom_levels;             /* mip levels used for the glow, 2..8; 0 = 6 (fewer on small targets) */
    float auto_key;                    /* target average luminance; 0 = 0.18 */
    float exposure_min;                /* auto exposure clamp; 0 = 1/32 */
    float exposure_max;                /* 0 = 32 */
} SuRenderPost;

#define SU_HAZE_ELLIPSOID 0u
#define SU_HAZE_BOX 1u
#define SU_HAZE_ADDITIVE 0u
#define SU_HAZE_OVER 1u
#define SU_HAZE_SHADOW 1u
#define SU_HAZE_SCATTER 0u
#define SU_HAZE_CHORD 1u
#define SU_HAZE_MAX_VOLUMES 1024u

typedef struct SuHazeVolume {
    SuWorldPos pos;
    SuMat4f transform;
    float color[3];
    float density;
    float falloff;
    float sun;
    uint32_t shape;
    uint32_t blend;
    uint32_t flags;
    uint32_t response;
    float chord_power;
    float source_brightness;
    float source_size;
} SuHazeVolume;

SU_RENDER_API float su_haze_optical_depth(const SuHazeVolume *volume, SuWorldPos eye, SuVec3f direction,
                                         float max_distance);

typedef struct SuRenderFlame {
    SuMat4f transform;
    float colour[4];
    float frequency, amplitude_start, amplitude_end, phase;
    float gain, warp, scale, soft_distance;
    float atlas_rows, atlas_index, steps, time;
    float noise_velocity[3], reserved;
    SuRenderHandle shape_atlas, gradient, noise_volume;
} SuRenderFlame;

typedef struct SuRenderFrameDesc {
    uint32_t struct_size;            /* sizeof(SuRenderFrameDesc) */
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
    SuRenderPost post;                 /* all zero: no bloom, no auto exposure (the plain tonemap of before) */
    SuRenderHandle env;                /* 0: paint materials take ambient from the procedural sky. Else an env cubemap
                                        * (su_render_env_create); a handle that is not READY counts as 0 */
    float env_intensity;               /* multiplies the env's light; 0 = 1 */
    const SuFxVertex *fx_tex;          /* textured effects (su_fx_sprite, su_fx_sprite_axis, su_trail_build_tex) */
    uint32_t fx_tex_vertex_count;
    const SuFxBatch *fx_batches;       /* ranges of fx_tex and their textures, drawn in this order */
    uint32_t fx_batch_count;
    SuRenderHandle sky_cube;           /* 0: the procedural sky (or the clear colour). Else a READY cube (su_render_env_create,
                                        * su_render_env_from_dds, or a cube texture) drawn as the background instead of
                                        * the procedural sky: radiance = cube(R^-1 * d) * sky_intensity. No sun disc or
                                        * stars are added; light.no_sky still turns the background off. Not READY: the
                                        * procedural sky is drawn. */
    float sky_intensity;               /* multiplies the cube background; 0 = 1 */
    SuQuatf sky_rot;                   /* orientation of the cube in the world (all zero = identity); rotate around
                                        * light.up with su_quatf_axis_angle */
    float sky_lod;                     /* mip level sampled (0 = sharpest); a prefiltered env cube gets blurrier with it */
    SuRenderShadow shadow;             /* all zero: no sun shadows (see SuRenderShadow) */
    float time_seconds;
    const SuHazeVolume *haze;
    uint32_t haze_count;
    const SuRenderFlame *flames;
    uint32_t flame_count;
    SuRenderHandle distortion_cube;
    float distortion_cube_intensity;
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
    /* ---- post processing ---- */
    uint32_t bloom_levels_active;  /* levels of the last frame's bloom chain, 0 when the chain did not run */
    uint32_t post_passes;          /* bloom render passes of the last frame (downsample + upsample) */
    /* ---- textured effects ---- */
    uint32_t fx_tex_vertices;      /* drawn */
    uint32_t fx_batches_drawn;
    uint32_t fx_batches_skipped;   /* texture not READY, not 2D, or bad range */
    uint32_t fx_quads_sorted;      /* quads of SU_FX_BATCH_SORT batches sorted in the last frame */
    uint32_t fx_sort_draws;        /* draw calls those sorted quads took (one per run of quads of the same batch) */
    /* ---- sun shadows ---- */
    uint32_t shadow_size;          /* texels per side of the shadow map used by the last frame, 0 when off */
    uint32_t shadow_casters;       /* caster instances (node x submesh) drawn into it */
    uint32_t shadow_draws;         /* draw calls of the shadow pass */
    float shadow_texel;            /* world units per shadow texel in the last frame (the first cascade's) */
    /* ---- environment prefiltering ---- */
    double env_prefilter_ms;       /* wall time of the last environment prefilter (upload included on the GPU path) */
    uint32_t env_prefilter_gpu;    /* 1: it ran on the GPU; 0: on the CPU */
    /* ---- sun shadow cascades ---- */
    uint32_t shadow_cascades;      /* cascades of the last frame (1 for the single map, 0 when off) */
    float shadow_cascade_texel[4]; /* world units per texel of each cascade */
    uint32_t haze_volumes_drawn;
    uint32_t flames_drawn, flames_skipped, flame_draws;
    uint32_t distortion_draws;
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
