/* Internal state of the Vulkan backend of sutekh/render.h, shared by su_render_vk.c (device, targets, passes,
 * pipelines, frame orchestration), su_render_res.c (resources, uploads, retirement) and su_render_scene.c
 * (camera-relative scene building). Not a public header. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_RENDER_VK_INT_H
#define SU_RENDER_VK_INT_H

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR 1
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c17 */
#endif
#include <dlfcn.h>
#include <time.h>
#endif
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/render.h"

#define FRAMES_IN_FLIGHT 2u
#define MAX_SWAP_IMAGES 8u
#define DEFAULT_INSTANCES 16384u
#define DEFAULT_LINE_VERTICES 131072u
#define DEFAULT_UPLOAD_BUDGET (16u << 20)
#define DEFAULT_RESOURCES 16384u
#define DEFAULT_DRAW_INSTANCES 65536u
#define DEFAULT_FX_VERTICES 196608u
#define DEFAULT_HUD_VERTICES 49152u
#define MAX_DEVICES 16u
#define HDR_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

/* ---------- entry points ---------- */

#define VK_GLOBAL_FNS(X) \
    X(vkCreateInstance)  \
    X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FNS(X)                          \
    X(vkDestroyInstance)                            \
    X(vkEnumeratePhysicalDevices)                   \
    X(vkGetPhysicalDeviceProperties)                \
    X(vkGetPhysicalDeviceFeatures)                  \
    X(vkGetPhysicalDeviceQueueFamilyProperties)     \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceFormatProperties)          \
    X(vkCreateDevice)                               \
    X(vkGetDeviceProcAddr)                          \
    X(vkEnumerateDeviceExtensionProperties)         \
    X(vkDestroySurfaceKHR)                          \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)         \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)    \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)         \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)    \
    X(vkCreateDebugUtilsMessengerEXT)               \
    X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FNS(X)               \
    X(vkDestroyDevice)                 \
    X(vkGetDeviceQueue)                \
    X(vkDeviceWaitIdle)                \
    X(vkCreateSwapchainKHR)            \
    X(vkDestroySwapchainKHR)           \
    X(vkGetSwapchainImagesKHR)         \
    X(vkAcquireNextImageKHR)           \
    X(vkQueuePresentKHR)               \
    X(vkCreateImageView)               \
    X(vkDestroyImageView)              \
    X(vkCreateImage)                   \
    X(vkDestroyImage)                  \
    X(vkGetImageMemoryRequirements)    \
    X(vkBindImageMemory)               \
    X(vkCreateBuffer)                  \
    X(vkDestroyBuffer)                 \
    X(vkGetBufferMemoryRequirements)   \
    X(vkBindBufferMemory)              \
    X(vkAllocateMemory)                \
    X(vkFreeMemory)                    \
    X(vkMapMemory)                     \
    X(vkUnmapMemory)                   \
    X(vkCreateRenderPass)              \
    X(vkDestroyRenderPass)             \
    X(vkCreateFramebuffer)             \
    X(vkDestroyFramebuffer)            \
    X(vkCreateShaderModule)            \
    X(vkDestroyShaderModule)           \
    X(vkCreatePipelineLayout)          \
    X(vkDestroyPipelineLayout)         \
    X(vkCreateGraphicsPipelines)       \
    X(vkDestroyPipeline)               \
    X(vkCreatePipelineCache)           \
    X(vkDestroyPipelineCache)          \
    X(vkGetPipelineCacheData)          \
    X(vkCreateDescriptorSetLayout)     \
    X(vkDestroyDescriptorSetLayout)    \
    X(vkCreateDescriptorPool)          \
    X(vkDestroyDescriptorPool)         \
    X(vkAllocateDescriptorSets)        \
    X(vkFreeDescriptorSets)            \
    X(vkResetDescriptorPool)           \
    X(vkUpdateDescriptorSets)          \
    X(vkCreateSampler)                 \
    X(vkDestroySampler)                \
    X(vkCreateCommandPool)             \
    X(vkDestroyCommandPool)            \
    X(vkAllocateCommandBuffers)        \
    X(vkResetCommandBuffer)            \
    X(vkBeginCommandBuffer)            \
    X(vkEndCommandBuffer)              \
    X(vkCmdBeginRenderPass)            \
    X(vkCmdEndRenderPass)              \
    X(vkCmdBindPipeline)               \
    X(vkCmdBindVertexBuffers)          \
    X(vkCmdBindIndexBuffer)            \
    X(vkCmdBindDescriptorSets)         \
    X(vkCmdDrawIndexed)                \
    X(vkCmdDraw)                       \
    X(vkCmdPushConstants)              \
    X(vkCmdSetViewport)                \
    X(vkCmdSetScissor)                 \
    X(vkCmdCopyImageToBuffer)          \
    X(vkCmdCopyBuffer)                 \
    X(vkCmdCopyBufferToImage)          \
    X(vkCmdPipelineBarrier)            \
    X(vkCmdResetQueryPool)             \
    X(vkCmdWriteTimestamp)             \
    X(vkCreateQueryPool)               \
    X(vkDestroyQueryPool)              \
    X(vkGetQueryPoolResults)           \
    X(vkCreateFence)                   \
    X(vkDestroyFence)                  \
    X(vkWaitForFences)                 \
    X(vkResetFences)                   \
    X(vkCreateSemaphore)               \
    X(vkDestroySemaphore)              \
    X(vkQueueSubmit)                   \
    X(vkQueueWaitIdle)

