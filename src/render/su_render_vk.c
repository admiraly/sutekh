/* Vulkan backend of sutekh/render.h. The Vulkan loader is opened at run time (vulkan-1.dll / libvulkan.so.1) and
 * every entry point is fetched through vkGetInstanceProcAddr / vkGetDeviceProcAddr, so the library has no
 * link-time Vulkan dependency and fails with a reason when Vulkan is missing. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/su_render_shaders.h"
#include "sutekh/render.h"

#define FRAMES_IN_FLIGHT 2u
#define MAX_SWAP_IMAGES 8u
#define DEFAULT_INSTANCES 16384u
#define DEFAULT_LINE_VERTICES 131072u
#define MAX_DEVICES 16u

/* ---------- entry points ---------- */

#define VK_GLOBAL_FNS(X) \
    X(vkCreateInstance)  \
    X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FNS(X)                                                                                        \
    X(vkDestroyInstance)                                                                                          \
    X(vkEnumeratePhysicalDevices)                                                                                 \
    X(vkGetPhysicalDeviceProperties)                                                                              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                   \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                        \
    X(vkGetPhysicalDeviceFormatProperties)                                                                        \
    X(vkCreateDevice)                                                                                             \
    X(vkGetDeviceProcAddr)                                                                                        \
    X(vkEnumerateDeviceExtensionProperties)                                                                       \
    X(vkDestroySurfaceKHR)                                                                                        \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                                  \
    X(vkCreateDebugUtilsMessengerEXT)                                                                             \
    X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FNS(X)                                                                                          \
    X(vkDestroyDevice)                                                                                            \
    X(vkGetDeviceQueue)                                                                                           \
    X(vkDeviceWaitIdle)                                                                                           \
    X(vkCreateSwapchainKHR)                                                                                       \
    X(vkDestroySwapchainKHR)                                                                                      \
    X(vkGetSwapchainImagesKHR)                                                                                    \
    X(vkAcquireNextImageKHR)                                                                                      \
    X(vkQueuePresentKHR)                                                                                          \
    X(vkCreateImageView)                                                                                          \
    X(vkDestroyImageView)                                                                                         \
    X(vkCreateImage)                                                                                              \
    X(vkDestroyImage)                                                                                             \
    X(vkGetImageMemoryRequirements)                                                                               \
    X(vkBindImageMemory)                                                                                          \
    X(vkCreateBuffer)                                                                                             \
    X(vkDestroyBuffer)                                                                                            \
    X(vkGetBufferMemoryRequirements)                                                                              \
    X(vkBindBufferMemory)                                                                                         \
    X(vkAllocateMemory)                                                                                           \
    X(vkFreeMemory)                                                                                               \
    X(vkMapMemory)                                                                                                \
    X(vkUnmapMemory)                                                                                              \
    X(vkCreateRenderPass)                                                                                         \
    X(vkDestroyRenderPass)                                                                                        \
    X(vkCreateFramebuffer)                                                                                        \
    X(vkDestroyFramebuffer)                                                                                       \
    X(vkCreateShaderModule)                                                                                       \
    X(vkDestroyShaderModule)                                                                                      \
    X(vkCreatePipelineLayout)                                                                                     \
    X(vkDestroyPipelineLayout)                                                                                    \
    X(vkCreateGraphicsPipelines)                                                                                  \
    X(vkDestroyPipeline)                                                                                          \
    X(vkCreateCommandPool)                                                                                        \
    X(vkDestroyCommandPool)                                                                                       \
    X(vkAllocateCommandBuffers)                                                                                   \
    X(vkResetCommandBuffer)                                                                                       \
    X(vkBeginCommandBuffer)                                                                                       \
    X(vkEndCommandBuffer)                                                                                         \
    X(vkCmdBeginRenderPass)                                                                                       \
    X(vkCmdEndRenderPass)                                                                                         \
    X(vkCmdBindPipeline)                                                                                          \
    X(vkCmdBindVertexBuffers)                                                                                     \
    X(vkCmdBindIndexBuffer)                                                                                       \
    X(vkCmdDrawIndexed)                                                                                           \
    X(vkCmdDraw)                                                                                                  \
    X(vkCmdPushConstants)                                                                                         \
    X(vkCmdSetViewport)                                                                                           \
    X(vkCmdSetScissor)                                                                                            \
    X(vkCmdCopyImageToBuffer)                                                                                     \
    X(vkCmdCopyBuffer)                                                                                            \
    X(vkCmdPipelineBarrier)                                                                                       \
    X(vkCmdResetQueryPool)                                                                                        \
    X(vkCmdWriteTimestamp)                                                                                        \
    X(vkCreateQueryPool)                                                                                          \
    X(vkDestroyQueryPool)                                                                                         \
    X(vkGetQueryPoolResults)                                                                                      \
    X(vkCreateFence)                                                                                              \
    X(vkDestroyFence)                                                                                             \
    X(vkWaitForFences)                                                                                            \
    X(vkResetFences)                                                                                              \
    X(vkCreateSemaphore)                                                                                          \
    X(vkDestroySemaphore)                                                                                         \
    X(vkQueueSubmit)                                                                                              \
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

static void set_msg(char *msg, const char *fmt, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(msg, 256, fmt, ap);
        va_end(ap);
    }
}

static int load_loader(VkFns *f, char *msg) {
    memset(f, 0, sizeof *f);
#if defined(_WIN32)
    f->lib = (void *)LoadLibraryW(L"vulkan-1.dll");
    if (f->lib != NULL) {
        f->vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress((HMODULE)f->lib, "vkGetInstanceProcAddr");
    }
#else
    f->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (f->lib != NULL) {
        *(void **)&f->vkGetInstanceProcAddr = dlsym(f->lib, "vkGetInstanceProcAddr");
    }
#endif
    if (f->vkGetInstanceProcAddr == NULL) {
        set_msg(msg, "no Vulkan loader on this system (vulkan-1.dll / libvulkan.so.1 not found)");
        return 0;
    }
#define X(name) f->name = (PFN_##name)f->vkGetInstanceProcAddr(NULL, #name);
    VK_GLOBAL_FNS(X)
#undef X
    f->vkEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)f->vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (f->vkCreateInstance == NULL) {
        set_msg(msg, "the Vulkan loader does not provide vkCreateInstance");
        return 0;
    }
    return 1;
}

static void unload_loader(VkFns *f) {
    if (f->lib != NULL) {
#if defined(_WIN32)
        FreeLibrary((HMODULE)f->lib);
#else
        dlclose(f->lib);
#endif
    }
    f->lib = NULL;
}

static void load_instance_fns(VkFns *f, VkInstance inst) {
#define X(name) f->name = (PFN_##name)f->vkGetInstanceProcAddr(inst, #name);
    VK_INSTANCE_FNS(X)
#if defined(_WIN32)
    X(vkCreateWin32SurfaceKHR)
#endif
#undef X
}

