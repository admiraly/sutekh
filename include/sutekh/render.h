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

#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_RENDER_API
#define SU_RENDER_API_VERSION 1u

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
    uint32_t struct_size;          /* sizeof(SuRenderDesc) */
    uint32_t flags;                /* SU_RENDER_FLAG_* */
    void *hwnd;                    /* Windows HWND to present to, or NULL for an offscreen target */
    uint32_t width, height;        /* offscreen size, or the initial window size */
    uint32_t max_instances;        /* per frame; 0 = 16384 */
    uint32_t max_line_vertices;    /* per frame; 0 = 131072 */
    int32_t device_index;          /* index from su_render_enumerate, or -1 for the best device */
} SuRenderDesc;

/* Creates the renderer. SU_E_UNSUPPORTED_METHOD without a usable Vulkan device (message says why), SU_E_IO for
 * a surface or swapchain the platform refuses, SU_E_RESOURCE_LIMIT for out-of-memory. */
SU_RENDER_API uint32_t su_render_create(const SuRenderDesc *desc, SuRenderer **out, char message[256]);
SU_RENDER_API void su_render_destroy(SuRenderer *r);
SU_RENDER_API uint32_t su_render_device(const SuRenderer *r, SuRenderDeviceInfo *out);
/* Window resized (also detected from the swapchain). A zero size pauses presentation. */
SU_RENDER_API void su_render_resize(SuRenderer *r, uint32_t width, uint32_t height);

typedef struct SuRenderStats {
    uint64_t frame;                /* frames submitted */
    double cpu_ms;                 /* CPU time of the last su_render_frame call, excluding wait_ms and present_ms */
    double gpu_ms;                 /* GPU time of the most recent completed frame, or -1 when unavailable */
    double wait_ms;                /* time su_render_frame waited for a free frame slot or an image */
    double present_ms;             /* time spent in vkQueuePresentKHR (FIFO may block here) */
    uint32_t draws;                /* draw calls of the last frame */
    uint32_t instances;            /* instances drawn */
    uint32_t line_vertices;        /* line vertices drawn */
    uint32_t dropped;              /* instances or line vertices beyond the capacity, not drawn */
    uint32_t swapchain_recreations;
    uint32_t validation_enabled;   /* 1 when VK_LAYER_KHRONOS_validation was requested and found */
    uint32_t validation_errors;    /* messages of error severity from the validation layer (0 when not enabled) */
    uint32_t validation_warnings;  /* messages of warning severity */
    uint32_t present_mode;         /* VkPresentModeKHR in use (window), or UINT32_MAX offscreen */
    uint32_t width, height;
} SuRenderStats;

/* Draws one frame: instances (any order; sorted by mesh internally) and line-list vertices, then presents
 * (window) or finishes the offscreen image. Returns SU_OK, or SU_E_RESOURCE_LIMIT when the window is minimized
 * (nothing drawn), or another status with the reason in the stats-independent last error (su_render_last_error). */
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
