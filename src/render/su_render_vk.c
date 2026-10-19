/* Vulkan backend of sutekh/render.h: loader, instance, device, targets, passes, pipelines (with a persistent
 * pipeline cache) and frame orchestration. Resources and uploads are in su_render_res.c, the camera-relative scene
 * in su_render_scene.c.
 *
 * The Vulkan loader is opened at run time (vulkan-1.dll / libvulkan.so.1) and every entry point is fetched through
 * vkGetInstanceProcAddr / vkGetDeviceProcAddr, so the library has no link-time Vulkan dependency and fails with a
 * reason when Vulkan is missing.
 *
 * Frame structure: [queued uploads: copies + barriers] -> scene pass into an RGBA16F target with a reversed-Z D32
 * depth buffer (sky, opaque and masked PBR, debug meshes and lines, blended PBR, additive effects) -> output pass
 * into the swapchain image or the offscreen RGBA8 sRGB image (tonemapping, then the HUD) -> [offscreen readback
 * copy]. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "render/su_render_vk_int.h"

#include "ir/su_sha256.h"
#include "render/su_render_shaders.h"

/* ---------- loader ---------- */

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
#define X(name)                                                                         \
    f->name = (PFN_##name)f->vkGetDeviceProcAddr(dev, #name);                          \
    if (f->name == NULL && missing == NULL && (swapchain || !is_swapchain_fn(#name))) { \
        missing = #name;                                                                \
    }
    VK_DEVICE_FNS(X)
#undef X
    return missing;
}

double su_rvk_now_ms(void) {
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

/* A depth format the shadow map can render to and sample: D32 float, else D16 (both required to be sampleable when
 * present as attachments on conformant devices). */
static VkFormat pick_shadow_depth(VkFns *f, VkPhysicalDevice pd) {
    static const VkFormat cands[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    size_t i;
    for (i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        VkFormatProperties fp;
        f->vkGetPhysicalDeviceFormatProperties(pd, cands[i], &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) {
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
    VkFormatProperties hp;
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
    f->vkGetPhysicalDeviceFormatProperties(pd, HDR_FORMAT, &hp);
    info->bloom = (hp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0u &&
                  (hp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0u;
    {
        VkPhysicalDeviceFeatures ft;
        memset(&ft, 0, sizeof ft);
        f->vkGetPhysicalDeviceFeatures(pd, &ft);
        info->bc_textures = ft.textureCompressionBC == VK_TRUE;
    }
    if (p.apiVersion < VK_API_VERSION_1_2) {
        snprintf(info->why_not, sizeof info->why_not, "device supports Vulkan %u.%u; 1.2 is required",
                 VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    } else if (family < 0) {
        snprintf(info->why_not, sizeof info->why_not,
                 surface != VK_NULL_HANDLE ? "no graphics queue that can present to the window" : "no graphics queue");
    } else if (pick_depth(f, pd) == VK_FORMAT_UNDEFINED) {
        snprintf(info->why_not, sizeof info->why_not, "no depth attachment format");
    } else if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0 ||
               (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0 ||
               (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        snprintf(info->why_not, sizeof info->why_not, "R8G8B8A8_SRGB is not a renderable, sampled, copyable format");
    } else if ((hp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0 ||
               (hp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        snprintf(info->why_not, sizeof info->why_not, "R16G16B16A16_SFLOAT is not a renderable, sampled format");
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

/* ---------- memory ---------- */

uint32_t su_rvk_fail(SuRenderer *r, uint32_t code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(r->last_error, sizeof r->last_error, fmt, ap);
    va_end(ap);
    return code;
}

int su_rvk_find_memory(SuRenderer *r, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *type) {
    uint32_t i;
    for (i = 0; i < r->memprops.memoryTypeCount; i++) {
        if ((bits & (1u << i)) != 0 && (r->memprops.memoryTypes[i].propertyFlags & want) == want) {
            *type = i;
            return 1;
        }
    }
    return 0;
}

int su_rvk_make_buffer(SuRenderer *r, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       Buffer *b) {
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    uint32_t type = 0;
    memset(b, 0, sizeof *b);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size > 0u ? size : 4u;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (r->f.vkCreateBuffer(r->dev, &bi, NULL, &b->buf) != VK_SUCCESS) {
        return 0;
    }
    r->f.vkGetBufferMemoryRequirements(r->dev, b->buf, &req);
    if (!su_rvk_find_memory(r, req.memoryTypeBits, props, &type)) {
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

void su_rvk_free_buffer(SuRenderer *r, Buffer *b) {
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
    if (!su_rvk_find_memory(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
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

/* ---------- render passes ---------- */

static int make_scene_pass(SuRenderer *r) {
    VkAttachmentDescription at[2];
    VkAttachmentReference cref;
    VkAttachmentReference dref;
    VkAttachmentReference dro;
    VkSubpassDescription sub[2];
    VkSubpassDependency dep[3];
    VkRenderPassCreateInfo pi;
    memset(at, 0, sizeof at);
    at[0].format = HDR_FORMAT;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
    dro.attachment = 1;
    dro.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    memset(sub, 0, sizeof sub);
    sub[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub[0].colorAttachmentCount = 1;
    sub[0].pColorAttachments = &cref;
    sub[0].pDepthStencilAttachment = &dref;
    /* subpass 1, textured effects: depth tested read-only and read as an input attachment (soft particles) */
    sub[1] = sub[0];
    sub[1].pDepthStencilAttachment = &dro;
    sub[1].inputAttachmentCount = 1;
    sub[1].pInputAttachments = &dro;
    memset(dep, 0, sizeof dep);
    /* The previous frame's output pass read the HDR image (fragment shader) and its depth writes; both finish
     * before this frame's layout transitions and writes. */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    /* HDR colour writes before the output pass samples it. */
    dep[1].srcSubpass = 1;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    /* scene depth and colour of subpass 0 before the effects test, read and blend over them */
    dep[2].srcSubpass = 0;
    dep[2].dstSubpass = 1;
    dep[2].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[2].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[2].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[2].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pi.attachmentCount = 2;
    pi.pAttachments = at;
    pi.subpassCount = 2;
    pi.pSubpasses = sub;
    pi.dependencyCount = 3;
    pi.pDependencies = dep;
    return r->f.vkCreateRenderPass(r->dev, &pi, NULL, &r->scene_pass) == VK_SUCCESS;
}

/* The sun shadow pass: one depth attachment, cleared to 1 (far), left ready for sampling. */
static int make_shadow_pass(SuRenderer *r) {
    VkAttachmentDescription at;
    VkAttachmentReference dref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo pi;
    memset(&at, 0, sizeof at);
    at.format = r->shadow_format;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dref.attachment = 0;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &dref;
    memset(dep, 0, sizeof dep);
    /* the previous frame's lit passes read the map before it is cleared and written again */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = 0;
    dep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* depth writes before this frame's lit passes sample the map */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pi.attachmentCount = 1;
    pi.pAttachments = &at;
    pi.subpassCount = 1;
    pi.pSubpasses = &sub;
    pi.dependencyCount = 2;
    pi.pDependencies = dep;
    return r->f.vkCreateRenderPass(r->dev, &pi, NULL, &r->shadow_pass) == VK_SUCCESS;
}

static void free_shadow_map(SuRenderer *r) {
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        if (r->shadow_fbs[i] != VK_NULL_HANDLE) {
            r->f.vkDestroyFramebuffer(r->dev, r->shadow_fbs[i], NULL);
            r->shadow_fbs[i] = VK_NULL_HANDLE;
        }
        if (r->shadow_layer_views[i] != VK_NULL_HANDLE) {
            r->f.vkDestroyImageView(r->dev, r->shadow_layer_views[i], NULL);
            r->shadow_layer_views[i] = VK_NULL_HANDLE;
        }
    }
    free_image(r, &r->shadow_img, &r->shadow_mem, &r->shadow_view);
    r->shadow_size = 0;
    r->shadow_layers = 0;
}

/* (Re)creates the shadow map: layers x (size x size), sampled as an array, one framebuffer per layer, and points every
 * frame set's binding 1 at it. The caller has made sure no submitted work uses the old one. */
static int make_shadow_map(SuRenderer *r, uint32_t size, uint32_t layers) {
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vi;
    uint32_t type = 0;
    uint32_t i;
    free_shadow_map(r);
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = r->shadow_format;
    ii.extent.width = ii.extent.height = size;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r->f.vkCreateImage(r->dev, &ii, NULL, &r->shadow_img) != VK_SUCCESS) {
        r->shadow_img = VK_NULL_HANDLE;
        return 0;
    }
    r->f.vkGetImageMemoryRequirements(r->dev, r->shadow_img, &req);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    if (!su_rvk_find_memory(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        return 0;
    }
    ai.memoryTypeIndex = type;
    if (r->f.vkAllocateMemory(r->dev, &ai, NULL, &r->shadow_mem) != VK_SUCCESS) {
        r->shadow_mem = VK_NULL_HANDLE;
        return 0;
    }
    if (r->f.vkBindImageMemory(r->dev, r->shadow_img, r->shadow_mem, 0) != VK_SUCCESS) {
        return 0;
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = r->shadow_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = r->shadow_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = layers;
    if (r->f.vkCreateImageView(r->dev, &vi, NULL, &r->shadow_view) != VK_SUCCESS) {
        r->shadow_view = VK_NULL_HANDLE;
        return 0;
    }
    for (i = 0; i < layers; i++) {
        VkFramebufferCreateInfo fi;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange.baseArrayLayer = i;
        vi.subresourceRange.layerCount = 1;
        if (r->f.vkCreateImageView(r->dev, &vi, NULL, &r->shadow_layer_views[i]) != VK_SUCCESS) {
            r->shadow_layer_views[i] = VK_NULL_HANDLE;
            return 0;
        }
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = r->shadow_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &r->shadow_layer_views[i];
        fi.width = size;
        fi.height = size;
        fi.layers = 1;
        if (r->f.vkCreateFramebuffer(r->dev, &fi, NULL, &r->shadow_fbs[i]) != VK_SUCCESS) {
            r->shadow_fbs[i] = VK_NULL_HANDLE;
            return 0;
        }
    }
    for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo di;
        VkWriteDescriptorSet w;
        di.sampler = r->shadow_sampler;
        di.imageView = r->shadow_view;
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = r->frames[i].frame_set;
        w.dstBinding = 1;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &di;
        r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
    }
    r->shadow_size = size;
    r->shadow_layers = layers;
    r->shadow_fresh = 1;
    return 1;
}

static int make_post_pass(SuRenderer *r) {
    VkAttachmentDescription at;
    VkAttachmentReference cref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo pi;
    memset(&at, 0, sizeof at);
    at.format = r->color_format;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; /* the tonemap triangle covers every pixel */
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = r->offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    cref.attachment = 0;
    cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    memset(dep, 0, sizeof dep);
    /* The acquire (waited at colour output) and the previous readback copy of the offscreen image before writing. */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    /* Colour writes before the readback copy (offscreen). */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pi.attachmentCount = 1;
    pi.pAttachments = &at;
    pi.subpassCount = 1;
    pi.pSubpasses = &sub;
    pi.dependencyCount = r->offscreen ? 2u : 1u;
    pi.pDependencies = dep;
    return r->f.vkCreateRenderPass(r->dev, &pi, NULL, &r->post_pass) == VK_SUCCESS;
}

/* One colour attachment of HDR_FORMAT, written once per use (contents discarded at the start) and left readable by
 * fragment shaders: every level of both bloom chains is rendered through this pass. */
static int make_bloom_pass(SuRenderer *r) {
    VkAttachmentDescription at;
    VkAttachmentReference cref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo pi;
    memset(&at, 0, sizeof at);
    at.format = HDR_FORMAT;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cref.attachment = 0;
    cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    memset(dep, 0, sizeof dep);
    /* Earlier reads of the level (the previous frame's upsample or output pass) and writes finish before this
     * frame writes it; the scene pass's own dependency makes the HDR image readable by the first downsample. */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* The level's colour writes before any later fragment shader samples it. */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pi.attachmentCount = 1;
    pi.pAttachments = &at;
    pi.subpassCount = 1;
    pi.pSubpasses = &sub;
    pi.dependencyCount = 2;
    pi.pDependencies = dep;
    return r->f.vkCreateRenderPass(r->dev, &pi, NULL, &r->bloom_pass) == VK_SUCCESS;
}

/* ---------- pipeline cache ---------- */

#define CACHE_MAGIC "SUPCACHE"
#define CACHE_HEADER 80u

static void cache_load(SuRenderer *r, const char *path, VkPipelineCacheCreateInfo *ci, uint8_t **data) {
    FILE *fh;
    long n;
    uint8_t *b;
    uint8_t d[32];
    SuSha256 s;
    uint64_t size;
    *data = NULL;
    fh = fopen(path, "rb");
    if (fh == NULL) {
        r->stats.pipeline_cache = 3u;
        return;
    }
    r->stats.pipeline_cache = 2u;
    if (fseek(fh, 0, SEEK_END) != 0 || (n = ftell(fh)) < (long)CACHE_HEADER || fseek(fh, 0, SEEK_SET) != 0) {
        fclose(fh);
        return;
    }
    b = malloc((size_t)n);
    if (b == NULL || fread(b, 1, (size_t)n, fh) != (size_t)n) {
        fclose(fh);
        free(b);
        return;
    }
    fclose(fh);
    memcpy(&size, b + 40, 8);
    if (memcmp(b, CACHE_MAGIC, 8) != 0 || b[8] != 1u || memcmp(b + 12, &r->props.vendorID, 4) != 0 ||
        memcmp(b + 16, &r->props.deviceID, 4) != 0 || memcmp(b + 20, &r->props.driverVersion, 4) != 0 ||
        memcmp(b + 24, r->props.pipelineCacheUUID, 16) != 0 || size != (uint64_t)n - CACHE_HEADER) {
        free(b);
        return;
    }
    su_sha256_init(&s);
    su_sha256_update(&s, b + CACHE_HEADER, (size_t)size);
    su_sha256_final(&s, d);
    if (memcmp(d, b + 48, 32) != 0) {
        free(b);
        return;
    }
    ci->initialDataSize = (size_t)size;
    ci->pInitialData = b + CACHE_HEADER;
    *data = b;
    r->stats.pipeline_cache = 1u;
}

static void cache_save(SuRenderer *r) {
    size_t n = 0;
    uint8_t *b;
    uint8_t hdr[CACHE_HEADER];
    uint64_t size;
    SuSha256 s;
    char tmp[1100];
    FILE *fh;
    if (r->cache == VK_NULL_HANDLE || r->cache_path[0] == '\0' ||
        r->f.vkGetPipelineCacheData(r->dev, r->cache, &n, NULL) != VK_SUCCESS || n == 0u) {
        return;
    }
    b = malloc(n);
    if (b == NULL || r->f.vkGetPipelineCacheData(r->dev, r->cache, &n, b) != VK_SUCCESS) {
        free(b);
        return;
    }
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, CACHE_MAGIC, 8);
    hdr[8] = 1u;
    memcpy(hdr + 12, &r->props.vendorID, 4);
    memcpy(hdr + 16, &r->props.deviceID, 4);
    memcpy(hdr + 20, &r->props.driverVersion, 4);
    memcpy(hdr + 24, r->props.pipelineCacheUUID, 16);
    size = n;
    memcpy(hdr + 40, &size, 8);
    su_sha256_init(&s);
    su_sha256_update(&s, b, n);
    su_sha256_final(&s, hdr + 48);
    snprintf(tmp, sizeof tmp, "%s.tmp", r->cache_path);
    fh = fopen(tmp, "wb");
    if (fh != NULL) {
        int ok = fwrite(hdr, 1, sizeof hdr, fh) == sizeof hdr && fwrite(b, 1, n, fh) == n;
        ok = fclose(fh) == 0 && ok;
        if (ok) {
            (void)remove(r->cache_path);
            ok = rename(tmp, r->cache_path) == 0;
        }
        if (!ok) {
            (void)remove(tmp);
        }
    }
    free(b);
}

/* ---------- pipelines ---------- */

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

#define VIN_LEGACY_MESH 0
#define VIN_LINE 1
#define VIN_NONE 2
#define VIN_PBR 3
#define VIN_FX 4
#define VIN_HUD 5
#define VIN_SHADOW 6 /* asset vertex position and the instance rows only (shadow casters) */

#define BLEND_NONE 0
#define BLEND_ALPHA 1
#define BLEND_ADD 2
#define BLEND_PREMUL 3

typedef struct PipeSpec {
    const uint32_t *vs;
    size_t vs_bytes;
    const uint32_t *fs;
    size_t fs_bytes;
    int vin;
    int lines;
    int cull;          /* 1: back faces */
    int depth_test;
    int depth_write;
    int blend;
    int post;          /* output pass (no depth attachment) */
    int bloom;         /* 1: bloom downsample, 3: bloom upsample, 2: bloom output pass (all without depth) */
    int paint;         /* 1: the layered paint layout (frame, paint material, environment) */
    int layout;        /* 1: the textured-effects layout (frame, one sampler), 2: the cube sky layout */
    int subpass;       /* scene pass subpass: 0, or 1 for the textured effects */
    int shadow;        /* the shadow pass: depth only, standard depth (less or equal), slope bias */
} PipeSpec;

static void attr(VkVertexInputAttributeDescription *a, uint32_t *n, uint32_t loc, uint32_t binding, VkFormat fmt,
                 uint32_t off) {
    a[*n].location = loc;
    a[*n].binding = binding;
    a[*n].format = fmt;
    a[*n].offset = off;
    (*n)++;
}

static int make_pipe(SuRenderer *r, const PipeSpec *s, VkPipeline *out) {
    VkShaderModule vs = shader(r, s->vs, s->vs_bytes);
    VkShaderModule fs = shader(r, s->fs, s->fs_bytes);
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription bind[2];
    VkVertexInputAttributeDescription at[12];
    uint32_t nb = 0;
    uint32_t na = 0;
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
        if (vs != VK_NULL_HANDLE) {
            r->f.vkDestroyShaderModule(r->dev, vs, NULL);
        }
        if (fs != VK_NULL_HANDLE) {
            r->f.vkDestroyShaderModule(r->dev, fs, NULL);
        }
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
    memset(at, 0, sizeof at);
    switch (s->vin) {
    case VIN_LEGACY_MESH:
        bind[0].binding = 0;
        bind[0].stride = sizeof(SuMeshVertex);
        bind[1].binding = 1;
        bind[1].stride = sizeof(SuRenderInstance);
        bind[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        nb = 2;
        attr(at, &na, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
        attr(at, &na, 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12);
        attr(at, &na, 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
        attr(at, &na, 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
        attr(at, &na, 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32);
        break;
    case VIN_LINE:
        bind[0].stride = sizeof(SuRenderLineVertex);
        nb = 1;
        attr(at, &na, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
        attr(at, &na, 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12);
        break;
    case VIN_PBR:
        bind[0].stride = sizeof(SuAssetVertex);
        bind[1].binding = 1;
        bind[1].stride = sizeof(DrawInstance);
        bind[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        nb = 2;
        attr(at, &na, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
        attr(at, &na, 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12);
        attr(at, &na, 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24);
        attr(at, &na, 3, 0, VK_FORMAT_R32G32_SFLOAT, 40);
        attr(at, &na, 4, 0, VK_FORMAT_R32G32_SFLOAT, 48);
        attr(at, &na, 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
        attr(at, &na, 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
        attr(at, &na, 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32);
        attr(at, &na, 8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48);
        attr(at, &na, 9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64);
        break;
    case VIN_SHADOW:
        bind[0].stride = sizeof(SuAssetVertex);
        bind[1].binding = 1;
        bind[1].stride = sizeof(DrawInstance);
        bind[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        nb = 2;
        attr(at, &na, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
        attr(at, &na, 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
        attr(at, &na, 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
        attr(at, &na, 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32);
        break;
    case VIN_FX:
        bind[0].stride = sizeof(SuFxVertex);
        nb = 1;
        attr(at, &na, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
        attr(at, &na, 1, 0, VK_FORMAT_R32G32_SFLOAT, 12);
        attr(at, &na, 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20);
        break;
    case VIN_HUD:
        bind[0].stride = sizeof(SuHudVertex);
        nb = 1;
        attr(at, &na, 0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
        attr(at, &na, 1, 0, VK_FORMAT_R32G32_SFLOAT, 8);
        attr(at, &na, 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16);
        break;
    default:
        break;
    }
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = nb;
    vin.pVertexBindingDescriptions = bind;
    vin.vertexAttributeDescriptionCount = na;
    vin.pVertexAttributeDescriptions = at;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = s->lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* Meshes are counter-clockwise seen from outside; the projection flips y into Vulkan's y-down clip space,
     * which keeps them counter-clockwise in Vulkan's framebuffer convention. The offscreen test checks this. */
    rs.cullMode = s->cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = s->depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = s->depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = s->shadow ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL; /* reversed Z */
    if (s->shadow) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 1.0f;
        rs.depthBiasSlopeFactor = 1.5f;
    }
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    if (s->blend == BLEND_ALPHA) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (s->blend == BLEND_PREMUL) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (s->blend == BLEND_ADD) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = s->shadow ? 0u : 1u;
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
    gi.pDepthStencilState = (s->post || s->bloom) ? NULL : &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = s->post ? r->post_layout : s->paint ? r->paint_layout : r->scene_layout;
    if (s->layout == 1) {
        gi.layout = r->fxtex_layout;
    } else if (s->layout == 2) {
        gi.layout = r->skycube_layout;
    }
    gi.renderPass = s->post ? r->post_pass : s->shadow ? r->shadow_pass : r->scene_pass;
    gi.subpass = (uint32_t)s->subpass;
    if (s->bloom == 1 || s->bloom == 3) { /* 1: downsample (one sampler), 3: upsample (three samplers) */
        gi.layout = s->bloom == 1 ? r->bloom_layout : r->bloom3_layout;
        gi.renderPass = r->bloom_pass;
    } else if (s->bloom == 2) {
        gi.layout = r->bloom3_layout;
        gi.renderPass = r->post_pass;
    }
    vr = r->f.vkCreateGraphicsPipelines(r->dev, r->cache, 1, &gi, NULL, out);
    r->f.vkDestroyShaderModule(r->dev, vs, NULL);
    r->f.vkDestroyShaderModule(r->dev, fs, NULL);
    return vr == VK_SUCCESS;
}

#define SPV(name) su_shader_##name, sizeof su_shader_##name

static int make_pipelines(SuRenderer *r) {
    PipeSpec s;
    uint32_t i;
    double t0 = su_rvk_now_ms();
    int ok = 1;
    /* debug meshes and lines */
    memset(&s, 0, sizeof s);
    s.vs = su_shader_mesh_vert;
    s.vs_bytes = sizeof su_shader_mesh_vert;
    s.fs = su_shader_mesh_frag;
    s.fs_bytes = sizeof su_shader_mesh_frag;
    s.vin = VIN_LEGACY_MESH;
    s.cull = 1;
    s.depth_test = s.depth_write = 1;
    ok = ok && make_pipe(r, &s, &r->mesh_pipe);
    memset(&s, 0, sizeof s);
    s.vs = su_shader_line_vert;
    s.vs_bytes = sizeof su_shader_line_vert;
    s.fs = su_shader_line_frag;
    s.fs_bytes = sizeof su_shader_line_frag;
    s.vin = VIN_LINE;
    s.lines = 1;
    s.depth_test = 1;
    ok = ok && make_pipe(r, &s, &r->line_pipe);
    /* sky */
    memset(&s, 0, sizeof s);
    s.vs = su_shader_fullscreen_vert;
    s.vs_bytes = sizeof su_shader_fullscreen_vert;
    s.fs = su_shader_sky_frag;
    s.fs_bytes = sizeof su_shader_sky_frag;
    s.vin = VIN_NONE;
    ok = ok && make_pipe(r, &s, &r->sky_pipe);
    /* effects */
    memset(&s, 0, sizeof s);
    s.vs = su_shader_fx_vert;
    s.vs_bytes = sizeof su_shader_fx_vert;
    s.fs = su_shader_fx_frag;
    s.fs_bytes = sizeof su_shader_fx_frag;
    s.vin = VIN_FX;
    s.depth_test = 1;
    s.blend = BLEND_ADD;
    ok = ok && make_pipe(r, &s, &r->fx_pipe);
    /* textured effects: additive and premultiplied alpha, in the effects subpass */
    s.layout = 1;
    s.subpass = 1;
    s.fs = su_shader_fx_tex_add_frag;
    s.fs_bytes = sizeof su_shader_fx_tex_add_frag;
    ok = ok && make_pipe(r, &s, &r->fxtex_pipe[0]);
    s.fs = su_shader_fx_tex_pm_frag;
    s.fs_bytes = sizeof su_shader_fx_tex_pm_frag;
    s.blend = BLEND_PREMUL;
    ok = ok && make_pipe(r, &s, &r->fxtex_pipe[1]);
    /* the cube-map sky background */
    memset(&s, 0, sizeof s);
    s.vs = su_shader_fullscreen_vert;
    s.vs_bytes = sizeof su_shader_fullscreen_vert;
    s.fs = su_shader_sky_cube_frag;
    s.fs_bytes = sizeof su_shader_sky_cube_frag;
    s.vin = VIN_NONE;
    s.layout = 2;
    ok = ok && make_pipe(r, &s, &r->skycube_pipe);
    /* PBR variants */
    for (i = 0; i < PIPE_PBR_COUNT && ok; i++) {
        memset(&s, 0, sizeof s);
        s.vs = su_shader_pbr_vert;
        s.vs_bytes = sizeof su_shader_pbr_vert;
        s.fs = su_shader_pbr_frag;
        s.fs_bytes = sizeof su_shader_pbr_frag;
        s.vin = VIN_PBR;
        s.cull = (i & 1u) == 0u;
        s.depth_test = 1;
        s.depth_write = i < PIPE_BLEND;
        s.blend = i >= PIPE_BLEND ? BLEND_ALPHA : BLEND_NONE;
        ok = make_pipe(r, &s, &r->pbr_pipe[i]);
    }
    /* sun shadow casters: depth only */
    if (ok) {
        memset(&s, 0, sizeof s);
        s.vs = su_shader_shadow_vert;
        s.vs_bytes = sizeof su_shader_shadow_vert;
        s.fs = su_shader_shadow_frag;
        s.fs_bytes = sizeof su_shader_shadow_frag;
        s.vin = VIN_SHADOW;
        s.depth_test = s.depth_write = 1;
        s.shadow = 1;
        ok = make_pipe(r, &s, &r->shadow_pipe);
    }
    /* layered paint variants (same states, same vertex stage, own layout and fragment shader) */
    for (i = 0; i < PIPE_PBR_COUNT && ok; i++) {
        memset(&s, 0, sizeof s);
        s.vs = su_shader_pbr_vert;
        s.vs_bytes = sizeof su_shader_pbr_vert;
        s.fs = su_shader_paint_frag;
        s.fs_bytes = sizeof su_shader_paint_frag;
        s.vin = VIN_PBR;
        s.paint = 1;
        s.cull = (i & 1u) == 0u;
        s.depth_test = 1;
        s.depth_write = i < PIPE_BLEND;
        s.blend = i >= PIPE_BLEND ? BLEND_ALPHA : BLEND_NONE;
        ok = make_pipe(r, &s, &r->paint_pipe[i]);
    }
    /* output pass */
    memset(&s, 0, sizeof s);
    s.vs = su_shader_fullscreen_vert;
    s.vs_bytes = sizeof su_shader_fullscreen_vert;
    s.fs = su_shader_post_frag;
    s.fs_bytes = sizeof su_shader_post_frag;
    s.vin = VIN_NONE;
    s.post = 1;
    ok = ok && make_pipe(r, &s, &r->post_pipe);
    /* bloom: downsample, upsample, and the output pass that adds the result (device must filter the HDR format) */
    if (r->info.bloom) {
        static const struct {
            const uint32_t *fs;
            size_t fs_bytes;
            int kind;
            VkPipeline *out;
        } bl[3] = {{su_shader_bloom_down_frag, sizeof su_shader_bloom_down_frag, 1, NULL},
                   {su_shader_bloom_up_frag, sizeof su_shader_bloom_up_frag, 3, NULL},
                   {su_shader_post_bloom_frag, sizeof su_shader_post_bloom_frag, 2, NULL}};
        VkPipeline *outs[3];
        uint32_t k;
        outs[0] = &r->bloom_down_pipe;
        outs[1] = &r->bloom_up_pipe;
        outs[2] = &r->post_bloom_pipe;
        for (k = 0; k < 3u && ok; k++) {
            memset(&s, 0, sizeof s);
            s.vs = su_shader_fullscreen_vert;
            s.vs_bytes = sizeof su_shader_fullscreen_vert;
            s.fs = bl[k].fs;
            s.fs_bytes = bl[k].fs_bytes;
            s.vin = VIN_NONE;
            s.bloom = bl[k].kind;
            ok = make_pipe(r, &s, outs[k]);
        }
    }
    memset(&s, 0, sizeof s);
    s.vs = su_shader_hud_vert;
    s.vs_bytes = sizeof su_shader_hud_vert;
    s.fs = su_shader_hud_frag;
    s.fs_bytes = sizeof su_shader_hud_frag;
    s.vin = VIN_HUD;
    s.post = 1;
    s.blend = BLEND_ALPHA;
    ok = ok && make_pipe(r, &s, &r->hud_pipe);
    r->stats.pipeline_create_ms = su_rvk_now_ms() - t0;
    return ok;
}

/* ---------- descriptor layouts, pools, samplers ---------- */

static int make_layouts(SuRenderer *r, uint32_t material_sets) {
    VkDescriptorSetLayoutBinding b[SU_ASSET_SLOT_COUNT];
    VkDescriptorSetLayoutCreateInfo li;
    VkDescriptorSetLayout sets[2];
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli;
    VkDescriptorPoolSize ps[3];
    VkDescriptorPoolCreateInfo pi;
    uint32_t i;
    memset(b, 0, sizeof b);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; /* the sun's shadow map */
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = b;
    if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->frame_dsl) != VK_SUCCESS) {
        return 0;
    }
    memset(&b[1], 0, sizeof b[1]);
    for (i = 0; i < SU_ASSET_SLOT_COUNT; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = SU_ASSET_SLOT_COUNT;
    if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->mat_dsl) != VK_SUCCESS) {
        return 0;
    }
    li.bindingCount = 1;
    if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->tex_dsl) != VK_SUCCESS) {
        return 0;
    }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->depth_in_dsl) != VK_SUCCESS) {
        return 0;
    }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    {
        /* layered paint: eight samplers and the layer parameters; the environment cube */
        VkDescriptorSetLayoutBinding pb[SU_PAINT_SLOT_COUNT + 1u];
        VkDescriptorSetLayout psets[3];
        memset(pb, 0, sizeof pb);
        for (i = 0; i < SU_PAINT_SLOT_COUNT; i++) {
            pb[i].binding = i;
            pb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pb[i].descriptorCount = 1;
            pb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        pb[SU_PAINT_SLOT_COUNT].binding = SU_PAINT_SLOT_COUNT;
        pb[SU_PAINT_SLOT_COUNT].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pb[SU_PAINT_SLOT_COUNT].descriptorCount = 1;
        pb[SU_PAINT_SLOT_COUNT].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        li.bindingCount = SU_PAINT_SLOT_COUNT + 1u;
        li.pBindings = pb;
        if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->paint_dsl) != VK_SUCCESS) {
            return 0;
        }
        li.bindingCount = 1;
        li.pBindings = b; /* binding 0, one fragment sampler (b[0] was rewritten above) */
        if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->env_dsl) != VK_SUCCESS) {
            return 0;
        }
        memset(&pcr, 0, sizeof pcr);
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.size = sizeof(LegacyPush) > sizeof(MatPush) ? sizeof(LegacyPush) : sizeof(MatPush);
        psets[0] = r->frame_dsl;
        psets[1] = r->paint_dsl;
        psets[2] = r->env_dsl;
        memset(&pli, 0, sizeof pli);
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 3;
        pli.pSetLayouts = psets;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        if (r->f.vkCreatePipelineLayout(r->dev, &pli, NULL, &r->paint_layout) != VK_SUCCESS) {
            return 0;
        }
    }
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(LegacyPush) > sizeof(MatPush) ? sizeof(LegacyPush) : sizeof(MatPush);
    sets[0] = r->frame_dsl;
    sets[1] = r->mat_dsl;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (r->f.vkCreatePipelineLayout(r->dev, &pli, NULL, &r->scene_layout) != VK_SUCCESS) {
        return 0;
    }
    pcr.size = sizeof(PostPush);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r->tex_dsl;
    if (r->f.vkCreatePipelineLayout(r->dev, &pli, NULL, &r->post_layout) != VK_SUCCESS) {
        return 0;
    }
    {
        /* textured effects (frame + one sampler + scene depth, FxPush) and the cube sky (frame + cube + SkyPush) */
        VkDescriptorSetLayout fsets[3];
        VkPushConstantRange spc;
        VkPipelineLayoutCreateInfo fli;
        fsets[0] = r->frame_dsl;
        fsets[1] = r->tex_dsl;
        fsets[2] = r->depth_in_dsl;
        memset(&spc, 0, sizeof spc);
        spc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        spc.size = sizeof(FxPush);
        memset(&fli, 0, sizeof fli);
        fli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        fli.setLayoutCount = 3;
        fli.pSetLayouts = fsets;
        fli.pushConstantRangeCount = 1;
        fli.pPushConstantRanges = &spc;
        if (r->f.vkCreatePipelineLayout(r->dev, &fli, NULL, &r->fxtex_layout) != VK_SUCCESS) {
            return 0;
        }
        fli.setLayoutCount = 2;
        fli.pushConstantRangeCount = 0;
        fli.pPushConstantRanges = NULL;
        fsets[1] = r->env_dsl;
        memset(&spc, 0, sizeof spc);
        spc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        spc.size = sizeof(SkyPush);
        fli.pushConstantRangeCount = 1;
        fli.pPushConstantRanges = &spc;
        if (r->f.vkCreatePipelineLayout(r->dev, &fli, NULL, &r->skycube_layout) != VK_SUCCESS) {
            return 0;
        }
    }
    /* frame sets (one per slot), the output pass input and the font */
    /* bloom: output/upsample layout with three samplers, push constants for both bloom layouts, one pool for the
     * per-level sets (BLOOM_MAX_LEVELS down + up_a + up_b + the output set) */
    {
        VkDescriptorSetLayoutBinding b3[3];
        memset(b3, 0, sizeof b3);
        for (i = 0; i < 3u; i++) {
            b3[i].binding = i;
            b3[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b3[i].descriptorCount = 1;
            b3[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        li.bindingCount = 3;
        li.pBindings = b3;
        if (r->f.vkCreateDescriptorSetLayout(r->dev, &li, NULL, &r->tex3_dsl) != VK_SUCCESS) {
            return 0;
        }
        li.bindingCount = 1;
        li.pBindings = b;
        pcr.size = sizeof(BloomPush);
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &r->tex_dsl;
        if (r->f.vkCreatePipelineLayout(r->dev, &pli, NULL, &r->bloom_layout) != VK_SUCCESS) {
            return 0;
        }
        pli.pSetLayouts = &r->tex3_dsl;
        if (r->f.vkCreatePipelineLayout(r->dev, &pli, NULL, &r->bloom3_layout) != VK_SUCCESS) {
            return 0;
        }
        ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps[0].descriptorCount = BLOOM_MAX_LEVELS * 8u;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = BLOOM_MAX_LEVELS * 3u + 1u;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = ps;
        if (r->f.vkCreateDescriptorPool(r->dev, &pi, NULL, &r->pool_bloom) != VK_SUCCESS) {
            return 0;
        }
    }
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[0].descriptorCount = FRAMES_IN_FLIGHT;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = 2u + FRAMES_IN_FLIGHT; /* post input, font, a shadow map per frame set */
    ps[2].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    ps[2].descriptorCount = 1;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = FRAMES_IN_FLIGHT + 3u;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    if (r->f.vkCreateDescriptorPool(r->dev, &pi, NULL, &r->pool_frame) != VK_SUCCESS) {
        return 0;
    }
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[0].descriptorCount = material_sets * SU_PAINT_SLOT_COUNT;
    ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[1].descriptorCount = material_sets;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = material_sets;
    pi.poolSizeCount = 2;
    return r->f.vkCreateDescriptorPool(r->dev, &pi, NULL, &r->pool_mat) == VK_SUCCESS;
}

static int make_shadow_sampler(SuRenderer *r) {
    VkSamplerCreateInfo si;
    VkFormatProperties fp;
    r->f.vkGetPhysicalDeviceFormatProperties(r->pd, r->shadow_format, &fp);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    /* hardware 2 x 2 percentage-closer filtering when the depth format can be filtered linearly */
    si.magFilter = si.minFilter =
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; /* outside the map: depth 1, lit */
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    return r->f.vkCreateSampler(r->dev, &si, NULL, &r->shadow_sampler) == VK_SUCCESS;
}

static int make_samplers(SuRenderer *r) {
    static const VkSamplerAddressMode modes[3] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                  VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    VkSamplerCreateInfo si;
    uint32_t i;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (r->anisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = r->props.limits.maxSamplerAnisotropy < 8.0f ? r->props.limits.maxSamplerAnisotropy : 8.0f;
    }
    for (i = 0; i < 9u; i++) {
        si.addressModeU = modes[i / 3u];
        si.addressModeV = modes[i % 3u];
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (r->f.vkCreateSampler(r->dev, &si, NULL, &r->samplers[i]) != VK_SUCCESS) {
            return 0;
        }
    }
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (r->f.vkCreateSampler(r->dev, &si, NULL, &r->nearest) != VK_SUCCESS) {
        return 0;
    }
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    return r->f.vkCreateSampler(r->dev, &si, NULL, &r->bloom_sampler) == VK_SUCCESS;
}

static void write_image_set(SuRenderer *r, VkDescriptorSet set, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    ii.sampler = sampler;
    ii.imageView = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
}

/* The per-frame pool of sampler sets for textured effect batches (reset at the start of each frame). */
static int make_fx_pool(SuRenderer *r, Frame *fr) {
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo pi;
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = r->max_fx_batches;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = r->max_fx_batches;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    return r->f.vkCreateDescriptorPool(r->dev, &pi, NULL, &fr->fx_pool) == VK_SUCCESS;
}

static int make_frame_sets(SuRenderer *r) {
    VkDescriptorSetLayout l[FRAMES_IN_FLIGHT + 3u];
    VkDescriptorSet sets[FRAMES_IN_FLIGHT + 3u];
    VkDescriptorSetAllocateInfo ai;
    uint32_t i;
    for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
        l[i] = r->frame_dsl;
    }
    l[FRAMES_IN_FLIGHT] = r->tex_dsl;
    l[FRAMES_IN_FLIGHT + 1u] = r->tex_dsl;
    l[FRAMES_IN_FLIGHT + 2u] = r->depth_in_dsl;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = r->pool_frame;
    ai.descriptorSetCount = FRAMES_IN_FLIGHT + 3u;
    ai.pSetLayouts = l;
    if (r->f.vkAllocateDescriptorSets(r->dev, &ai, sets) != VK_SUCCESS) {
        return 0;
    }
    for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bi;
        VkWriteDescriptorSet w;
        r->frames[i].frame_set = sets[i];
        bi.buffer = r->frames[i].ubo.buf;
        bi.offset = 0;
        bi.range = sizeof(FrameUbo);
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
    }
    r->post_set = sets[FRAMES_IN_FLIGHT];
    r->font_set = sets[FRAMES_IN_FLIGHT + 1u];
    r->depth_in_set = sets[FRAMES_IN_FLIGHT + 2u];
    return 1;
}

/* ---------- built-in debug meshes (uploaded once at creation, outside any frame) ---------- */

static int upload_now(SuRenderer *r, const void *data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer *dst) {
    Buffer staging;
    VkCommandBuffer cmd = r->frames[0].cmd;
    VkCommandBufferBeginInfo bi;
    VkBufferCopy copy;
    VkSubmitInfo si;
    int ok;
    if (!su_rvk_make_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) {
        su_rvk_free_buffer(r, &staging);
        return 0;
    }
    memcpy(staging.map, data, (size_t)size);
    if (!su_rvk_make_buffer(r, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst)) {
        su_rvk_free_buffer(r, &staging);
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
    su_rvk_free_buffer(r, &staging);
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
    ok = ok && upload_now(r, verts, total_v * sizeof *verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &r->mesh_vb) &&
         upload_now(r, idx, total_i * sizeof *idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &r->mesh_ib);
    free(verts);
    free(idx);
    return ok;
}

/* ---------- targets ---------- */

static void destroy_bloom_targets(SuRenderer *r);

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
    destroy_bloom_targets(r);
    if (r->hdr_fb != VK_NULL_HANDLE) {
        r->f.vkDestroyFramebuffer(r->dev, r->hdr_fb, NULL);
        r->hdr_fb = VK_NULL_HANDLE;
    }
    free_image(r, &r->hdr_img, &r->hdr_mem, &r->hdr_view);
    free_image(r, &r->depth_img, &r->depth_mem, &r->depth_view);
    free_image(r, &r->color_img, &r->color_mem, &r->color_view);
    su_rvk_free_buffer(r, &r->readback);
}

static void destroy_bloom_targets(SuRenderer *r) {
    uint32_t c;
    uint32_t i;
    r->bloom_ok = 0;
    for (c = 0; c < 2u; c++) {
        for (i = 0; i < BLOOM_MAX_LEVELS; i++) {
            if (r->bl_fb[c][i] != VK_NULL_HANDLE) {
                r->f.vkDestroyFramebuffer(r->dev, r->bl_fb[c][i], NULL);
            }
            if (r->bl_view[c][i] != VK_NULL_HANDLE) {
                r->f.vkDestroyImageView(r->dev, r->bl_view[c][i], NULL);
            }
            r->bl_fb[c][i] = VK_NULL_HANDLE;
            r->bl_view[c][i] = VK_NULL_HANDLE;
        }
        if (r->bl_img[c] != VK_NULL_HANDLE) {
            r->f.vkDestroyImage(r->dev, r->bl_img[c], NULL);
        }
        if (r->bl_mem[c] != VK_NULL_HANDLE) {
            r->f.vkFreeMemory(r->dev, r->bl_mem[c], NULL);
        }
        r->bl_img[c] = VK_NULL_HANDLE;
        r->bl_mem[c] = VK_NULL_HANDLE;
    }
    if (r->pool_bloom != VK_NULL_HANDLE) {
        (void)r->f.vkResetDescriptorPool(r->dev, r->pool_bloom, 0);
    }
    r->bloom_count = 0;
}

static void write_sets(SuRenderer *r, VkDescriptorSet set, uint32_t n, const VkImageView *views, const VkSampler *samplers) {
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    uint32_t i;
    for (i = 0; i < n; i++) {
        ii[i].sampler = samplers[i];
        ii[i].imageView = views[i];
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w[i], 0, sizeof w[i]);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    r->f.vkUpdateDescriptorSets(r->dev, n, w, 0, NULL);
}

/* Bloom chains at half resolution. A has L levels (down to about 2 texels, for the auto exposure average), B has L - 1.
 * Not being able to build them (small target, no memory) only turns bloom off; the frame still renders. */
static void make_bloom_targets(SuRenderer *r) {
    uint32_t bw = r->width / 2u;
    uint32_t bh = r->height / 2u;
    uint32_t s = bw < bh ? bw : bh;
    uint32_t levels = 1;
    uint32_t c;
    uint32_t i;
    VkDescriptorSetLayout dl[BLOOM_MAX_LEVELS * 3u + 1u];
    VkDescriptorSet sets[BLOOM_MAX_LEVELS * 3u + 1u];
    VkDescriptorSetAllocateInfo ai;
    uint32_t nsets;
    r->bloom_ok = 0;
    if (!r->info.bloom || r->bloom_pass == VK_NULL_HANDLE || s < 4u) {
        return;
    }
    while (s > 2u && levels < BLOOM_MAX_LEVELS) {
        s >>= 1;
        levels++;
    }
    for (c = 0; c < 2u; c++) {
        VkImageCreateInfo ii;
        VkMemoryRequirements req;
        VkMemoryAllocateInfo ma;
        uint32_t type = 0;
        uint32_t n = c == 0u ? levels : levels - 1u;
        memset(&ii, 0, sizeof ii);
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = HDR_FORMAT;
        ii.extent.width = bw;
        ii.extent.height = bh;
        ii.extent.depth = 1;
        ii.mipLevels = n;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (r->f.vkCreateImage(r->dev, &ii, NULL, &r->bl_img[c]) != VK_SUCCESS) {
            goto fail;
        }
        r->f.vkGetImageMemoryRequirements(r->dev, r->bl_img[c], &req);
        if (!su_rvk_find_memory(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
            goto fail;
        }
        memset(&ma, 0, sizeof ma);
        ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ma.allocationSize = req.size;
        ma.memoryTypeIndex = type;
        if (r->f.vkAllocateMemory(r->dev, &ma, NULL, &r->bl_mem[c]) != VK_SUCCESS ||
            r->f.vkBindImageMemory(r->dev, r->bl_img[c], r->bl_mem[c], 0) != VK_SUCCESS) {
            goto fail;
        }
        for (i = 0; i < n; i++) {
            VkImageViewCreateInfo vi;
            VkFramebufferCreateInfo fi;
            memset(&vi, 0, sizeof vi);
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = r->bl_img[c];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = HDR_FORMAT;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel = i;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (r->f.vkCreateImageView(r->dev, &vi, NULL, &r->bl_view[c][i]) != VK_SUCCESS) {
                goto fail;
            }
            memset(&fi, 0, sizeof fi);
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = r->bloom_pass;
            fi.attachmentCount = 1;
            fi.pAttachments = &r->bl_view[c][i];
            fi.width = (bw >> i) > 0u ? (bw >> i) : 1u;
            fi.height = (bh >> i) > 0u ? (bh >> i) : 1u;
            fi.layers = 1;
            if (r->f.vkCreateFramebuffer(r->dev, &fi, NULL, &r->bl_fb[c][i]) != VK_SUCCESS) {
                goto fail;
            }
        }
    }
    /* sets: down[0..L-1] (one sampler), up_a[0..L-2], up_b[0..L-3] and the output set (three samplers) */
    nsets = levels + (levels - 1u) + (levels - 2u) + 1u;
    for (i = 0; i < nsets; i++) {
        dl[i] = i < levels ? r->tex_dsl : r->tex3_dsl;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = r->pool_bloom;
    ai.descriptorSetCount = nsets;
    ai.pSetLayouts = dl;
    if (r->f.vkAllocateDescriptorSets(r->dev, &ai, sets) != VK_SUCCESS) {
        goto fail;
    }
    {
        VkSampler lin[3];
        VkImageView v[3];
        uint32_t k = 0;
        lin[0] = lin[1] = lin[2] = r->bloom_sampler;
        for (i = 0; i < levels; i++) {
            r->bl_down_set[i] = sets[k++];
            v[0] = i == 0u ? r->hdr_view : r->bl_view[0][i - 1u];
            write_sets(r, r->bl_down_set[i], 1, v, lin);
        }
        for (i = 0; i + 1u < levels; i++) {
            r->bl_up_a[i] = sets[k++];
            v[0] = r->bl_view[0][i + 1u];
            v[1] = r->bl_view[0][i];
            write_sets(r, r->bl_up_a[i], 2, v, lin);
        }
        for (i = 0; i + 2u < levels; i++) {
            r->bl_up_b[i] = sets[k++];
            v[0] = r->bl_view[1][i + 1u];
            v[1] = r->bl_view[0][i];
            write_sets(r, r->bl_up_b[i], 2, v, lin);
        }
        r->bl_final_set = sets[k];
        v[0] = r->hdr_view;
        v[1] = r->bl_view[1][0];
        v[2] = r->bl_view[0][levels - 1u];
        lin[0] = r->nearest;
        write_sets(r, r->bl_final_set, 3, v, lin);
    }
    r->bloom_count = levels;
    r->bloom_ok = 1;
    return;
fail:
    destroy_bloom_targets(r);
}

/* HDR colour, depth, the scene framebuffer, the output framebuffers; the output pass input set. */
static int make_framebuffers(SuRenderer *r) {
    uint32_t i;
    VkImageView att[2];
    VkFramebufferCreateInfo fi;
    if (!make_image(r, r->width, r->height, r->depth_format,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &r->depth_img, &r->depth_mem, &r->depth_view) ||
        !make_image(r, r->width, r->height, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &r->hdr_img, &r->hdr_mem, &r->hdr_view)) {
        return 0;
    }
    att[0] = r->hdr_view;
    att[1] = r->depth_view;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = r->scene_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = att;
    fi.width = r->width;
    fi.height = r->height;
    fi.layers = 1;
    if (r->f.vkCreateFramebuffer(r->dev, &fi, NULL, &r->hdr_fb) != VK_SUCCESS) {
        return 0;
    }
    write_image_set(r, r->post_set, r->hdr_view, r->nearest);
    {
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet w;
        ii.sampler = VK_NULL_HANDLE;
        ii.imageView = r->depth_view;
        ii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = r->depth_in_set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        w.pImageInfo = &ii;
        r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
    }
    make_bloom_targets(r);
    for (i = 0; i < r->image_count; i++) {
        VkSemaphoreCreateInfo si;
        fi.renderPass = r->post_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &r->views[i];
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
    if (!su_rvk_make_buffer(r, (VkDeviceSize)r->width * r->height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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

/* ---------- create / destroy ---------- */

void su_render_destroy(SuRenderer *r) {
    uint32_t i;
    if (r == NULL) {
        return;
    }
    if (r->dev != VK_NULL_HANDLE) {
        (void)r->f.vkDeviceWaitIdle(r->dev);
        cache_save(r);
        su_rvk_res_shutdown(r);
        destroy_targets(r);
        if (r->swapchain != VK_NULL_HANDLE) {
            r->f.vkDestroySwapchainKHR(r->dev, r->swapchain, NULL);
        }
        for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
            Frame *fr = &r->frames[i];
            su_rvk_free_buffer(r, &fr->instances);
            su_rvk_free_buffer(r, &fr->lines);
            su_rvk_free_buffer(r, &fr->draw_inst);
            su_rvk_free_buffer(r, &fr->fx);
            su_rvk_free_buffer(r, &fr->fx_tex);
            su_rvk_free_buffer(r, &fr->shadow_inst);
            if (fr->fx_pool != VK_NULL_HANDLE) {
                r->f.vkDestroyDescriptorPool(r->dev, fr->fx_pool, NULL);
            }
            su_rvk_free_buffer(r, &fr->hud);
            su_rvk_free_buffer(r, &fr->ubo);
            su_rvk_free_buffer(r, &fr->staging);
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
        su_rvk_free_buffer(r, &r->mesh_vb);
        su_rvk_free_buffer(r, &r->mesh_ib);
        {
            VkPipeline all[10 + 2 * PIPE_PBR_COUNT];
            uint32_t k;
            all[0] = r->mesh_pipe;
            all[1] = r->line_pipe;
            all[2] = r->sky_pipe;
            all[3] = r->fx_pipe;
            all[4] = r->post_pipe;
            all[5] = r->hud_pipe;
            all[6] = r->bloom_down_pipe;
            all[7] = r->fxtex_pipe[0];
            all[8] = r->fxtex_pipe[1];
            all[9] = r->skycube_pipe;
            if (r->bloom_up_pipe != VK_NULL_HANDLE) {
                r->f.vkDestroyPipeline(r->dev, r->bloom_up_pipe, NULL);
            }
            if (r->post_bloom_pipe != VK_NULL_HANDLE) {
                r->f.vkDestroyPipeline(r->dev, r->post_bloom_pipe, NULL);
            }
            if (r->pool_bloom != VK_NULL_HANDLE) {
                r->f.vkDestroyDescriptorPool(r->dev, r->pool_bloom, NULL);
            }
            if (r->bloom3_layout != VK_NULL_HANDLE) {
                r->f.vkDestroyPipelineLayout(r->dev, r->bloom3_layout, NULL);
            }
            if (r->bloom_layout != VK_NULL_HANDLE) {
                r->f.vkDestroyPipelineLayout(r->dev, r->bloom_layout, NULL);
            }
            if (r->tex3_dsl != VK_NULL_HANDLE) {
                r->f.vkDestroyDescriptorSetLayout(r->dev, r->tex3_dsl, NULL);
            }
            if (r->bloom_pass != VK_NULL_HANDLE) {
                r->f.vkDestroyRenderPass(r->dev, r->bloom_pass, NULL);
            }
            if (r->bloom_sampler != VK_NULL_HANDLE) {
                r->f.vkDestroySampler(r->dev, r->bloom_sampler, NULL);
            }
            for (k = 0; k < PIPE_PBR_COUNT; k++) {
                all[10 + k] = r->pbr_pipe[k];
                all[10 + PIPE_PBR_COUNT + k] = r->paint_pipe[k];
            }
            for (k = 0; k < 10 + 2 * PIPE_PBR_COUNT; k++) {
                if (all[k] != VK_NULL_HANDLE) {
                    r->f.vkDestroyPipeline(r->dev, all[k], NULL);
                }
            }
        }
        for (i = 0; i < 9u; i++) {
            if (r->samplers[i] != VK_NULL_HANDLE) {
                r->f.vkDestroySampler(r->dev, r->samplers[i], NULL);
            }
        }
        if (r->nearest != VK_NULL_HANDLE) {
            r->f.vkDestroySampler(r->dev, r->nearest, NULL);
        }
        if (r->cache != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineCache(r->dev, r->cache, NULL);
        }
        if (r->pool_mat != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorPool(r->dev, r->pool_mat, NULL);
        }
        if (r->pool_frame != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorPool(r->dev, r->pool_frame, NULL);
        }
        if (r->fxtex_layout != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineLayout(r->dev, r->fxtex_layout, NULL);
        }
        if (r->depth_in_dsl != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorSetLayout(r->dev, r->depth_in_dsl, NULL);
        }
        free_shadow_map(r);
        if (r->shadow_pipe != VK_NULL_HANDLE) {
            r->f.vkDestroyPipeline(r->dev, r->shadow_pipe, NULL);
        }
        if (r->shadow_pass != VK_NULL_HANDLE) {
            r->f.vkDestroyRenderPass(r->dev, r->shadow_pass, NULL);
        }
        if (r->shadow_sampler != VK_NULL_HANDLE) {
            r->f.vkDestroySampler(r->dev, r->shadow_sampler, NULL);
        }
        if (r->skycube_layout != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineLayout(r->dev, r->skycube_layout, NULL);
        }
        if (r->scene_layout != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineLayout(r->dev, r->scene_layout, NULL);
        }
        if (r->paint_layout != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineLayout(r->dev, r->paint_layout, NULL);
        }
        if (r->paint_dsl != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorSetLayout(r->dev, r->paint_dsl, NULL);
        }
        if (r->env_dsl != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorSetLayout(r->dev, r->env_dsl, NULL);
        }
        if (r->post_layout != VK_NULL_HANDLE) {
            r->f.vkDestroyPipelineLayout(r->dev, r->post_layout, NULL);
        }
        if (r->frame_dsl != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorSetLayout(r->dev, r->frame_dsl, NULL);
        }
        if (r->mat_dsl != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorSetLayout(r->dev, r->mat_dsl, NULL);
        }
        if (r->tex_dsl != VK_NULL_HANDLE) {
            r->f.vkDestroyDescriptorSetLayout(r->dev, r->tex_dsl, NULL);
        }
        if (r->scene_pass != VK_NULL_HANDLE) {
            r->f.vkDestroyRenderPass(r->dev, r->scene_pass, NULL);
        }
        if (r->post_pass != VK_NULL_HANDLE) {
            r->f.vkDestroyRenderPass(r->dev, r->post_pass, NULL);
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
    free(r->items);
    free(r->order);
    free(r->fxb);
    free(r->fxs);
    free(r->fxq);
    free(r->sitems);
    free(r->sorder);
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

#define DESC_FIELD(d, field, dflt) ((d)->struct_size == sizeof(SuRenderDesc) && (d)->field != 0u ? (d)->field : (dflt))

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
    VkPhysicalDeviceFeatures have;
    VkPhysicalDeviceFeatures want;
    const char *dev_exts[1];
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cai;
    VkPipelineCacheCreateInfo cci;
    uint8_t *cache_data = NULL;
    uint32_t max_res;
    VkResult vr;

    if (message != NULL) {
        message[0] = '\0';
    }
    if (out == NULL || desc == NULL || (desc->struct_size != sizeof *desc && desc->struct_size != SU_RENDER_DESC_V1_SIZE) ||
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
    r->budget = DESC_FIELD(desc, upload_budget_bytes, DEFAULT_UPLOAD_BUDGET);
    r->max_draw = DESC_FIELD(desc, max_draw_instances, DEFAULT_DRAW_INSTANCES);
    r->max_fx = DESC_FIELD(desc, max_fx_vertices, DEFAULT_FX_VERTICES);
    r->max_hud = DESC_FIELD(desc, max_hud_vertices, DEFAULT_HUD_VERTICES);
    r->max_fx_tex = DESC_FIELD(desc, max_fx_tex_vertices, DEFAULT_FX_TEX_VERTICES);
    r->max_fx_batches = DESC_FIELD(desc, max_fx_batches, DEFAULT_FX_BATCHES);
    r->fxb_cap = r->max_fx_batches + r->max_fx_tex / 6u + 1u;
    r->fxb = malloc((size_t)r->fxb_cap * sizeof *r->fxb);
    r->fxs = malloc((size_t)r->max_fx_batches * sizeof *r->fxs);
    r->fxq = malloc(((size_t)r->max_fx_tex / 6u + 1u) * sizeof *r->fxq);
    max_res = DESC_FIELD(desc, max_resources, DEFAULT_RESOURCES);
    if (desc->struct_size == sizeof *desc && desc->pipeline_cache_path != NULL) {
        snprintf(r->cache_path, sizeof r->cache_path, "%s", desc->pipeline_cache_path);
    }
    r->stats.gpu_ms = -1.0;
    r->stats.present_mode = UINT32_MAX;
    r->sorted = malloc((size_t)r->max_instances * sizeof *r->sorted);
    r->items = malloc((size_t)r->max_draw * sizeof *r->items);
    r->order = malloc((size_t)r->max_draw * sizeof *r->order);
    r->sitems = malloc((size_t)r->max_draw * sizeof *r->sitems);
    r->sorder = malloc((size_t)r->max_draw * sizeof *r->sorder);
    if (r->sorted == NULL || r->items == NULL || r->order == NULL || r->fxb == NULL || r->fxs == NULL || r->fxq == NULL ||
        r->sitems == NULL || r->sorder == NULL) {
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
    r->f.vkGetPhysicalDeviceProperties(r->pd, &r->props);
    r->f.vkGetPhysicalDeviceFeatures(r->pd, &have);
    r->depth_format = pick_depth(&r->f, r->pd);
    r->shadow_format = pick_shadow_depth(&r->f, r->pd);
    memset(&want, 0, sizeof want);
    r->anisotropy = have.samplerAnisotropy == VK_TRUE;
    r->bc = have.textureCompressionBC == VK_TRUE;
    want.samplerAnisotropy = have.samplerAnisotropy;
    want.textureCompressionBC = have.textureCompressionBC;
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
    di.pEnabledFeatures = &want;
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
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
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
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_instances * sizeof(SuRenderInstance),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, &fr->instances) ||
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_lines * sizeof(SuRenderLineVertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, &fr->lines) ||
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_draw * sizeof(DrawInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                host, &fr->draw_inst) ||
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_fx * sizeof(SuFxVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host,
                                &fr->fx) ||
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_draw * sizeof(DrawInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                host, &fr->shadow_inst) ||
            /* two halves: the frame's vertices as given, then the sorted quads */
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_fx_tex * 2u * sizeof(SuFxVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                host, &fr->fx_tex) ||
            !make_fx_pool(r, fr) ||
            !su_rvk_make_buffer(r, (VkDeviceSize)r->max_hud * sizeof(SuHudVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                host, &fr->hud) ||
            !su_rvk_make_buffer(r, sizeof(FrameUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, &fr->ubo) ||
            !su_rvk_make_buffer(r, r->budget, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, &fr->staging)) {
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
    memset(&cci, 0, sizeof cci);
    cci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (r->cache_path[0] != '\0') {
        cache_load(r, r->cache_path, &cci, &cache_data);
    }
    vr = r->f.vkCreatePipelineCache(r->dev, &cci, NULL, &r->cache);
    if (vr != VK_SUCCESS && cache_data != NULL) {
        /* the driver refused the stored data: start empty */
        cci.initialDataSize = 0;
        cci.pInitialData = NULL;
        r->stats.pipeline_cache = 2u;
        vr = r->f.vkCreatePipelineCache(r->dev, &cci, NULL, &r->cache);
    }
    free(cache_data);
    if (vr != VK_SUCCESS) {
        r->cache = VK_NULL_HANDLE;
    }
    if (r->shadow_format == VK_FORMAT_UNDEFINED || !make_layouts(r, max_res + 64u) || !make_samplers(r) ||
        !make_shadow_sampler(r) || !make_scene_pass(r) || !make_shadow_pass(r) || !make_post_pass(r) ||
        !make_bloom_pass(r) || !make_pipelines(r) || !make_frame_sets(r) || !make_shadow_map(r, 1u, 1u)) {
        return create_fail(r, SU_E_UNSUPPORTED_METHOD, message, "render passes, layouts or pipelines could not be created");
    }
    if (!make_meshes(r)) {
        return create_fail(r, SU_E_RESOURCE_LIMIT, message, "mesh upload failed");
    }
    if (su_rvk_res_init(r, max_res) != SU_OK || su_rvk_internal_textures(r) != SU_OK) {
        return create_fail(r, SU_E_RESOURCE_LIMIT, message, "resource tables or built-in textures: %s", r->last_error);
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

/* ---------- frames ---------- */

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

/* What one frame draws, from either entry point. */
typedef struct FrameIn {
    FrameUbo ubo;
    SuMat4f debug_vp;          /* debug meshes and lines (reversed Z) */
    float light[4];            /* debug meshes: light direction, ambient */
    int sky;
    float clear[4];
    float exposure;
    uint32_t tonemap;
    SuRenderPost post;
    const SuRenderInstance *inst;
    uint32_t inst_count;
    const SuRenderLineVertex *lines;
    uint32_t line_count;
    const SuFxVertex *fx;
    uint32_t fx_count;
    const SuHudVertex *hud;
    uint32_t hud_count;
    const SuFxVertex *fx_tex;
    uint32_t fx_tex_count;
    const SuFxBatch *batches;
    uint32_t batch_count;
    SuRenderHandle sky_cube;
    float sky_intensity;
    float sky_lod;
    SuQuatf sky_rot;
    const SuRenderFrameDesc *scene;
    SuMat4f scene_vp;
    float proj_y;
    uint32_t shadow_size;     /* > 0: sun shadows requested at this map size */
    uint32_t shadow_layers;   /* with it: cascades (1 for the single map) */
} FrameIn;

/* Farther quads first; equal distances keep submission order. */
static int cmp_fx_quads(const void *a, const void *b) {
    const FxQuad *x = (const FxQuad *)a;
    const FxQuad *y = (const FxQuad *)b;
    if (x->d2 != y->d2) {
        return x->d2 > y->d2 ? -1 : 1;
    }
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void viewport(SuRenderer *r, VkCommandBuffer cmd) {
    VkViewport vpt;
    VkRect2D sc;
    memset(&vpt, 0, sizeof vpt);
    vpt.width = (float)r->width;
    vpt.height = (float)r->height;
    vpt.maxDepth = 1.0f;
    memset(&sc, 0, sizeof sc);
    sc.extent.width = r->width;
    sc.extent.height = r->height;
    r->f.vkCmdSetViewport(cmd, 0, 1, &vpt);
    r->f.vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void push_legacy(SuRenderer *r, VkCommandBuffer cmd, const FrameIn *in) {
    LegacyPush pc;
    memcpy(pc.view_proj, in->debug_vp.m, sizeof pc.view_proj);
    memcpy(pc.light_dir_ambient, in->light, sizeof pc.light_dir_ambient);
    r->f.vkCmdPushConstants(cmd, r->scene_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc,
                            &pc);
}

static uint32_t render(SuRenderer *r, const FrameIn *in) {
    Frame *fr;
    double t0;
    double tw;
    double tp = 0.0;
    double ts0;
    uint32_t image = 0;
    uint32_t counts[SU_MESH_COUNT];
    uint32_t starts[SU_MESH_COUNT];
    uint32_t n_inst;
    uint32_t n_lines;
    uint32_t n_fx;
    uint32_t n_hud;
    uint32_t n_fxt = 0;
    uint32_t n_fxb = 0;
    uint32_t n_batches = 0;
    uint32_t n_sorted = 0;
    uint32_t n_quads = 0;
    uint32_t n_sort_draws = 0;
    uint32_t sort_at = UINT32_MAX;
    FxPush fxp;
    uint32_t skipped = 0;
    VkDescriptorSet sky_set = VK_NULL_HANDLE;
    uint32_t n_items = 0;
    uint32_t i;
    uint32_t m;
    VkResult vr;
    VkCommandBufferBeginInfo bi;
    VkRenderPassBeginInfo rp;
    VkClearValue clear[2];
    VkSubmitInfo si;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkDeviceSize zero = 0;
    VkCommandBuffer cmd;

    t0 = su_rvk_now_ms();
    if (in->shadow_size != 0u && (in->shadow_size != r->shadow_size || in->shadow_layers != r->shadow_layers)) {
        /* first use or a new size: no submitted frame may still sample the old map */
        (void)r->f.vkDeviceWaitIdle(r->dev);
        if (!make_shadow_map(r, in->shadow_size, in->shadow_layers)) {
            (void)make_shadow_map(r, 1u, 1u);
            return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "device memory for a %u x %u shadow map", (unsigned)in->shadow_size,
                               (unsigned)in->shadow_size);
        }
    }
    if (!r->offscreen && (r->needs_rebuild || r->swapchain == VK_NULL_HANDLE)) {
        int s = make_swapchain(r);
        if (s < 0) {
            return SU_E_RESOURCE_LIMIT;
        }
        if (s == 0) {
            return su_rvk_fail(r, SU_E_IO, "the swapchain could not be recreated");
        }
        r->stats.swapchain_recreations++;
    }
    fr = &r->frames[r->frame_slot];
    cmd = fr->cmd;
    tw = su_rvk_now_ms();
    (void)r->f.vkWaitForFences(r->dev, 1, &fr->fence, VK_TRUE, UINT64_MAX);
    if (fr->serial > r->completed) {
        r->completed = fr->serial;
    }
    collect_gpu_time(r, fr);
    if (!r->offscreen) {
        vr = r->f.vkAcquireNextImageKHR(r->dev, r->swapchain, UINT64_MAX, fr->acquired, VK_NULL_HANDLE, &image);
        if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
            r->needs_rebuild = 1;
            return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "swapchain out of date; recreated on the next frame");
        }
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            return su_rvk_fail(r, SU_E_IO, "vkAcquireNextImageKHR failed (VkResult %d)", (int)vr);
        }
        if (vr == VK_SUBOPTIMAL_KHR) {
            r->needs_rebuild = 1;
        }
    }
    r->stats.wait_ms = su_rvk_now_ms() - tw;
    (void)r->f.vkResetFences(r->dev, 1, &fr->fence);
    su_rvk_collect_retired(r);
    (void)r->f.vkResetDescriptorPool(r->dev, fr->fx_pool, 0);

    (void)r->f.vkResetCommandBuffer(cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    (void)r->f.vkBeginCommandBuffer(cmd, &bi);
    if (fr->queries != VK_NULL_HANDLE) {
        r->f.vkCmdResetQueryPool(cmd, fr->queries, 0, 2);
        r->f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, fr->queries, 0);
    }
    su_rvk_record_uploads(r, fr, cmd);
    su_rvk_refresh_materials(r);
    n_fxt = in->fx_tex_count < r->max_fx_tex ? in->fx_tex_count : r->max_fx_tex;
    n_fxt -= n_fxt % 3u;
    if (n_fxt > 0u) {
        memcpy(fr->fx_tex.map, in->fx_tex, (size_t)n_fxt * sizeof *in->fx_tex);
    }
    for (i = 0; i < in->batch_count; i++) {
        const SuFxBatch *bt = &in->batches[i];
        Res *tr = su_rvk_get(r, bt->texture, RES_TEXTURE, NULL);
        FxBatchDraw tmpl;
        uint32_t first = bt->first_vertex;
        uint32_t count = bt->vertex_count;
        VkDescriptorSetAllocateInfo dai;
        VkDescriptorSet set = VK_NULL_HANDLE;
        static const uint32_t wrap_index[3] = {1u, 0u, 2u}; /* SU_FX_WRAP_* to the sampler table (repeat, clamp, mirror) */
        if (count == 0u) {
            continue;
        }
        if (first >= n_fxt) {
            count = 0;
        } else if (count > n_fxt - first) {
            count = n_fxt - first;
        }
        count -= count % 3u;
        if (tr == NULL || tr->state != SU_RENDER_STATE_READY || tr->u.tex.layers != 1u || count == 0u || first % 3u != 0u ||
            bt->blend > SU_FX_BLEND_PREMUL || n_batches >= r->max_fx_batches) {
            skipped++;
            continue;
        }
        memset(&dai, 0, sizeof dai);
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = fr->fx_pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &r->tex_dsl;
        if (r->f.vkAllocateDescriptorSets(r->dev, &dai, &set) != VK_SUCCESS) {
            skipped++;
            continue;
        }
        write_image_set(r, set, tr->u.tex.view,
                        r->samplers[wrap_index[bt->wrap_u > 2u ? 0u : bt->wrap_u] * 3u +
                                    wrap_index[bt->wrap_v > 2u ? 0u : bt->wrap_v]]);
        tmpl.set = set;
        tmpl.first = first;
        tmpl.count = count;
        tmpl.pipe = bt->blend;
        tmpl.soft = bt->soft_distance > 0.0f && bt->soft_distance < 1e30f ? 1.0f / bt->soft_distance : 0.0f;
        n_batches++;
        if ((bt->flags & SU_FX_BATCH_SORT) != 0u && count % 6u == 0u && n_quads + count / 6u <= r->max_fx_tex / 6u &&
            n_sorted < r->max_fx_batches) {
            /* collect the quads; drawn later, sorted, where the first sorted batch stands */
            uint32_t q;
            if (sort_at == UINT32_MAX) {
                sort_at = n_fxb;
            }
            r->fxs[n_sorted] = tmpl;
            for (q = 0; q < count / 6u; q++) {
                const SuFxVertex *v = in->fx_tex + first + q * 6u;
                float c[3] = {0.0f, 0.0f, 0.0f};
                uint32_t k;
                for (k = 0; k < 6u; k++) {
                    c[0] += v[k].pos[0];
                    c[1] += v[k].pos[1];
                    c[2] += v[k].pos[2];
                }
                r->fxq[n_quads].d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
                r->fxq[n_quads].src = n_sorted;
                r->fxq[n_quads].first = first + q * 6u;
                r->fxq[n_quads].seq = n_quads;
                n_quads++;
            }
            n_sorted++;
            continue;
        }
        r->fxb[n_fxb++] = tmpl;
    }
    if (n_quads > 0u) {
        /* back to front, written into the second half of the vertex buffer, one draw per run of the same batch */
        SuFxVertex *dst = (SuFxVertex *)fr->fx_tex.map + r->max_fx_tex;
        uint32_t runs = 0;
        uint32_t q;
        qsort(r->fxq, n_quads, sizeof *r->fxq, cmp_fx_quads);
        for (q = 0; q < n_quads; q++) {
            memcpy(dst + (size_t)q * 6u, in->fx_tex + r->fxq[q].first, 6u * sizeof *dst);
            if (q == 0u || r->fxq[q].src != r->fxq[q - 1u].src) {
                runs++;
            }
        }
        memmove(&r->fxb[sort_at + runs], &r->fxb[sort_at], (size_t)(n_fxb - sort_at) * sizeof *r->fxb);
        runs = 0;
        for (q = 0; q < n_quads; q++) {
            if (q == 0u || r->fxq[q].src != r->fxq[q - 1u].src) {
                r->fxb[sort_at + runs] = r->fxs[r->fxq[q].src];
                r->fxb[sort_at + runs].first = r->max_fx_tex + q * 6u;
                r->fxb[sort_at + runs].count = 0;
                runs++;
            }
            r->fxb[sort_at + runs - 1u].count += 6u;
        }
        n_fxb += runs;
        n_sort_draws = runs;
    }
    if (in->sky && in->sky_cube != 0u) {
        Res *cr = su_rvk_get(r, in->sky_cube, RES_TEXTURE, NULL);
        if (cr != NULL && cr->state == SU_RENDER_STATE_READY && cr->u.tex.layers == 6u && cr->u.tex.env_set != VK_NULL_HANDLE) {
            sky_set = cr->u.tex.env_set;
        }
    }
    {
        /* the environment: a READY cube from the frame description, else the built-in black one (params.z = 0) */
        FrameUbo u = in->ubo;
        float lod = 0.0f;
        r->frame_env_set = su_rvk_env_set(r, in->scene != NULL ? in->scene->env : 0u,
                                          in->scene != NULL ? in->scene->env_intensity : 0.0f, &lod, &u.params[2]);
        u.params[3] = lod;
        memcpy(fr->ubo.map, &u, sizeof u);
    }

    /* debug instances, grouped by mesh (counting sort; stable within a mesh) */
    n_inst = in->inst_count < r->max_instances ? in->inst_count : r->max_instances;
    memset(counts, 0, sizeof counts);
    for (i = 0; i < n_inst; i++) {
        counts[in->inst[i].mesh < SU_MESH_COUNT ? in->inst[i].mesh : SU_MESH_BOX]++;
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
            uint32_t mm = in->inst[i].mesh < SU_MESH_COUNT ? in->inst[i].mesh : SU_MESH_BOX;
            dst[fill[mm]++] = in->inst[i];
        }
    }
    n_lines = in->line_count < r->max_lines ? in->line_count & ~1u : r->max_lines & ~1u;
    if (n_lines > 0u) {
        memcpy(fr->lines.map, in->lines, (size_t)n_lines * sizeof *in->lines);
    }
    n_fx = in->fx_count < r->max_fx ? in->fx_count : r->max_fx;
    n_fx -= n_fx % 3u;
    if (n_fx > 0u) {
        memcpy(fr->fx.map, in->fx, (size_t)n_fx * sizeof *in->fx);
    }
    n_hud = in->hud_count < r->max_hud ? in->hud_count : r->max_hud;
    n_hud -= n_hud % 3u;
    if (n_hud > 0u) {
        memcpy(fr->hud.map, in->hud, (size_t)n_hud * sizeof *in->hud);
    }
    r->stats.dropped = (in->inst_count - n_inst) + (in->line_count - n_lines) + (in->fx_count - n_fx) +
                       (in->hud_count - n_hud);
    ts0 = su_rvk_now_ms();
    if (in->scene != NULL) {
        su_rvk_scene_build(r, fr, in->scene, &in->scene_vp, in->proj_y, &n_items);
    } else {
        r->stats.objects = r->stats.objects_culled = r->stats.nodes_drawn = r->stats.nodes_culled = 0;
        r->stats.nodes_pending = r->stats.draw_instances = 0;
        memset(r->stats.lod_nodes, 0, sizeof r->stats.lod_nodes);
    }
    r->stats.scene_cpu_ms = su_rvk_now_ms() - ts0;

    /* ---- sun shadow pass (before the scene; also once to give a fresh map its sampled layout) ---- */
    {
        uint32_t n_cast = 0;
        FrameUbo *u = (FrameUbo *)fr->ubo.map;
        uint32_t layer;
        memset(u->shadow_vp, 0, sizeof u->shadow_vp);
        memset(u->shadow_params, 0, sizeof u->shadow_params);
        memset(u->shadow_split, 0, sizeof u->shadow_split);
        memset(u->shadow_bias, 0, sizeof u->shadow_bias);
        memset(u->shadow_noff, 0, sizeof u->shadow_noff);
        memset(u->shadow_fwd, 0, sizeof u->shadow_fwd);
        r->stats.shadow_size = 0;
        r->stats.shadow_cascades = 0;
        r->stats.shadow_casters = r->stats.shadow_draws = 0;
        r->stats.shadow_texel = 0.0f;
        memset(r->stats.shadow_cascade_texel, 0, sizeof r->stats.shadow_cascade_texel);
        memset(r->shadow_count, 0, sizeof r->shadow_count);
        if (in->shadow_size != 0u && in->scene != NULL) {
            n_cast = su_rvk_shadow_build(r, fr, in->scene, &in->scene_vp, in->proj_y, u);
            r->stats.shadow_size = r->shadow_size;
            r->stats.shadow_cascades = r->shadow_layers;
        }
        for (layer = 0; layer < r->shadow_layers && (in->shadow_size != 0u || r->shadow_fresh); layer++) {
            VkClearValue sc;
            VkViewport svp;
            VkRect2D ssc;
            memset(&sc, 0, sizeof sc);
            sc.depthStencil.depth = 1.0f;
            memset(&rp, 0, sizeof rp);
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = r->shadow_pass;
            rp.framebuffer = r->shadow_fbs[layer];
            rp.renderArea.extent.width = r->shadow_size;
            rp.renderArea.extent.height = r->shadow_size;
            rp.clearValueCount = 1;
            rp.pClearValues = &sc;
            r->f.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            if (n_cast > 0u && r->shadow_count[layer] > 0u) {
                uint32_t idx = layer;
                memset(&svp, 0, sizeof svp);
                svp.width = svp.height = (float)r->shadow_size;
                svp.maxDepth = 1.0f;
                memset(&ssc, 0, sizeof ssc);
                ssc.extent.width = ssc.extent.height = r->shadow_size;
                r->f.vkCmdSetViewport(cmd, 0, 1, &svp);
                r->f.vkCmdSetScissor(cmd, 0, 1, &ssc);
                r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shadow_pipe);
                r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->scene_layout, 0, 1, &fr->frame_set, 0,
                                             NULL);
                r->f.vkCmdPushConstants(cmd, r->scene_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof idx, &idx);
                su_rvk_shadow_draw(r, fr, cmd, r->shadow_first[layer], r->shadow_count[layer]);
            }
            r->f.vkCmdEndRenderPass(cmd);
        }
        r->shadow_fresh = 0;
    }

    /* ---- scene pass (HDR) ---- */
    memset(clear, 0, sizeof clear);
    memcpy(clear[0].color.float32, in->clear, sizeof in->clear);
    clear[1].depthStencil.depth = 0.0f; /* reversed Z: far is 0 */
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = r->scene_pass;
    rp.framebuffer = r->hdr_fb;
    rp.renderArea.extent.width = r->width;
    rp.renderArea.extent.height = r->height;
    rp.clearValueCount = 2;
    rp.pClearValues = clear;
    r->f.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    viewport(r, cmd);
    r->stats.draws = 0;
    r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->scene_layout, 0, 1, &fr->frame_set, 0, NULL);
    if (in->sky && sky_set != VK_NULL_HANDLE) {
        SkyPush sp;
        SuQuatf q = in->sky_rot;
        SuMat4f rm;
        VkDescriptorSet two[2];
        uint32_t k;
        if (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f) {
            q.w = 1.0f;
        } else {
            float ql = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q.x /= ql;
            q.y /= ql;
            q.z /= ql;
            q.w /= ql;
        }
        q.x = -q.x; /* world to cube: the inverse rotation */
        q.y = -q.y;
        q.z = -q.z;
        rm = su_mat4f_from_quat(q);
        memset(&sp, 0, sizeof sp);
        for (k = 0; k < 3u; k++) { /* rows of the matrix: element (row k, column c) is m[c * 4 + k] */
            sp.r[k][0] = rm.m[0 * 4 + k];
            sp.r[k][1] = rm.m[1 * 4 + k];
            sp.r[k][2] = rm.m[2 * 4 + k];
        }
        sp.p[0] = in->sky_intensity > 0.0f ? in->sky_intensity : 1.0f;
        sp.p[1] = in->sky_lod > 0.0f ? in->sky_lod : 0.0f;
        two[0] = fr->frame_set;
        two[1] = sky_set;
        r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->skycube_pipe);
        r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->skycube_layout, 0, 2, two, 0, NULL);
        r->f.vkCmdPushConstants(cmd, r->skycube_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof sp, &sp);
        r->f.vkCmdDraw(cmd, 3, 1, 0, 0);
        r->stats.draws++;
        /* the scene layout's push constant range differs: bind its frame set again */
        r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->scene_layout, 0, 1, &fr->frame_set, 0, NULL);
    } else if (in->sky) {
        r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->sky_pipe);
        r->f.vkCmdDraw(cmd, 3, 1, 0, 0);
        r->stats.draws++;
    }
    if (n_items > 0u) {
        su_rvk_scene_draw(r, fr, cmd, n_items, 0);
    }
    if (n_inst > 0u) {
        VkBuffer vbs[2];
        VkDeviceSize offs[2] = {0, 0};
        vbs[0] = r->mesh_vb.buf;
        vbs[1] = fr->instances.buf;
        r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->mesh_pipe);
        push_legacy(r, cmd, in);
        r->f.vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        r->f.vkCmdBindIndexBuffer(cmd, r->mesh_ib.buf, 0, VK_INDEX_TYPE_UINT16);
        for (m = 0; m < SU_MESH_COUNT; m++) {
            if (counts[m] > 0u) {
                r->f.vkCmdDrawIndexed(cmd, r->mesh_index_count[m], counts[m], r->mesh_first_index[m],
                                      r->mesh_vertex_offset[m], starts[m]);
                r->stats.draws++;
            }
        }
    }
    if (n_lines > 0u) {
        r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->line_pipe);
        push_legacy(r, cmd, in);
        r->f.vkCmdBindVertexBuffers(cmd, 0, 1, &fr->lines.buf, &zero);
        r->f.vkCmdDraw(cmd, n_lines, 1, 0, 0);
        r->stats.draws++;
    }
    if (n_items > 0u) {
        su_rvk_scene_draw(r, fr, cmd, n_items, 1);
    }
    if (n_fx > 0u) {
        r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->fx_pipe);
        r->f.vkCmdBindVertexBuffers(cmd, 0, 1, &fr->fx.buf, &zero);
        r->f.vkCmdDraw(cmd, n_fx, 1, 0, 0);
        r->stats.draws++;
    }
    /* subpass 1: textured effects, with the scene depth readable for soft particles */
    r->f.vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
    if (n_fxb > 0u) {
        uint32_t last_pipe = UINT32_MAX;
        memset(&fxp, 0, sizeof fxp);
        fxp.near_z = in->scene != NULL && in->scene->camera.near_z > 0.0f ? in->scene->camera.near_z : 1.0f;
        r->f.vkCmdBindVertexBuffers(cmd, 0, 1, &fr->fx_tex.buf, &zero);
        for (i = 0; i < n_fxb; i++) {
            VkDescriptorSet three[3];
            if (r->fxb[i].pipe != last_pipe) {
                r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->fxtex_pipe[r->fxb[i].pipe]);
                last_pipe = r->fxb[i].pipe;
            }
            three[0] = fr->frame_set;
            three[1] = r->fxb[i].set;
            three[2] = r->depth_in_set;
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->fxtex_layout, 0, 3, three, 0, NULL);
            fxp.soft = r->fxb[i].soft;
            r->f.vkCmdPushConstants(cmd, r->fxtex_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof fxp, &fxp);
            r->f.vkCmdDraw(cmd, r->fxb[i].count, 1, r->fxb[i].first, 0);
            r->stats.draws++;
        }
    }
    r->f.vkCmdEndRenderPass(cmd);

    /* ---- bloom chain (half resolution), when asked for and available ---- */
    r->stats.bloom_levels_active = 0;
    r->stats.post_passes = 0;
    if (r->bloom_ok && (in->post.flags & (SU_POST_BLOOM | SU_POST_AUTO_EXPOSURE)) != 0u) {
        uint32_t want = (in->post.flags & SU_POST_BLOOM) != 0u ? (in->post.bloom_levels != 0u ? in->post.bloom_levels : 6u) : 2u;
        uint32_t lv;
        uint32_t k;
        float thr = in->post.bloom_threshold > 0.0f ? in->post.bloom_threshold : 1.0f;
        float knee = in->post.bloom_knee < 0.0f ? 0.0f : (in->post.bloom_knee > 1.0f ? 1.0f : in->post.bloom_knee);
        float radius = in->post.bloom_radius > 0.0f ? in->post.bloom_radius : 1.0f;
        VkClearValue none;
        memset(&none, 0, sizeof none);
        want = want < 2u ? 2u : (want > 8u ? 8u : want);
        lv = want < r->bloom_count ? want : r->bloom_count;
        r->stats.bloom_levels_active = lv;
        for (k = 0; k < r->bloom_count + lv - 1u; k++) {
            /* k < bloom_count: downsample level k; then upsample levels lv - 2 down to 0 */
            int down = k < r->bloom_count;
            uint32_t lvl = down ? k : lv - 2u - (k - r->bloom_count);
            uint32_t bw = (r->width / 2u) >> lvl;
            uint32_t bh = (r->height / 2u) >> lvl;
            BloomPush bp;
            VkViewport vpt;
            VkRect2D sc;
            VkDescriptorSet set;
            bw = bw > 0u ? bw : 1u;
            bh = bh > 0u ? bh : 1u;
            memset(&bp, 0, sizeof bp);
            bp.b[0] = 1.0f / (float)bw;
            bp.b[1] = 1.0f / (float)bh;
            if (down) {
                bp.a[0] = thr;
                bp.a[1] = thr * knee;
                bp.a[2] = lvl == 0u ? 1.0f : 0.0f;
                set = r->bl_down_set[lvl];
            } else {
                bp.a[0] = radius;
                set = (lvl + 2u == lv) ? r->bl_up_a[lvl] : r->bl_up_b[lvl];
            }
            memset(&rp, 0, sizeof rp);
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = r->bloom_pass;
            rp.framebuffer = r->bl_fb[down ? 0 : 1][lvl];
            rp.renderArea.extent.width = bw;
            rp.renderArea.extent.height = bh;
            rp.clearValueCount = 0;
            rp.pClearValues = &none;
            r->f.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            memset(&vpt, 0, sizeof vpt);
            vpt.width = (float)bw;
            vpt.height = (float)bh;
            vpt.maxDepth = 1.0f;
            memset(&sc, 0, sizeof sc);
            sc.extent.width = bw;
            sc.extent.height = bh;
            r->f.vkCmdSetViewport(cmd, 0, 1, &vpt);
            r->f.vkCmdSetScissor(cmd, 0, 1, &sc);
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, down ? r->bloom_down_pipe : r->bloom_up_pipe);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, down ? r->bloom_layout : r->bloom3_layout, 0, 1,
                                         &set, 0, NULL);
            r->f.vkCmdPushConstants(cmd, down ? r->bloom_layout : r->bloom3_layout,
                                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
            r->f.vkCmdDraw(cmd, 3, 1, 0, 0);
            r->f.vkCmdEndRenderPass(cmd);
            r->stats.draws++;
            r->stats.post_passes++;
        }
    }

    /* ---- output pass: tonemap, then the HUD ---- */
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderArea.extent.width = r->width;
    rp.renderArea.extent.height = r->height;
    rp.renderPass = r->post_pass;
    rp.framebuffer = r->fbs[r->offscreen ? 0u : image];
    rp.clearValueCount = 0;
    rp.pClearValues = NULL;
    r->f.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    viewport(r, cmd);
    {
        PostPush pp;
        pp.p[0] = in->exposure;
        pp.p[1] = (float)in->tonemap;
        pp.p[2] = 1.0f / (float)r->width;
        pp.p[3] = 1.0f / (float)r->height;
        if (r->stats.bloom_levels_active != 0u) {
            BloomPush bp;
            memset(&bp, 0, sizeof bp);
            bp.a[0] = in->exposure;
            bp.a[1] = (float)in->tonemap;
            bp.a[2] = (in->post.flags & SU_POST_BLOOM) != 0u && in->post.bloom_intensity > 0.0f
                          ? in->post.bloom_intensity / (float)r->stats.bloom_levels_active
                          : 0.0f;
            bp.a[3] = (in->post.flags & SU_POST_AUTO_EXPOSURE) != 0u ? 1.0f : 0.0f;
            bp.b[0] = in->post.auto_key > 0.0f ? in->post.auto_key : 0.18f;
            bp.b[1] = in->post.exposure_min > 0.0f ? in->post.exposure_min : 1.0f / 32.0f;
            bp.b[2] = in->post.exposure_max > 0.0f ? in->post.exposure_max : 32.0f;
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->post_bloom_pipe);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->bloom3_layout, 0, 1, &r->bl_final_set, 0,
                                         NULL);
            r->f.vkCmdPushConstants(cmd, r->bloom3_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof bp, &bp);
        } else {
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->post_pipe);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->post_layout, 0, 1, &r->post_set, 0, NULL);
            r->f.vkCmdPushConstants(cmd, r->post_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof pp, &pp);
        }
        r->f.vkCmdDraw(cmd, 3, 1, 0, 0);
        r->stats.draws++;
        if (n_hud > 0u) {
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->hud_pipe);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->post_layout, 0, 1, &r->font_set, 0, NULL);
            /* the HUD shaders read the output size from the post push constants, whichever pass drew the tonemap */
            r->f.vkCmdPushConstants(cmd, r->post_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof pp, &pp);
            r->f.vkCmdBindVertexBuffers(cmd, 0, 1, &fr->hud.buf, &zero);
            r->f.vkCmdDraw(cmd, n_hud, 1, 0, 0);
            r->stats.draws++;
        }
    }
    r->f.vkCmdEndRenderPass(cmd);
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
        r->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                  NULL, 0, NULL);
        r->f.vkCmdCopyImageToBuffer(cmd, r->color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->readback.buf, 1, &copy);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        r->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL,
                                  0, NULL);
    }
    if (fr->queries != VK_NULL_HANDLE) {
        r->f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fr->queries, 1);
        fr->timed = 1;
    }
    (void)r->f.vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (!r->offscreen) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &fr->acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &r->rendered[image];
    }
    vr = r->f.vkQueueSubmit(r->queue, 1, &si, fr->fence);
    if (vr != VK_SUCCESS) {
        return su_rvk_fail(r, SU_E_IO, "vkQueueSubmit failed (VkResult %d)", (int)vr);
    }
    fr->submitted = 1;
    fr->serial = ++r->serial;
    if (!r->offscreen) {
        VkPresentInfoKHR pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &r->rendered[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &r->swapchain;
        pi.pImageIndices = &image;
        tp = su_rvk_now_ms();
        vr = r->f.vkQueuePresentKHR(r->queue, &pi);
        r->stats.present_ms = su_rvk_now_ms() - tp;
        if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
            r->needs_rebuild = 1;
        } else if (vr != VK_SUCCESS) {
            return su_rvk_fail(r, SU_E_IO, "vkQueuePresentKHR failed (VkResult %d)", (int)vr);
        }
    }
    r->frame_slot = (r->frame_slot + 1u) % FRAMES_IN_FLIGHT;
    r->stats.frame++;
    r->stats.instances = n_inst;
    r->stats.line_vertices = n_lines;
    r->stats.fx_vertices = n_fx;
    r->stats.fx_tex_vertices = n_fxt;
    r->stats.fx_batches_drawn = n_batches;
    r->stats.fx_quads_sorted = n_quads;
    r->stats.fx_sort_draws = n_sort_draws;
    r->stats.fx_batches_skipped = skipped;
    r->stats.hud_vertices = n_hud;
    r->stats.width = r->width;
    r->stats.height = r->height;
    r->stats.cpu_ms = su_rvk_now_ms() - t0 - r->stats.wait_ms - r->stats.present_ms;
    return SU_OK;
}

static void set4(float *d, float a, float b, float c, float e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}

/* Inverse of a rotation-only view times a reversed-Z infinite projection (for the sky's view rays). */
static SuMat4f inverse_vp(SuMat4f view_rot, SuMat4f proj) {
    SuMat4f inv_proj = su_mat4f_identity();
    SuMat4f inv_view = su_mat4f_identity();
    int i;
    int j;
    /* proj: x' = px x, y' = py y, z' = n, w' = -z  ->  x = x'/px, y = y'/py, z = -w', w = z'/n */
    memset(&inv_proj, 0, sizeof inv_proj);
    inv_proj.m[0] = 1.0f / proj.m[0];
    inv_proj.m[5] = 1.0f / proj.m[5];
    inv_proj.m[14] = -1.0f;
    inv_proj.m[11] = 1.0f / proj.m[14];
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            inv_view.m[i * 4 + j] = view_rot.m[j * 4 + i];
        }
    }
    return su_mat4f_mul(inv_view, inv_proj);
}

uint32_t su_render_frame(SuRenderer *r, const SuRenderView *view, const SuRenderInstance *instances,
                         uint32_t instance_count, const SuRenderLineVertex *lines, uint32_t line_vertex_count) {
    FrameIn in;
    SuMat4f rz = su_mat4f_identity();
    if (r == NULL || view == NULL || (instances == NULL && instance_count > 0u) || (lines == NULL && line_vertex_count > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&in, 0, sizeof in);
    /* z' = w - z turns a 0..1 depth projection into the renderer's reversed Z */
    rz.m[10] = -1.0f;
    rz.m[14] = 1.0f;
    in.debug_vp = su_mat4f_mul(rz, su_mat4f_mul(view->proj, view->view));
    in.light[0] = view->light_dir.x;
    in.light[1] = view->light_dir.y;
    in.light[2] = view->light_dir.z;
    in.light[3] = view->ambient;
    memcpy(in.ubo.view_proj, in.debug_vp.m, sizeof in.ubo.view_proj);
    in.ubo.inv_view_proj[0] = in.ubo.inv_view_proj[5] = in.ubo.inv_view_proj[10] = in.ubo.inv_view_proj[15] = 1.0f;
    set4(in.ubo.sun_dir, view->light_dir.x, view->light_dir.y, view->light_dir.z, 0.0f);
    set4(in.ubo.up, 0.0f, 1.0f, 0.0f, 1.0f);
    in.sky = 0;
    memcpy(in.clear, view->clear, sizeof in.clear);
    in.exposure = 1.0f;
    in.tonemap = SU_TONEMAP_CLAMP;
    in.inst = instances;
    in.inst_count = instance_count;
    in.lines = lines;
    in.line_count = line_vertex_count;
    return render(r, &in);
}

SuMat4f su_render_camera_view(const SuRenderCamera *c) {
    SuQuatf inv;
    inv.x = -c->rot.x;
    inv.y = -c->rot.y;
    inv.z = -c->rot.z;
    inv.w = c->rot.w;
    return su_mat4f_from_quat(inv);
}

static float vlen(SuVec3f v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

uint32_t su_render_frame_ex(SuRenderer *r, const SuRenderFrameDesc *f) {
    FrameIn in;
    SuMat4f view;
    SuMat4f proj;
    const SuRenderLighting *l;
    SuVec3f sd;
    SuVec3f up;
    float len;
    if (r == NULL || f == NULL || f->struct_size != sizeof *f || (f->objects == NULL && f->object_count > 0u) ||
        (f->debug_instances == NULL && f->debug_instance_count > 0u) || (f->lines == NULL && f->line_vertex_count > 0u) ||
        (f->fx == NULL && f->fx_vertex_count > 0u) || (f->hud == NULL && f->hud_vertex_count > 0u) ||
        (f->fx_tex == NULL && f->fx_tex_vertex_count > 0u) || (f->fx_batches == NULL && f->fx_batch_count > 0u) ||
        !(f->camera.fov_y > 0.0f && f->camera.fov_y < 3.1f) || !(f->camera.near_z > 0.0f)) {
        return SU_E_INVALID_ARGUMENT;
    }
    l = &f->light;
    memset(&in, 0, sizeof in);
    view = su_render_camera_view(&f->camera);
    proj = su_mat4f_perspective_rz(f->camera.fov_y, (float)r->width / (float)(r->height > 0u ? r->height : 1u),
                                   f->camera.near_z);
    in.scene_vp = su_mat4f_mul(proj, view);
    in.proj_y = -proj.m[5];
    memcpy(in.ubo.view_proj, in.scene_vp.m, sizeof in.ubo.view_proj);
    memcpy(in.ubo.inv_view_proj, inverse_vp(view, proj).m, sizeof in.ubo.inv_view_proj);
    sd = l->sun_dir;
    len = vlen(sd);
    if (len > 0.0f) {
        sd.x /= len;
        sd.y /= len;
        sd.z /= len;
    } else {
        sd.y = -1.0f;
    }
    up = l->up;
    len = vlen(up);
    if (len > 0.0f) {
        up.x /= len;
        up.y /= len;
        up.z /= len;
    } else {
        up.y = 1.0f;
    }
    set4(in.ubo.sun_dir, sd.x, sd.y, sd.z, l->sun_disc > 0.0f ? cosf(l->sun_disc) : 0.0f);
    set4(in.ubo.sun_color, l->sun_color.x * l->sun_intensity, l->sun_color.y * l->sun_intensity,
         l->sun_color.z * l->sun_intensity, 0.0f);
    set4(in.ubo.sky_zenith, l->sky_zenith.x, l->sky_zenith.y, l->sky_zenith.z, l->ambient);
    set4(in.ubo.sky_horizon, l->sky_horizon.x, l->sky_horizon.y, l->sky_horizon.z, 0.0f);
    set4(in.ubo.sky_nadir, l->sky_nadir.x, l->sky_nadir.y, l->sky_nadir.z, 0.0f);
    in.exposure = l->exposure > 0.0f ? l->exposure : 1.0f;
    in.tonemap = l->tonemap == SU_TONEMAP_CLAMP ? SU_TONEMAP_CLAMP : SU_TONEMAP_ACES;
    set4(in.ubo.up, up.x, up.y, up.z, in.exposure);
    set4(in.ubo.params, (float)in.tonemap, 0.0f, 0.0f, 0.0f);
    in.sky = !l->no_sky;
    in.post = f->post;
    memcpy(in.clear, l->clear_color, sizeof in.clear);
    /* debug items: relative to debug_origin, shifted by (origin - camera) in float */
    {
        SuVec3f off = su_wpos_delta(f->debug_origin, f->camera.pos);
        SuMat4f t = su_mat4f_identity();
        t.m[12] = off.x;
        t.m[13] = off.y;
        t.m[14] = off.z;
        in.debug_vp = su_mat4f_mul(in.scene_vp, t);
    }
    in.light[0] = sd.x;
    in.light[1] = sd.y;
    in.light[2] = sd.z;
    in.light[3] = l->ambient < 1.0f ? (l->ambient > 0.0f ? l->ambient : 0.0f) : 1.0f;
    in.inst = f->debug_instances;
    in.inst_count = f->debug_instance_count;
    in.lines = f->lines;
    in.line_count = f->line_vertex_count;
    in.fx = f->fx;
    in.fx_count = f->fx_vertex_count;
    in.hud = f->hud;
    in.hud_count = f->hud_vertex_count;
    in.fx_tex = f->fx_tex;
    in.fx_tex_count = f->fx_tex_vertex_count;
    in.batches = f->fx_batches;
    in.batch_count = f->fx_batch_count;
    in.sky_cube = f->sky_cube;
    in.sky_intensity = f->sky_intensity;
    in.sky_lod = f->sky_lod;
    in.sky_rot = f->sky_rot;
    in.scene = f;
    if ((f->shadow.flags & SU_SHADOW_ON) != 0u) {
        uint32_t sz = f->shadow.size != 0u ? f->shadow.size : 2048u;
        if (sz < 256u || sz > 8192u || (sz & (sz - 1u)) != 0u) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "shadow size %u is not a power of two in 256..8192", (unsigned)sz);
        }
        if (f->shadow.cascades > 4u || !(f->shadow.split_lambda >= 0.0f && f->shadow.split_lambda <= 1.0f)) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "shadow cascades %u or split lambda out of range",
                               (unsigned)f->shadow.cascades);
        }
        in.shadow_size = sz;
        in.shadow_layers = f->shadow.cascades > 1u ? f->shadow.cascades : 1u;
    }
    return render(r, &in);
}

uint32_t su_render_readback(SuRenderer *r, uint8_t *rgba, size_t cap) {
    uint32_t i;
    if (r == NULL || rgba == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!r->offscreen) {
        return su_rvk_fail(r, SU_E_UNSUPPORTED_METHOD, "readback is for offscreen renderers");
    }
    if (cap < (size_t)r->width * r->height * 4u) {
        return SU_E_RESOURCE_LIMIT;
    }
    (void)r->f.vkDeviceWaitIdle(r->dev);
    if (r->serial > r->completed) {
        r->completed = r->serial;
    }
    /* Oldest submission first, so the stats end with the newest frame's GPU time. */
    for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
        collect_gpu_time(r, &r->frames[(r->frame_slot + i) % FRAMES_IN_FLIGHT]);
    }
    memcpy(rgba, r->readback.map, (size_t)r->width * r->height * 4u);
    return SU_OK;
}