typedef struct VkFns {
    void *lib;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
#define X(name) PFN_##name name;
    VK_GLOBAL_FNS(X)
    VK_INSTANCE_FNS(X)
    VK_DEVICE_FNS(X)
#if defined(_WIN32)
    X(vkCreateWin32SurfaceKHR)
#endif
#undef X
} VkFns;

typedef struct Validation {
    uint32_t enabled;
    uint32_t errors;
    uint32_t warnings;
    uint32_t printed;
} Validation;

typedef struct Buffer {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} Buffer;

typedef struct Frame {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    VkQueryPool queries;
    Buffer instances;     /* debug instances */
    Buffer lines;
    Buffer draw_inst;     /* object draw instances (DrawInstance) */
    Buffer fx;
    Buffer hud;
    Buffer ubo;           /* FrameUbo */
    Buffer staging;       /* upload_budget bytes */
    VkDescriptorSet frame_set;
    uint64_t serial;      /* serial of the submission that used this slot last */
    int submitted;
    int timed;
} Frame;

/* ---------- resources ---------- */

#define RES_NONE 0u
#define RES_MESH 1u
#define RES_TEXTURE 2u
#define RES_MATERIAL 3u
#define RES_MODEL 4u

/* Internal texture format of environment cubes: RGBA16F, 8 bytes per texel, six array layers. */
#define TEXFMT_ENV 0x100u

typedef struct SubGpu {
    uint32_t first_index;
    uint32_t index_count;
    SuRenderHandle material;
    float center[3];
    float radius;
} SubGpu;

typedef struct MeshRes {
    Buffer vb;
    Buffer ib;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t sub_count;
    SubGpu *subs;
    float center[3];
    float radius;
    const uint8_t *src_v;
    const uint8_t *src_i;
    uint8_t *owned;           /* SU_RENDER_UPLOAD_COPY */
    uint64_t done;            /* bytes uploaded (vertices then indices) */
} MeshRes;

typedef struct TexRes {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat vkfmt;
    uint32_t format;          /* SU_TEX_* */
    uint32_t width, height, mips;
    const uint8_t *src[SU_ASSET_MAX_MIPS];
    uint8_t *owned;
    uint32_t layers;          /* array layers: 1, or 6 for an environment cube */
    uint32_t layer;           /* next layer of the current mip */
    uint32_t cube_mips;       /* environment: mips of the chain (for the sampling lod) */
    VkDescriptorSet env_set;  /* environment: set 2 of the paint layout */
    uint32_t mip;             /* next mip to upload */
    uint32_t row;             /* next block row of that mip */
    int transitioned;         /* UNDEFINED -> TRANSFER_DST recorded */
} TexRes;