/* Swapchain entry points exist only on devices created with VK_KHR_swapchain (not offscreen). */
static int is_swapchain_fn(const char *name) {
    return strstr(name, "Swapchain") != NULL || strstr(name, "NextImageKHR") != NULL ||
           strstr(name, "PresentKHR") != NULL;
}

/* Returns NULL when complete, else the name of the first missing entry point. */
static const char *load_device_fns(VkFns *f, VkDevice dev, int swapchain) {
    const char *missing = NULL;
#define X(name)                                                    \
    f->name = (PFN_##name)f->vkGetDeviceProcAddr(dev, #name);     \
    if (f->name == NULL && missing == NULL && (swapchain || !is_swapchain_fn(#name))) { \
        missing = #name;                                           \
    }
    VK_DEVICE_FNS(X)
#undef X
    return missing;
}

static double now_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

/* ---------- instance ---------- */

static int has_layer(VkFns *f, const char *name) {
    VkLayerProperties props[64];
    uint32_t n = 64;
    uint32_t i;
    if (f->vkEnumerateInstanceLayerProperties(&n, props) < 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(props[i].layerName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int has_instance_ext(VkFns *f, const char *name) {
    VkExtensionProperties props[128];
    uint32_t n = 128;
    uint32_t i;
    if (f->vkEnumerateInstanceExtensionProperties(NULL, &n, props) < 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(props[i].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct Validation {
    uint32_t enabled;
    uint32_t errors;
    uint32_t warnings;
    uint32_t printed;
} Validation;

/* Counts error and warning messages; the first 8 of either are printed to stderr. */
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    Validation *v = user;
    int error = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    (void)type;
    if (error) {
        v->errors++;
    } else {
        v->warnings++;
    }
    if (v->printed < 8u) {
        v->printed++;
        fprintf(stderr, "vulkan validation %s: %s\n", error ? "error" : "warning",
                data != NULL && data->pMessage != NULL ? data->pMessage : "");
    }
    return VK_FALSE;
}

/* Creates a Vulkan 1.2 instance. surface: also enable the surface extensions. */
static int make_instance(VkFns *f, int surface, int validation, Validation *vstate, VkInstance *inst,
                         VkDebugUtilsMessengerEXT *messenger, char *msg) {
    const char *exts[4];
    const char *layers[1];
    uint32_t next = 0;
    uint32_t nlay = 0;
    uint32_t version = VK_API_VERSION_1_0;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkResult vr;
    int debug = 0;

    if (f->vkEnumerateInstanceVersion != NULL) {
        (void)f->vkEnumerateInstanceVersion(&version);
    }
    if (version < VK_API_VERSION_1_2) {
        set_msg(msg, "the Vulkan loader supports instance version %u.%u; 1.2 is required", VK_API_VERSION_MAJOR(version),
                VK_API_VERSION_MINOR(version));
        return 0;
    }
    if (surface) {
        if (!has_instance_ext(f, VK_KHR_SURFACE_EXTENSION_NAME)) {
            set_msg(msg, "VK_KHR_surface is not available");
            return 0;
        }
        exts[next++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(_WIN32)
        if (!has_instance_ext(f, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
            set_msg(msg, "VK_KHR_win32_surface is not available");
            return 0;
        }
        exts[next++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif
    }
    if (validation && has_layer(f, "VK_LAYER_KHRONOS_validation") &&
        has_instance_ext(f, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        layers[nlay++] = "VK_LAYER_KHRONOS_validation";
        exts[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        debug = 1;
    }
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "sutekh";
    app.pEngineName = "sutekh";
    app.apiVersion = VK_API_VERSION_1_2;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = next;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = nlay;
    ci.ppEnabledLayerNames = layers;
    vr = f->vkCreateInstance(&ci, NULL, inst);
    if (vr != VK_SUCCESS) {
        set_msg(msg, "vkCreateInstance failed (VkResult %d)", (int)vr);
        return 0;
    }
    load_instance_fns(f, *inst);
    *messenger = VK_NULL_HANDLE;
    if (debug && f->vkCreateDebugUtilsMessengerEXT != NULL) {
        VkDebugUtilsMessengerCreateInfoEXT di;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_cb;
        di.pUserData = vstate;
        if (f->vkCreateDebugUtilsMessengerEXT(*inst, &di, NULL, messenger) == VK_SUCCESS) {
            vstate->enabled = 1u;
        }
    }
    return 1;
}

/* ---------- device facts ---------- */

static VkFormat pick_depth(VkFns *f, VkPhysicalDevice pd) {
    static const VkFormat cands[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    size_t i;
    for (i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        VkFormatProperties fp;
        f->vkGetPhysicalDeviceFormatProperties(pd, cands[i], &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return cands[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

static int has_device_ext(VkFns *f, VkPhysicalDevice pd, const char *name) {
    VkExtensionProperties props[512];
    uint32_t n = 512;
    uint32_t i;
    if (f->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, props) < 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(props[i].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Fills info; returns the graphics queue family (that can also present to surface when given), or -1. */
static int describe(VkFns *f, VkPhysicalDevice pd, VkSurfaceKHR surface, SuRenderDeviceInfo *info) {
    VkPhysicalDeviceProperties p;
    VkPhysicalDeviceMemoryProperties mp;
    VkQueueFamilyProperties qf[32];
    VkFormatProperties fp;
    uint32_t nq = 32;
    uint32_t i;
    int family = -1;
    f->vkGetPhysicalDeviceProperties(pd, &p);
    f->vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    memset(info, 0, sizeof *info);
    snprintf(info->name, sizeof info->name, "%s", p.deviceName);
    info->vendor_id = p.vendorID;
    info->device_id = p.deviceID;
    info->api_version = p.apiVersion;
    info->driver_version = p.driverVersion;
    switch (p.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        info->type = SU_RENDER_DEVICE_INTEGRATED;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        info->type = SU_RENDER_DEVICE_DISCRETE;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        info->type = SU_RENDER_DEVICE_VIRTUAL;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        info->type = SU_RENDER_DEVICE_CPU;
        break;
    default:
        info->type = SU_RENDER_DEVICE_OTHER;
        break;
    }
    info->timestamp_period_ns = p.limits.timestampPeriod;
    for (i = 0; i < mp.memoryHeapCount; i++) {
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            mp.memoryHeaps[i].size > info->device_local_bytes) {
            info->device_local_bytes = mp.memoryHeaps[i].size;
        }
    }
    f->vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    if (nq > 32u) {
        nq = 32u;
    }
    f->vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (i = 0; i < nq && family < 0; i++) {
        VkBool32 present = VK_TRUE;
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        if (surface != VK_NULL_HANDLE &&
            (f->vkGetPhysicalDeviceSurfaceSupportKHR == NULL ||
             f->vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present) != VK_SUCCESS)) {
            present = VK_FALSE;
        }
        if (present) {
            family = (int)i;
            info->timestamps = qf[i].timestampValidBits > 0u && p.limits.timestampPeriod > 0.0f;
        }
    }
    f->vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_SRGB, &fp);
    if (p.apiVersion < VK_API_VERSION_1_2) {
        snprintf(info->why_not, sizeof info->why_not, "device supports Vulkan %u.%u; 1.2 is required",
                 VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    } else if (family < 0) {
        snprintf(info->why_not, sizeof info->why_not,
                 surface != VK_NULL_HANDLE ? "no graphics queue that can present to the window" : "no graphics queue");
    } else if (pick_depth(f, pd) == VK_FORMAT_UNDEFINED) {
        snprintf(info->why_not, sizeof info->why_not, "no depth attachment format");
    } else if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0 ||
               (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
        snprintf(info->why_not, sizeof info->why_not, "R8G8B8A8_SRGB is not a renderable, copyable format");
    } else if (surface != VK_NULL_HANDLE && !has_device_ext(f, pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        snprintf(info->why_not, sizeof info->why_not, "no VK_KHR_swapchain");
    } else {
        info->meets_baseline = 1u;
    }
    return info->meets_baseline ? family : -1;
}

uint32_t su_render_enumerate(SuRenderDeviceInfo *out, uint32_t cap, uint32_t *count, char message[256]) {
    VkFns f;
    VkInstance inst = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mess = VK_NULL_HANDLE;
    VkPhysicalDevice pds[MAX_DEVICES];
    Validation v;
    uint32_t n = MAX_DEVICES;
    uint32_t i;
    if (count == NULL || (out == NULL && cap > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *count = 0;
    memset(&v, 0, sizeof v);
    if (!load_loader(&f, message)) {
        unload_loader(&f);
        return SU_E_UNSUPPORTED_METHOD;
    }
    if (!make_instance(&f, 0, 0, &v, &inst, &mess, message)) {
        unload_loader(&f);
        return SU_E_UNSUPPORTED_METHOD;
    }
    if (f.vkEnumeratePhysicalDevices(inst, &n, pds) < 0) {
        n = 0;
    }
    for (i = 0; i < n; i++) {
        SuRenderDeviceInfo info;
        (void)describe(&f, pds[i], VK_NULL_HANDLE, &info);
        if (i < cap) {
            out[i] = info;
        }
    }
    *count = n;
    f.vkDestroyInstance(inst, NULL);
    unload_loader(&f);
    if (n == 0u) {
        set_msg(message, "the Vulkan instance reports no physical device");
    }
    return SU_OK;
}

/* ---------- renderer ---------- */

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
    Buffer instances;
    Buffer lines;
    int submitted;
    int timed;
} Frame;

struct SuRenderer {
    VkFns f;
    Validation validation;
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice pd;
    SuRenderDeviceInfo info;
    VkPhysicalDeviceMemoryProperties memprops;
    uint32_t family;
    VkDevice dev;
    VkQueue queue;
    VkFormat color_format;
    VkFormat depth_format;
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline mesh_pipe;
    VkPipeline line_pipe;
    VkCommandPool pool;
    Buffer mesh_vb;
    Buffer mesh_ib;
    uint32_t mesh_first_index[SU_MESH_COUNT];
    uint32_t mesh_index_count[SU_MESH_COUNT];
    int32_t mesh_vertex_offset[SU_MESH_COUNT];
    Frame frames[FRAMES_IN_FLIGHT];
    uint32_t frame_slot;
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
    VkImage color_img;          /* offscreen colour */
    VkDeviceMemory color_mem;
    VkImageView color_view;
    VkImage depth_img;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;
    Buffer readback;
    uint32_t max_instances;
    uint32_t max_lines;
    SuRenderInstance *sorted;
    SuRenderStats stats;
    char last_error[256];
};

static uint32_t fail(SuRenderer *r, uint32_t code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(r->last_error, sizeof r->last_error, fmt, ap);
    va_end(ap);
    return code;
}

static int find_memory(SuRenderer *r, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *type) {
    uint32_t i;
    for (i = 0; i < r->memprops.memoryTypeCount; i++) {
        if ((bits & (1u << i)) != 0 && (r->memprops.memoryTypes[i].propertyFlags & want) == want) {
            *type = i;
            return 1;
        }
    }
    return 0;
}

static int make_buffer(SuRenderer *r, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       Buffer *b) {
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    uint32_t type = 0;
    memset(b, 0, sizeof *b);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (r->f.vkCreateBuffer(r->dev, &bi, NULL, &b->buf) != VK_SUCCESS) {
        return 0;
    }
    r->f.vkGetBufferMemoryRequirements(r->dev, b->buf, &req);
    if (!find_memory(r, req.memoryTypeBits, props, &type)) {
        return 0;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (r->f.vkAllocateMemory(r->dev, &ai, NULL, &b->mem) != VK_SUCCESS ||
        r->f.vkBindBufferMemory(r->dev, b->buf, b->mem, 0) != VK_SUCCESS) {
        return 0;
    }
    if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
        r->f.vkMapMemory(r->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
        return 0;
    }
    b->size = size;
    return 1;
}

static void free_buffer(SuRenderer *r, Buffer *b) {
    if (b->buf != VK_NULL_HANDLE) {
        r->f.vkDestroyBuffer(r->dev, b->buf, NULL);
    }
    if (b->mem != VK_NULL_HANDLE) {
        r->f.vkFreeMemory(r->dev, b->mem, NULL);
    }
    memset(b, 0, sizeof *b);
}

static int make_image(SuRenderer *r, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vi;
    uint32_t type = 0;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r->f.vkCreateImage(r->dev, &ii, NULL, img) != VK_SUCCESS) {
        return 0;
    }
    r->f.vkGetImageMemoryRequirements(r->dev, *img, &req);
    if (!find_memory(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        return 0;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (r->f.vkAllocateMemory(r->dev, &ai, NULL, mem) != VK_SUCCESS ||
        r->f.vkBindImageMemory(r->dev, *img, *mem, 0) != VK_SUCCESS) {
        return 0;
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return r->f.vkCreateImageView(r->dev, &vi, NULL, view) == VK_SUCCESS;
}

static void free_image(SuRenderer *r, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    if (*view != VK_NULL_HANDLE) {
        r->f.vkDestroyImageView(r->dev, *view, NULL);
    }
    if (*img != VK_NULL_HANDLE) {
        r->f.vkDestroyImage(r->dev, *img, NULL);
    }
    if (*mem != VK_NULL_HANDLE) {
        r->f.vkFreeMemory(r->dev, *mem, NULL);
    }
    *view = VK_NULL_HANDLE;
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
}

static int make_pass(SuRenderer *r) {
    VkAttachmentDescription at[2];
    VkAttachmentReference cref;
    VkAttachmentReference dref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo pi;
    memset(at, 0, sizeof at);
    at[0].format = r->color_format;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[0].finalLayout = r->offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    at[1].format = r->depth_format;
    at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    cref.attachment = 0;
    cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dref.attachment = 1;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    memset(dep, 0, sizeof dep);
    /* Previous frames' colour and depth writes (and the acquire, at colour output) before this frame's. */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    /* Colour writes before the readback copy (offscreen). */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pi.attachmentCount = 2;
    pi.pAttachments = at;
    pi.subpassCount = 1;
    pi.pSubpasses = &sub;
    pi.dependencyCount = r->offscreen ? 2u : 1u;
    pi.pDependencies = dep;
    return r->f.vkCreateRenderPass(r->dev, &pi, NULL, &r->pass) == VK_SUCCESS;
}

static VkShaderModule shader(SuRenderer *r, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo si;
    VkShaderModule m = VK_NULL_HANDLE;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = bytes;
    si.pCode = code;
    (void)r->f.vkCreateShaderModule(r->dev, &si, NULL, &m);
    return m;
}

typedef struct PushConstants {
    float view_proj[16];
    float light_dir_ambient[4];
} PushConstants;

static int make_pipeline(SuRenderer *r, int lines, VkPipeline *out) {
    VkShaderModule vs = lines ? shader(r, su_shader_line_vert, sizeof su_shader_line_vert)
                              : shader(r, su_shader_mesh_vert, sizeof su_shader_mesh_vert);
    VkShaderModule fs = lines ? shader(r, su_shader_line_frag, sizeof su_shader_line_frag)
                              : shader(r, su_shader_mesh_frag, sizeof su_shader_mesh_frag);
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription bind[2];
    VkVertexInputAttributeDescription attr[5];
    VkPipelineVertexInputStateCreateInfo vin;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo gi;
    VkResult vr;
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        return 0;
    }
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    memset(bind, 0, sizeof bind);
    memset(attr, 0, sizeof attr);
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (lines) {
        bind[0].binding = 0;
        bind[0].stride = sizeof(SuRenderLineVertex);
        bind[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attr[0].location = 0;
        attr[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attr[0].offset = 0;
        attr[1].location = 1;
        attr[1].format = VK_FORMAT_R8G8B8A8_UNORM;
        attr[1].offset = 12;
        vin.vertexBindingDescriptionCount = 1;
        vin.vertexAttributeDescriptionCount = 2;
    } else {
        bind[0].binding = 0;
        bind[0].stride = sizeof(SuMeshVertex);
        bind[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind[1].binding = 1;
        bind[1].stride = sizeof(SuRenderInstance);
        bind[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        attr[0].location = 0;
        attr[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attr[0].offset = 0;
        attr[1].location = 1;
        attr[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attr[1].offset = 12;
        attr[2].location = 2;
        attr[2].binding = 1;
        attr[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attr[2].offset = 0;
        attr[3].location = 3;
        attr[3].binding = 1;
        attr[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attr[3].offset = 16;
        attr[4].location = 4;
        attr[4].binding = 1;
        attr[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attr[4].offset = 32;
        vin.vertexBindingDescriptionCount = 2;
        vin.vertexAttributeDescriptionCount = 5;
    }
    vin.pVertexBindingDescriptions = bind;
    vin.pVertexAttributeDescriptions = attr;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* Meshes are counter-clockwise seen from outside; the projection flips y into Vulkan's y-down clip space,
     * which keeps them counter-clockwise in Vulkan's framebuffer convention. The offscreen test checks this. */
    rs.cullMode = lines ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = lines ? VK_FALSE : VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    memset(&gi, 0, sizeof gi);
    gi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = r->layout;
    gi.renderPass = r->pass;
    vr = r->f.vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gi, NULL, out);
    r->f.vkDestroyShaderModule(r->dev, vs, NULL);
    r->f.vkDestroyShaderModule(r->dev, fs, NULL);
    return vr == VK_SUCCESS;
}

/* One-time submit of a copy into device-local memory. */
static int upload(SuRenderer *r, const void *data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer *dst) {
    Buffer staging;
    VkCommandBuffer cmd = r->frames[0].cmd;
    VkCommandBufferBeginInfo bi;
    VkBufferCopy copy;
    VkSubmitInfo si;
    int ok;
    if (!make_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) {
        free_buffer(r, &staging);
        return 0;
    }
    memcpy(staging.map, data, (size_t)size);
    if (!make_buffer(r, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst)) {
        free_buffer(r, &staging);
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    (void)r->f.vkBeginCommandBuffer(cmd, &bi);
    copy.srcOffset = 0;
    copy.dstOffset = 0;
    copy.size = size;
    r->f.vkCmdCopyBuffer(cmd, staging.buf, dst->buf, 1, &copy);
    (void)r->f.vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    ok = r->f.vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && r->f.vkQueueWaitIdle(r->queue) == VK_SUCCESS;
    (void)r->f.vkResetCommandBuffer(cmd, 0);
    free_buffer(r, &staging);
    return ok;
}

static int make_meshes(SuRenderer *r) {
    SuMeshVertex *verts = NULL;
    uint16_t *idx = NULL;
    uint32_t total_v = 0;
    uint32_t total_i = 0;
    uint32_t m;
    int ok;
    for (m = 0; m < SU_MESH_COUNT; m++) {
        uint32_t nv = 0;
        uint32_t ni = 0;
        (void)su_mesh_build(m, NULL, 0, &nv, NULL, 0, &ni);
        r->mesh_vertex_offset[m] = (int32_t)total_v;
        r->mesh_first_index[m] = total_i;
        r->mesh_index_count[m] = ni;
        total_v += nv;
        total_i += ni;
    }
    verts = malloc(total_v * sizeof *verts);
    idx = malloc(total_i * sizeof *idx);
    ok = verts != NULL && idx != NULL;
    for (m = 0; ok && m < SU_MESH_COUNT; m++) {
        uint32_t nv = 0;
        uint32_t ni = 0;
        ok = su_mesh_build(m, verts + r->mesh_vertex_offset[m], total_v - (uint32_t)r->mesh_vertex_offset[m], &nv,
                           idx + r->mesh_first_index[m], total_i - r->mesh_first_index[m], &ni) == SU_OK;
    }
    ok = ok && upload(r, verts, total_v * sizeof *verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &r->mesh_vb) &&
         upload(r, idx, total_i * sizeof *idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &r->mesh_ib);
    free(verts);
    free(idx);
    return ok;
}

static void destroy_targets(SuRenderer *r) {
    uint32_t i;
    for (i = 0; i < r->image_count; i++) {
        if (r->fbs[i] != VK_NULL_HANDLE) {
            r->f.vkDestroyFramebuffer(r->dev, r->fbs[i], NULL);
        }
        if (!r->offscreen && r->views[i] != VK_NULL_HANDLE) {
            r->f.vkDestroyImageView(r->dev, r->views[i], NULL);
        }
        if (r->rendered[i] != VK_NULL_HANDLE) {
            r->f.vkDestroySemaphore(r->dev, r->rendered[i], NULL);
        }
        r->fbs[i] = VK_NULL_HANDLE;
        r->views[i] = VK_NULL_HANDLE;
        r->rendered[i] = VK_NULL_HANDLE;
    }
    r->image_count = 0;
    free_image(r, &r->depth_img, &r->depth_mem, &r->depth_view);
    free_image(r, &r->color_img, &r->color_mem, &r->color_view);
    free_buffer(r, &r->readback);
}

static int make_framebuffers(SuRenderer *r) {
    uint32_t i;
    if (!make_image(r, r->width, r->height, r->depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &r->depth_img, &r->depth_mem, &r->depth_view)) {
        return 0;
    }
    for (i = 0; i < r->image_count; i++) {
        VkImageView att[2];
        VkFramebufferCreateInfo fi;
        VkSemaphoreCreateInfo si;
        att[0] = r->views[i];
        att[1] = r->depth_view;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = r->pass;
        fi.attachmentCount = 2;
        fi.pAttachments = att;
        fi.width = r->width;
        fi.height = r->height;
        fi.layers = 1;
        if (r->f.vkCreateFramebuffer(r->dev, &fi, NULL, &r->fbs[i]) != VK_SUCCESS) {
            return 0;
        }
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (r->f.vkCreateSemaphore(r->dev, &si, NULL, &r->rendered[i]) != VK_SUCCESS) {
            return 0;
        }
    }
    return 1;
}

static int make_offscreen_targets(SuRenderer *r) {
    if (!make_image(r, r->width, r->height, r->color_format,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    &r->color_img, &r->color_mem, &r->color_view)) {
        return 0;
    }
    r->image_count = 1;
    r->images[0] = r->color_img;
    r->views[0] = r->color_view;
    if (!make_buffer(r, (VkDeviceSize)r->width * r->height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r->readback)) {
        return 0;
    }
    return make_framebuffers(r);
}

/* (Re)creates the swapchain for the current window size. Returns 1 when ready, 0 on failure, -1 when the window
 * has no area (minimized). */
static int make_swapchain(SuRenderer *r) {
    VkSurfaceCapabilitiesKHR caps;
    VkPresentModeKHR modes[16];
    uint32_t nmodes = 16;
    VkSwapchainCreateInfoKHR sci;
    VkSwapchainKHR old = r->swapchain;
    uint32_t i;
    uint32_t n;
    if (r->f.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->pd, r->surface, &caps) != VK_SUCCESS) {
        return 0;
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        r->width = caps.currentExtent.width;
        r->height = caps.currentExtent.height;
    } else {
        r->width = r->want_width;
        r->height = r->want_height;
    }
    if (r->width == 0u || r->height == 0u) {
        return -1;
    }
    r->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if ((r->flags & SU_RENDER_FLAG_NO_VSYNC) != 0u &&
        r->f.vkGetPhysicalDeviceSurfacePresentModesKHR(r->pd, r->surface, &nmodes, modes) >= 0) {
        for (i = 0; i < nmodes; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                r->present_mode = modes[i];
            }
        }
        for (i = 0; i < nmodes && r->present_mode == VK_PRESENT_MODE_FIFO_KHR; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                r->present_mode = modes[i];
            }
        }
    }
    n = caps.minImageCount + 1u;
    if (caps.maxImageCount != 0u && n > caps.maxImageCount) {
        n = caps.maxImageCount;
    }
    if (n > MAX_SWAP_IMAGES) {
        n = MAX_SWAP_IMAGES;
    }
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = r->surface;
    sci.minImageCount = n;
    sci.imageFormat = r->color_format;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent.width = r->width;
    sci.imageExtent.height = r->height;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));
    sci.presentMode = r->present_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    (void)r->f.vkDeviceWaitIdle(r->dev);
    destroy_targets(r);
    if (r->f.vkCreateSwapchainKHR(r->dev, &sci, NULL, &r->swapchain) != VK_SUCCESS) {
        r->swapchain = VK_NULL_HANDLE;
        if (old != VK_NULL_HANDLE) {
            r->f.vkDestroySwapchainKHR(r->dev, old, NULL);
        }
        return 0;
    }
    if (old != VK_NULL_HANDLE) {
        r->f.vkDestroySwapchainKHR(r->dev, old, NULL);
    }
    n = MAX_SWAP_IMAGES;
    if (r->f.vkGetSwapchainImagesKHR(r->dev, r->swapchain, &n, r->images) < 0) {
        return 0;
    }
    r->image_count = n;
    for (i = 0; i < n; i++) {
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = r->images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = r->color_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (r->f.vkCreateImageView(r->dev, &vi, NULL, &r->views[i]) != VK_SUCCESS) {
            return 0;
        }
    }
    r->needs_rebuild = 0;
    return make_framebuffers(r);
}

static VkFormat pick_surface_format(SuRenderer *r) {
    VkSurfaceFormatKHR fmts[64];
    uint32_t n = 64;
    uint32_t i;
    if (r->f.vkGetPhysicalDeviceSurfaceFormatsKHR(r->pd, r->surface, &n, fmts) < 0 || n == 0u) {
        return VK_FORMAT_UNDEFINED;
    }
    for (i = 0; i < n; i++) {
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB || fmts[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return fmts[i].format;
        }
    }
    return fmts[0].format;
}

void su_render_destroy(SuRenderer *r) {
    uint32_t i;
    if (r == NULL) {
        return;
    }
    if (r->dev != VK_NULL_HANDLE) {
        (void)r->f.vkDeviceWaitIdle(r->dev);
        destroy_targets(r);
        if (r->swapchain != VK_NULL_HANDLE) {
            r->f.vkDestroySwapchainKHR(r->dev, r->swapchain, NULL);
        }
        for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
            Frame *fr = &r->frames[i];
            free_buffer(r, &fr->instances);
            free_buffer(r, &fr->lines);
            if (fr->fence != VK_NULL_HANDLE) {
                r->f.vkDestroyFence(r->dev, fr->fence, NULL);
            }
            if (fr->acquired != VK_NULL_HANDLE) {
                r->f.vkDestroySemaphore(r->dev, fr->acquired, NULL);
            }
            if (fr->queries != VK_NULL_HANDLE) {
                r->f.vkDestroyQueryPool(r->dev, fr->queries, NULL);
            }
        }
        free_buffer(r, &r->mesh_vb);
        free_buffer(r, &r->mesh_ib);
        if (r->mesh_pipe != VK_NULL_HANDLE) {
            r->f.vkDestroyPipeline(r->dev, r->mesh_pipe, NULL);
        }
        if (r->line_pipe != VK_NULL_HANDLE) {
            r->f.vkDestroyPipeline(r->dev, r->line_pipe, NULL);
        }
        if (r->layout != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineLayout(r->dev, r->layout, NULL);
        }
        if (r->pass != VK_NULL_HANDLE) {
            r->f.vkDestroyRenderPass(r->dev, r->pass, NULL);
        }
        if (r->pool != VK_NULL_HANDLE) {
            r->f.vkDestroyCommandPool(r->dev, r->pool, NULL);
        }
        r->f.vkDestroyDevice(r->dev, NULL);
    }
    if (r->inst != VK_NULL_HANDLE) {
        if (r->surface != VK_NULL_HANDLE) {
            r->f.vkDestroySurfaceKHR(r->inst, r->surface, NULL);
        }
        if (r->messenger != VK_NULL_HANDLE && r->f.vkDestroyDebugUtilsMessengerEXT != NULL) {
            r->f.vkDestroyDebugUtilsMessengerEXT(r->inst, r->messenger, NULL);
        }
        r->f.vkDestroyInstance(r->inst, NULL);
    }
    unload_loader(&r->f);
    free(r->sorted);
    free(r);
}

static uint32_t create_fail(SuRenderer *r, uint32_t code, char *message, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (message != NULL) {
        (void)vsnprintf(message, 256, fmt, ap);
    }
    va_end(ap);
    su_render_destroy(r);
    return code;
}

static int score(const SuRenderDeviceInfo *i) {
    static const int by_type[] = {1, 3, 4, 2, 0};
    return i->meets_baseline ? 10 + by_type[i->type <= SU_RENDER_DEVICE_CPU ? i->type : 0] : 0;
}

uint32_t su_render_create(const SuRenderDesc *desc, SuRenderer **out, char message[256]) {
    SuRenderer *r;
    VkPhysicalDevice pds[MAX_DEVICES];
    SuRenderDeviceInfo infos[MAX_DEVICES];
    int families[MAX_DEVICES];
    uint32_t n = MAX_DEVICES;
    uint32_t i;
    int best = -1;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    const char *dev_exts[1];
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cai;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli;
    VkResult vr;

    if (message != NULL) {
        message[0] = '\0';
    }
    if (out == NULL || desc == NULL || desc->struct_size != sizeof *desc ||
        (desc->flags & ~(SU_RENDER_FLAG_VALIDATION | SU_RENDER_FLAG_NO_VSYNC)) != 0u ||
        (desc->hwnd == NULL && (desc->width == 0u || desc->height == 0u || desc->width > 16384u || desc->height > 16384u))) {
        if (message != NULL) {
            snprintf(message, 256, "invalid descriptor");
        }
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
#if !defined(_WIN32)
    if (desc->hwnd != NULL) {
        if (message != NULL) {
            snprintf(message, 256, "window presentation is implemented for Windows only; use offscreen");
        }
        return SU_E_UNSUPPORTED_METHOD;
    }
#endif
    r = calloc(1, sizeof *r);
    if (r == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    r->offscreen = desc->hwnd == NULL;
    r->hwnd = desc->hwnd;
    r->flags = desc->flags;
    r->want_width = r->width = desc->width;
    r->want_height = r->height = desc->height;
    r->max_instances = desc->max_instances != 0u ? desc->max_instances : DEFAULT_INSTANCES;
    r->max_lines = desc->max_line_vertices != 0u ? desc->max_line_vertices : DEFAULT_LINE_VERTICES;
    r->stats.gpu_ms = -1.0;
    r->stats.present_mode = UINT32_MAX;
    r->sorted = malloc((size_t)r->max_instances * sizeof *r->sorted);
    if (r->sorted == NULL) {
        return create_fail(r, SU_E_RESOURCE_LIMIT, message, "allocation failed");
    }
    if (!load_loader(&r->f, message)) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, NULL, "");
    }
    if (!make_instance(&r->f, !r->offscreen, (desc->flags & SU_RENDER_FLAG_VALIDATION) != 0u, &r->validation, &r->inst,
                       &r->messenger, message)) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, NULL, "");
    }
#if defined(_WIN32)
    if (!r->offscreen) {
        VkWin32SurfaceCreateInfoKHR si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        si.hinstance = GetModuleHandleW(NULL);
        si.hwnd = (HWND)desc->hwnd;
        if (r->f.vkCreateWin32SurfaceKHR == NULL ||
            (vr = r->f.vkCreateWin32SurfaceKHR(r->inst, &si, NULL, &r->surface)) != VK_SUCCESS) {
            return create_fail(r, SU_E_IO, message, "the window surface could not be created");
        }
    }
#endif
    if (r->f.vkEnumeratePhysicalDevices(r->inst, &n, pds) < 0 || n == 0u) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, message, "no Vulkan physical device");
    }
    for (i = 0; i < n; i++) {
        families[i] = describe(&r->f, pds[i], r->surface, &infos[i]);
        if (desc->device_index < 0 ? (best < 0 || score(&infos[i]) > score(&infos[best])) : (int)i == desc->device_index) {
            best = (int)i;
        }
    }
    if (best < 0 || !infos[best].meets_baseline) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, message, "no usable device%s%s",
                           best >= 0 ? ": " : "", best >= 0 ? infos[best].why_not : "");
    }
    r->pd = pds[best];
    r->info = infos[best];
    r->family = (uint32_t)families[best];
    r->f.vkGetPhysicalDeviceMemoryProperties(r->pd, &r->memprops);
    r->depth_format = pick_depth(&r->f, r->pd);
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = r->family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    dev_exts[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = r->offscreen ? 0u : 1u;
    di.ppEnabledExtensionNames = dev_exts;
    vr = r->f.vkCreateDevice(r->pd, &di, NULL, &r->dev);
    if (vr != VK_SUCCESS) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, message, "vkCreateDevice failed (VkResult %d)", (int)vr);
    }
    {
        const char *missing = load_device_fns(&r->f, r->dev, !r->offscreen);
        if (missing != NULL) {
            return create_fail(r, SU_E_UNSUPPORTED_METHOD, message, "device entry point %s is missing", missing);
        }
    }
    r->f.vkGetDeviceQueue(r->dev, r->family, 0, &r->queue);
    r->color_format = r->offscreen ? VK_FORMAT_R8G8B8A8_SRGB : pick_surface_format(r);
    if (r->color_format == VK_FORMAT_UNDEFINED) {
        return create_fail(r, SU_E_IO, message, "the surface reports no format");
    }
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = r->family;
    if (r->f.vkCreateCommandPool(r->dev, &pci, NULL, &r->pool) != VK_SUCCESS) {
        return create_fail(r, SU_E_RESOURCE_LIMIT, message, "command pool");
    }
    for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
        Frame *fr = &r->frames[i];
        VkFenceCreateInfo fi;
        VkSemaphoreCreateInfo si;
        VkQueryPoolCreateInfo qpi;
        memset(&cai, 0, sizeof cai);
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = r->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (r->f.vkAllocateCommandBuffers(r->dev, &cai, &fr->cmd) != VK_SUCCESS ||
            r->f.vkCreateFence(r->dev, &fi, NULL, &fr->fence) != VK_SUCCESS ||
            r->f.vkCreateSemaphore(r->dev, &si, NULL, &fr->acquired) != VK_SUCCESS ||
            !make_buffer(r, (VkDeviceSize)r->max_instances * sizeof(SuRenderInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &fr->instances) ||
            !make_buffer(r, (VkDeviceSize)r->max_lines * sizeof(SuRenderLineVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &fr->lines)) {
            return create_fail(r, SU_E_RESOURCE_LIMIT, message, "per-frame resources");
        }
        if (r->info.timestamps) {
            memset(&qpi, 0, sizeof qpi);
            qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 2;
            if (r->f.vkCreateQueryPool(r->dev, &qpi, NULL, &fr->queries) != VK_SUCCESS) {
                fr->queries = VK_NULL_HANDLE;
            }
        }
    }
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(PushConstants);
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (r->f.vkCreatePipelineLayout(r->dev, &pli, NULL, &r->layout) != VK_SUCCESS || !make_pass(r) ||
        !make_pipeline(r, 0, &r->mesh_pipe) || !make_pipeline(r, 1, &r->line_pipe)) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, message, "render pass or pipelines could not be created");
    }
    if (!make_meshes(r)) {
        return create_fail(r, SU_E_RESOURCE_LIMIT, message, "mesh upload failed");
    }
    if (r->offscreen) {
        if (!make_offscreen_targets(r)) {
            return create_fail(r, SU_E_RESOURCE_LIMIT, message, "offscreen target");
        }
    } else if (make_swapchain(r) == 0) {
        return create_fail(r, SU_E_IO, message, "the swapchain could not be created");
    }
    r->stats.present_mode = r->offscreen ? UINT32_MAX : (uint32_t)r->present_mode;
    r->stats.width = r->width;
    r->stats.height = r->height;
    *out = r;
    return SU_OK;
}

uint32_t su_render_device(const SuRenderer *r, SuRenderDeviceInfo *out) {
    if (r == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = r->info;
    return SU_OK;
}

void su_render_resize(SuRenderer *r, uint32_t width, uint32_t height) {
    if (r != NULL && !r->offscreen) {
        r->want_width = width;
        r->want_height = height;
        r->needs_rebuild = 1;
    }
}

uint32_t su_render_stats(const SuRenderer *r, SuRenderStats *out) {
    if (r == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = r->stats;
    out->validation_enabled = r->validation.enabled;
    out->validation_errors = r->validation.errors;
    out->validation_warnings = r->validation.warnings;
    return SU_OK;
}

const char *su_render_last_error(const SuRenderer *r) {
    return r != NULL ? r->last_error : "no renderer";
}

/* Reads the timestamps of a frame slot whose fence has signalled. */
static void collect_gpu_time(SuRenderer *r, Frame *fr) {
    uint64_t ts[2];
    if (!fr->timed || fr->queries == VK_NULL_HANDLE) {
        return;
    }
    if (r->f.vkGetQueryPoolResults(r->dev, fr->queries, 0, 2, sizeof ts, ts, sizeof ts[0],
                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        r->stats.gpu_ms = (double)(ts[1] - ts[0]) * (double)r->info.timestamp_period_ns / 1e6;
    }
    fr->timed = 0;
}

static void push_view(SuRenderer *r, VkCommandBuffer cmd, const SuRenderView *view) {
    PushConstants pc;
    SuMat4f vp = su_mat4f_mul(view->proj, view->view);
    memcpy(pc.view_proj, vp.m, sizeof pc.view_proj);
    pc.light_dir_ambient[0] = view->light_dir.x;
    pc.light_dir_ambient[1] = view->light_dir.y;
    pc.light_dir_ambient[2] = view->light_dir.z;
    pc.light_dir_ambient[3] = view->ambient;
    r->f.vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
}

uint32_t su_render_frame(SuRenderer *r, const SuRenderView *view, const SuRenderInstance *instances,
                         uint32_t instance_count, const SuRenderLineVertex *lines, uint32_t line_vertex_count) {
    Frame *fr;
    double t0;
    double tw;
    double tp = 0.0;
    uint32_t image = 0;
    uint32_t counts[SU_MESH_COUNT];
    uint32_t starts[SU_MESH_COUNT];
    uint32_t n_inst;
    uint32_t n_lines;
    uint32_t i;
    uint32_t m;
    VkResult vr;
    VkCommandBufferBeginInfo bi;
    VkRenderPassBeginInfo rp;
    VkClearValue clear[2];
    VkViewport vpt;
    VkRect2D sc;
    VkSubmitInfo si;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkDeviceSize zero = 0;

    if (r == NULL || view == NULL || (instances == NULL && instance_count > 0u) || (lines == NULL && line_vertex_count > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    t0 = now_ms();
    if (!r->offscreen && (r->needs_rebuild || r->swapchain == VK_NULL_HANDLE)) {
        int s = make_swapchain(r);
        if (s < 0) {
            return SU_E_RESOURCE_LIMIT;
        }
        if (s == 0) {
            return fail(r, SU_E_IO, "the swapchain could not be recreated");
        }
        r->stats.swapchain_recreations++;
    }
    fr = &r->frames[r->frame_slot];
    tw = now_ms();
    (void)r->f.vkWaitForFences(r->dev, 1, &fr->fence, VK_TRUE, UINT64_MAX);
    collect_gpu_time(r, fr);
    if (!r->offscreen) {
        vr = r->f.vkAcquireNextImageKHR(r->dev, r->swapchain, UINT64_MAX, fr->acquired, VK_NULL_HANDLE, &image);
        if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
            r->needs_rebuild = 1;
            return fail(r, SU_E_RESOURCE_LIMIT, "swapchain out of date; recreated on the next frame");
        }
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            return fail(r, SU_E_IO, "vkAcquireNextImageKHR failed (VkResult %d)", (int)vr);
        }
        if (vr == VK_SUBOPTIMAL_KHR) {
            r->needs_rebuild = 1;
        }
    }
    r->stats.wait_ms = now_ms() - tw;
    (void)r->f.vkResetFences(r->dev, 1, &fr->fence);

    /* Instances, grouped by mesh (counting sort; stable within a mesh). */
    n_inst = instance_count < r->max_instances ? instance_count : r->max_instances;
    memset(counts, 0, sizeof counts);
    for (i = 0; i < n_inst; i++) {
        counts[instances[i].mesh < SU_MESH_COUNT ? instances[i].mesh : SU_MESH_BOX]++;
    }
    starts[0] = 0;
    for (m = 1; m < SU_MESH_COUNT; m++) {
        starts[m] = starts[m - 1u] + counts[m - 1u];
    }
    {
        uint32_t fill[SU_MESH_COUNT];
        SuRenderInstance *dst = fr->instances.map;
        memcpy(fill, starts, sizeof fill);
        for (i = 0; i < n_inst; i++) {
            uint32_t mm = instances[i].mesh < SU_MESH_COUNT ? instances[i].mesh : SU_MESH_BOX;
            dst[fill[mm]++] = instances[i];
        }
    }
    n_lines = line_vertex_count < r->max_lines ? line_vertex_count & ~1u : r->max_lines & ~1u;
    if (n_lines > 0u) {
        memcpy(fr->lines.map, lines, (size_t)n_lines * sizeof *lines);
    }
    r->stats.dropped = (instance_count - n_inst) + (line_vertex_count - n_lines);

    (void)r->f.vkResetCommandBuffer(fr->cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    (void)r->f.vkBeginCommandBuffer(fr->cmd, &bi);
    if (fr->queries != VK_NULL_HANDLE) {
        r->f.vkCmdResetQueryPool(fr->cmd, fr->queries, 0, 2);
        r->f.vkCmdWriteTimestamp(fr->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, fr->queries, 0);
    }
    memset(clear, 0, sizeof clear);
    memcpy(clear[0].color.float32, view->clear, sizeof view->clear);
    clear[1].depthStencil.depth = 1.0f;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = r->pass;
    rp.framebuffer = r->fbs[r->offscreen ? 0u : image];
    rp.renderArea.extent.width = r->width;
    rp.renderArea.extent.height = r->height;
    rp.clearValueCount = 2;
    rp.pClearValues = clear;
    r->f.vkCmdBeginRenderPass(fr->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    memset(&vpt, 0, sizeof vpt);
    vpt.width = (float)r->width;
    vpt.height = (float)r->height;
    vpt.maxDepth = 1.0f;
    memset(&sc, 0, sizeof sc);
    sc.extent.width = r->width;
    sc.extent.height = r->height;
    r->f.vkCmdSetViewport(fr->cmd, 0, 1, &vpt);
    r->f.vkCmdSetScissor(fr->cmd, 0, 1, &sc);
    r->stats.draws = 0;
    if (n_inst > 0u) {
        VkBuffer vbs[2];
        VkDeviceSize offs[2] = {0, 0};
        vbs[0] = r->mesh_vb.buf;
        vbs[1] = fr->instances.buf;
        r->f.vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->mesh_pipe);
        push_view(r, fr->cmd, view);
        r->f.vkCmdBindVertexBuffers(fr->cmd, 0, 2, vbs, offs);
        r->f.vkCmdBindIndexBuffer(fr->cmd, r->mesh_ib.buf, 0, VK_INDEX_TYPE_UINT16);
        for (m = 0; m < SU_MESH_COUNT; m++) {
            if (counts[m] > 0u) {
                r->f.vkCmdDrawIndexed(fr->cmd, r->mesh_index_count[m], counts[m], r->mesh_first_index[m],
                                      r->mesh_vertex_offset[m], starts[m]);
                r->stats.draws++;
            }
        }
    }
    if (n_lines > 0u) {
        r->f.vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->line_pipe);
        push_view(r, fr->cmd, view);
        r->f.vkCmdBindVertexBuffers(fr->cmd, 0, 1, &fr->lines.buf, &zero);
        r->f.vkCmdDraw(fr->cmd, n_lines, 1, 0, 0);
        r->stats.draws++;
    }
    r->f.vkCmdEndRenderPass(fr->cmd);
    if (r->offscreen) {
        VkBufferImageCopy copy;
        VkMemoryBarrier mb;
        memset(&copy, 0, sizeof copy);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = r->width;
        copy.imageExtent.height = r->height;
        copy.imageExtent.depth = 1;
        /* The readback buffer is shared by the frames in flight: the previous frame's copy into it (and any host
         * read, which su_render_readback orders with a device wait) completes before this one writes. */
        memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        r->f.vkCmdPipelineBarrier(fr->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                  NULL, 0, NULL);
        r->f.vkCmdCopyImageToBuffer(fr->cmd, r->color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->readback.buf, 1, &copy);
        /* Make the copy visible to host reads after the fence or device wait. */
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        r->f.vkCmdPipelineBarrier(fr->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL,
                                  0, NULL);
    }
    if (fr->queries != VK_NULL_HANDLE) {
        r->f.vkCmdWriteTimestamp(fr->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fr->queries, 1);
        fr->timed = 1;
    }
    (void)r->f.vkEndCommandBuffer(fr->cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr->cmd;
    if (!r->offscreen) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &fr->acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &r->rendered[image];
    }
    vr = r->f.vkQueueSubmit(r->queue, 1, &si, fr->fence);
    if (vr != VK_SUCCESS) {
        return fail(r, SU_E_IO, "vkQueueSubmit failed (VkResult %d)", (int)vr);
    }
    fr->submitted = 1;
    if (!r->offscreen) {
        VkPresentInfoKHR pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &r->rendered[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &r->swapchain;
        pi.pImageIndices = &image;
        tp = now_ms();
        vr = r->f.vkQueuePresentKHR(r->queue, &pi);
        r->stats.present_ms = now_ms() - tp;
        if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
            r->needs_rebuild = 1;
        } else if (vr != VK_SUCCESS) {
            return fail(r, SU_E_IO, "vkQueuePresentKHR failed (VkResult %d)", (int)vr);
        }
    }
    r->frame_slot = (r->frame_slot + 1u) % FRAMES_IN_FLIGHT;
    r->stats.frame++;
    r->stats.instances = n_inst;
    r->stats.line_vertices = n_lines;
    r->stats.width = r->width;
    r->stats.height = r->height;
    r->stats.cpu_ms = now_ms() - t0 - r->stats.wait_ms - r->stats.present_ms;
    return SU_OK;
}

uint32_t su_render_readback(SuRenderer *r, uint8_t *rgba, size_t cap) {
    uint32_t i;
    if (r == NULL || rgba == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!r->offscreen) {
        return fail(r, SU_E_UNSUPPORTED_METHOD, "readback is for offscreen renderers");
    }
    if (cap < (size_t)r->width * r->height * 4u) {
        return SU_E_RESOURCE_LIMIT;
    }
    (void)r->f.vkDeviceWaitIdle(r->dev);
    /* Oldest submission first, so the stats end with the newest frame's GPU time. */
    for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
        collect_gpu_time(r, &r->frames[(r->frame_slot + i) % FRAMES_IN_FLIGHT]);
    }
    memcpy(rgba, r->readback.map, (size_t)r->width * r->height * 4u);
    return SU_OK;
}