/* The per-material uniform block of a paint material (std140; paint.frag, block Paint). */
typedef struct PaintUbo {
    float a[4];               /* specular, dirt amount, dirt darken, dirt roughen */
    uint32_t b[4];            /* layer count, order bytes, channels (rough | metal << 8 | specular << 16 | dirt << 24), ao */
    struct {
        float color_rough[4];
        float mop[4];         /* metallic, opacity, flags, glow strength */
        float glow[4];
    } layer[4];
} PaintUbo;

typedef struct MatRes {
    SuAssetMaterial f;
    uint32_t kind;            /* 0: PBR-lite, 1: layered paint */
    SuAssetPaint paint;       /* kind 1 */
    Buffer ubo;               /* kind 1: PaintUbo, host visible, written once */
    SuRenderHandle tex[SU_PAINT_SLOT_COUNT];
    VkDescriptorSet set;
    uint32_t bound_ready;     /* slots whose real texture is in the current set */
    uint32_t pipeline;        /* PIPE_* of its draws */
} MatRes;

typedef struct ModelRes {
    SuRenderModelNode *nodes;
    uint32_t count;
    float center[3];
    float radius;
    float *node_center;       /* 4 floats per node: model-space sphere of LOD 0 */
} ModelRes;

typedef struct Res {
    uint32_t gen;             /* bumped on release; handles carry it */
    uint8_t type;             /* RES_* (RES_NONE: free slot) */
    uint8_t state;            /* SU_RENDER_STATE_* */
    uint8_t queued;           /* in the upload queue */
    uint8_t internal;         /* created by the renderer (defaults, font); not counted, never released by users */
    union {
        MeshRes mesh;
        TexRes tex;
        MatRes mat;
        ModelRes model;
    } u;
} Res;

/* GPU objects of a released resource, destroyed once the frames that could use them have completed. */
typedef struct Retired {
    uint64_t serial;
    Buffer a;
    Buffer b;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet set;
} Retired;

/* Pipelines of the HDR scene pass and the output pass. */
#define PIPE_OPAQUE 0u
#define PIPE_OPAQUE_2S 1u
#define PIPE_MASK 2u
#define PIPE_MASK_2S 3u
#define PIPE_BLEND 4u
#define PIPE_BLEND_2S 5u
#define PIPE_PBR_COUNT 6u

typedef struct DrawInstance {
    float r0[4];
    float r1[4];
    float r2[4];
    float tint[4];
    float extra[4];           /* x: emissive multiplier */
} DrawInstance;

typedef struct FrameUbo {
    float view_proj[16];
    float inv_view_proj[16];
    float sun_dir[4];
    float sun_color[4];
    float sky_zenith[4];
    float sky_horizon[4];
    float sky_nadir[4];
    float up[4];
    float params[4];
} FrameUbo;

typedef struct MatPush {
    float base_color[4];
    float emissive[4];
    float mr[4];
    float misc[4];
    uint32_t uvset[4];
} MatPush;

typedef struct LegacyPush {
    float view_proj[16];
    float light_dir_ambient[4];
} LegacyPush;

typedef struct PostPush {
    float p[4];
} PostPush;

/* Bloom passes and the bloom output pass (see shaders/bloom_*.frag, post_bloom.frag). */
typedef struct BloomPush {
    float a[4];
    float b[4];
} BloomPush;

#define BLOOM_MAX_LEVELS 12u

/* One draw item of the scene: an instance of a submesh with a material. */
typedef struct DrawItem {
    uint64_t key;
    uint32_t mesh;            /* resource index */
    uint32_t sub;
    uint32_t mat;             /* resource index, or UINT32_MAX for the default material */
    uint32_t pipe;
    uint32_t kind;            /* 0: PBR-lite, 1: layered paint (its own pipelines and layout) */
    float dist2;              /* blend sorting */
    uint32_t forced;          /* 1: opaque or masked material drawn in the blend pass because the instance tint is translucent */
    DrawInstance inst;
} DrawItem;

struct SuRenderer {
    VkFns f;
    Validation validation;
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice pd;
    SuRenderDeviceInfo info;
    VkPhysicalDeviceMemoryProperties memprops;
    VkPhysicalDeviceProperties props;
    int anisotropy;
    int bc;
    uint32_t family;
    VkDevice dev;
    VkQueue queue;
    VkFormat color_format;
    VkFormat depth_format;
    VkRenderPass scene_pass;
    VkRenderPass post_pass;
    VkPipelineCache cache;
    VkDescriptorSetLayout frame_dsl;
    VkDescriptorSetLayout mat_dsl;
    VkDescriptorSetLayout tex_dsl;      /* one sampler: post input, HUD font */
    VkDescriptorPool pool_frame;
    VkDescriptorPool pool_mat;
    VkPipelineLayout scene_layout;
    VkPipelineLayout post_layout;
    VkPipeline mesh_pipe;
    VkPipeline line_pipe;
    VkPipeline sky_pipe;
    VkPipeline fx_pipe;
    VkPipeline pbr_pipe[PIPE_PBR_COUNT];
    VkPipeline paint_pipe[PIPE_PBR_COUNT];
    VkDescriptorSetLayout paint_dsl;    /* eight samplers and the PaintUbo */
    VkDescriptorSetLayout env_dsl;      /* one cube sampler */
    VkPipelineLayout paint_layout;      /* frame, paint material, environment */
    VkDescriptorSet frame_env_set;      /* the environment bound for this frame (built-in black cube when none) */
    SuRenderHandle black_tex;
    SuRenderHandle default_env;
    VkPipeline post_pipe;
    VkPipeline hud_pipe;
    VkSampler samplers[9];              /* linear, mipmapped, by wrap S * 3 + wrap T */
    VkSampler nearest;
    VkDescriptorSet post_set;
    VkDescriptorSet font_set;
    VkCommandPool pool;
    Buffer mesh_vb;
    Buffer mesh_ib;
    uint32_t mesh_first_index[SU_MESH_COUNT];
    uint32_t mesh_index_count[SU_MESH_COUNT];
    int32_t mesh_vertex_offset[SU_MESH_COUNT];
    Frame frames[FRAMES_IN_FLIGHT];
    uint32_t frame_slot;
    uint64_t serial;                    /* frames submitted */
    uint64_t completed;                 /* highest serial known complete */
    /* targets */
    int offscreen;
    void *hwnd;
    uint32_t width, height;
    uint32_t want_width, want_height;
    int needs_rebuild;
    uint32_t flags;
    VkSwapchainKHR swapchain;
    VkPresentModeKHR present_mode;
    uint32_t image_count;
    VkImage images[MAX_SWAP_IMAGES];
    VkImageView views[MAX_SWAP_IMAGES];
    VkFramebuffer fbs[MAX_SWAP_IMAGES];
    VkSemaphore rendered[MAX_SWAP_IMAGES];
    VkImage color_img;                  /* offscreen output */
    VkDeviceMemory color_mem;
    VkImageView color_view;
    VkImage hdr_img;
    VkDeviceMemory hdr_mem;
    VkImageView hdr_view;
    VkFramebuffer hdr_fb;
    /* bloom (half resolution): chain A = downsampled levels (alpha: log2 luma), chain B = upsampled levels */
    VkRenderPass bloom_pass;
    VkDescriptorSetLayout tex3_dsl;     /* three samplers: upsample and bloom output */
    VkPipelineLayout bloom_layout;      /* tex_dsl + BloomPush */
    VkPipelineLayout bloom3_layout;     /* tex3_dsl + BloomPush */
    VkPipeline bloom_down_pipe;
    VkPipeline bloom_up_pipe;
    VkPipeline post_bloom_pipe;
    VkDescriptorPool pool_bloom;
    VkSampler bloom_sampler;            /* linear, clamp, no mips */
    int bloom_ok;                       /* device supports it and the targets exist */
    uint32_t bloom_count;               /* levels of chain A for the current size */
    VkImage bl_img[2];
    VkDeviceMemory bl_mem[2];
    VkImageView bl_view[2][BLOOM_MAX_LEVELS];
    VkFramebuffer bl_fb[2][BLOOM_MAX_LEVELS];
    VkDescriptorSet bl_down_set[BLOOM_MAX_LEVELS];
    VkDescriptorSet bl_up_a[BLOOM_MAX_LEVELS];  /* low = A[i + 1], hi = A[i] */
    VkDescriptorSet bl_up_b[BLOOM_MAX_LEVELS];  /* low = B[i + 1], hi = A[i] */
    VkDescriptorSet bl_final_set;
    VkImage depth_img;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;
    Buffer readback;
    /* capacities */
    uint32_t max_instances;
    uint32_t max_lines;
    uint32_t max_draw;
    uint32_t max_fx;
    uint32_t max_hud;
    uint32_t budget;
    SuRenderInstance *sorted;
    DrawItem *items;
    uint32_t *order;
    /* resources */
    Res *res;
    uint32_t res_cap;
    uint32_t *free_list;
    uint32_t free_count;
    void *upload_q;                     /* ring of (resource index, generation) */
    uint32_t q_head;
    uint32_t q_count;
    Retired *retired;
    uint32_t retired_count;
    uint32_t retired_cap;
    uint32_t tex_ready_this_frame;      /* a texture became READY: re-check materials */
    SuRenderHandle white_tex;
    SuRenderHandle normal_tex;
    SuRenderHandle font_tex;
    uint8_t *font_pixels;
    VkDescriptorSet default_mat_set;
    char cache_path[1024];
    SuRenderStats stats;
    char last_error[256];
};

/* su_render_vk.c */
uint32_t su_rvk_fail(SuRenderer *r, uint32_t code, const char *fmt, ...);
int su_rvk_find_memory(SuRenderer *r, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *type);
int su_rvk_make_buffer(SuRenderer *r, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buffer *b);
void su_rvk_free_buffer(SuRenderer *r, Buffer *b);
double su_rvk_now_ms(void);

/* su_render_res.c */
uint32_t su_rvk_res_init(SuRenderer *r, uint32_t cap);
void su_rvk_res_shutdown(SuRenderer *r);
/* Records queued uploads into cmd (before any render pass) within the budget of the slot's staging buffer. */
void su_rvk_record_uploads(SuRenderer *r, Frame *fr, VkCommandBuffer cmd);
/* Destroys retired objects whose serial is complete. */
void su_rvk_collect_retired(SuRenderer *r);
/* Refreshes material descriptor sets whose textures became ready. */
void su_rvk_refresh_materials(SuRenderer *r);
Res *su_rvk_get(const SuRenderer *r, SuRenderHandle h, uint32_t type, uint32_t *index);
VkDescriptorSet su_rvk_material_set(SuRenderer *r, uint32_t mat_index);
/* The environment cube set for a frame: the READY env handle (intensity_out = intensity, 1 when 0), else the built-in
 * black cube with intensity_out 0. lod_out = the last mip of the chain. */
VkDescriptorSet su_rvk_env_set(SuRenderer *r, SuRenderHandle env, float intensity, float *lod_out, float *intensity_out);
uint32_t su_rvk_internal_textures(SuRenderer *r);

/* su_render_scene.c */
/* Culls, selects LODs and batches objects into fr->draw_inst; records the PBR draws (pass: 0 opaque and mask,
 * 1 blend) into cmd inside the scene pass. */
void su_rvk_scene_build(SuRenderer *r, Frame *fr, const SuRenderFrameDesc *f, const SuMat4f *view_proj, float proj_y,
                        uint32_t *item_count);
void su_rvk_scene_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t item_count, int blend_pass);

#endif
