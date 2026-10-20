/* GPU resources of sutekh/render.h: generational handles, asynchronous budgeted uploads, deferred retirement,
 * material descriptor sets, and uploading a cooked pack.
 *
 * Upload path: creation validates the data and creates the (device-local) buffer or image right away; the bytes
 * are copied later by su_rvk_record_uploads, called at the start of every frame's command buffer: up to the slot's
 * staging size per frame, through vkCmdCopyBuffer / vkCmdCopyBufferToImage (textures in whole block rows, so a mip
 * level can span frames), followed by one barrier that makes the copies visible to vertex input and fragment
 * shaders of the same command buffer. Nothing waits on the queue.
 *
 * Retirement: a released resource's Vulkan objects move to a list tagged with the serial of the last submitted
 * frame; they are destroyed once that frame's fence has signalled. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "render/su_render_vk_int.h"

#define MIN_BUDGET (256u << 10)

typedef struct QEntry {
    uint32_t index;
    uint32_t gen;
} QEntry;

/* The upload queue stores (index, generation); entries of released resources are skipped. */
static QEntry *queue_of(SuRenderer *r) {
    return (QEntry *)(void *)r->upload_q;
}

static SuRenderHandle make_handle(uint32_t type, uint32_t gen, uint32_t index) {
    return ((uint64_t)type << 56) | ((uint64_t)(gen & 0xFFFFFFu) << 32) | (uint64_t)(index + 1u);
}

Res *su_rvk_get(const SuRenderer *r, SuRenderHandle h, uint32_t type, uint32_t *index) {
    uint32_t idx = (uint32_t)(h & 0xFFFFFFFFu);
    uint32_t t = (uint32_t)(h >> 56);
    uint32_t gen = (uint32_t)((h >> 32) & 0xFFFFFFu);
    Res *s;
    if (r == NULL || h == 0u || idx == 0u || idx > r->res_cap || (type != RES_NONE && t != type)) {
        return NULL;
    }
    s = &r->res[idx - 1u];
    if (s->type != t || (s->gen & 0xFFFFFFu) != gen) {
        return NULL;
    }
    if (index != NULL) {
        *index = idx - 1u;
    }
    return s;
}

uint32_t su_rvk_res_init(SuRenderer *r, uint32_t cap) {
    uint32_t i;
    cap += 16u; /* built-in textures */
    r->res = calloc(cap, sizeof *r->res);
    r->free_list = malloc((size_t)cap * sizeof *r->free_list);
    r->upload_q = malloc((size_t)cap * 2u * sizeof(QEntry));
    if (r->res == NULL || r->free_list == NULL || r->upload_q == NULL) {
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "resource table allocation");
    }
    r->res_cap = cap;
    for (i = 0; i < cap; i++) {
        r->free_list[i] = cap - 1u - i;
    }
    r->free_count = cap;
    if (r->budget < MIN_BUDGET) {
        r->budget = MIN_BUDGET;
    }
    return SU_OK;
}

static uint32_t alloc_slot(SuRenderer *r, uint32_t type, uint32_t *index) {
    Res *s;
    uint32_t gen;
    if (r->free_count == 0u) {
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "max_resources (%u) reached", (unsigned)(r->res_cap - 16u));
    }
    *index = r->free_list[--r->free_count];
    s = &r->res[*index];
    gen = s->gen;
    memset(s, 0, sizeof *s);
    s->gen = gen;
    s->type = (uint8_t)type;
    s->state = SU_RENDER_STATE_PENDING;
    return SU_OK;
}

static void free_slot(SuRenderer *r, uint32_t index) {
    Res *s = &r->res[index];
    uint32_t gen = s->gen + 1u;
    memset(s, 0, sizeof *s);
    s->gen = gen;
    r->free_list[r->free_count++] = index;
}

static void enqueue(SuRenderer *r, uint32_t index) {
    QEntry *q = queue_of(r);
    uint32_t cap = r->res_cap * 2u;
    if (r->q_count == cap) {
        /* compact: drop entries of released or already uploaded resources */
        uint32_t k;
        uint32_t n = 0;
        for (k = 0; k < r->q_count; k++) {
            QEntry e = q[(r->q_head + k) % cap];
            if (r->res[e.index].queued && r->res[e.index].gen == e.gen) {
                q[n++] = e;
            }
        }
        r->q_head = 0;
        r->q_count = n;
    }
    q[(r->q_head + r->q_count) % cap].index = index;
    q[(r->q_head + r->q_count) % cap].gen = r->res[index].gen;
    r->q_count++;
    r->res[index].queued = 1u;
}

static void retire(SuRenderer *r, const Retired *t) {
    if (r->retired_count == r->retired_cap) {
        uint32_t nc = r->retired_cap ? r->retired_cap * 2u : 64u;
        Retired *n = realloc(r->retired, (size_t)nc * sizeof *n);
        if (n == NULL) {
            /* cannot defer: wait for the device instead (rare; never on the frame path in practice) */
            Retired copy = *t;
            (void)r->f.vkDeviceWaitIdle(r->dev);
            copy.serial = 0;
            su_rvk_free_buffer(r, &copy.a);
            su_rvk_free_buffer(r, &copy.b);
            if (copy.view != VK_NULL_HANDLE) {
                r->f.vkDestroyImageView(r->dev, copy.view, NULL);
            }
            if (copy.img != VK_NULL_HANDLE) {
                r->f.vkDestroyImage(r->dev, copy.img, NULL);
            }
            if (copy.mem != VK_NULL_HANDLE) {
                r->f.vkFreeMemory(r->dev, copy.mem, NULL);
            }
            if (copy.set != VK_NULL_HANDLE) {
                (void)r->f.vkFreeDescriptorSets(r->dev, r->pool_mat, 1, &copy.set);
            }
            return;
        }
        r->retired = n;
        r->retired_cap = nc;
    }
    r->retired[r->retired_count] = *t;
    r->retired[r->retired_count].serial = r->serial;
    r->retired_count++;
}

static void destroy_retired(SuRenderer *r, Retired *t) {
    su_rvk_free_buffer(r, &t->a);
    su_rvk_free_buffer(r, &t->b);
    if (t->view != VK_NULL_HANDLE) {
        r->f.vkDestroyImageView(r->dev, t->view, NULL);
    }
    if (t->img != VK_NULL_HANDLE) {
        r->f.vkDestroyImage(r->dev, t->img, NULL);
    }
    if (t->mem != VK_NULL_HANDLE) {
        r->f.vkFreeMemory(r->dev, t->mem, NULL);
    }
    if (t->set != VK_NULL_HANDLE) {
        (void)r->f.vkFreeDescriptorSets(r->dev, r->pool_mat, 1, &t->set);
    }
}

void su_rvk_collect_retired(SuRenderer *r) {
    uint32_t i = 0;
    while (i < r->retired_count) {
        if (r->retired[i].serial <= r->completed) {
            destroy_retired(r, &r->retired[i]);
            r->retired[i] = r->retired[--r->retired_count];
        } else {
            i++;
        }
    }
    r->stats.retired_pending = r->retired_count;
}

/* Moves a slot's GPU objects to the retirement list and frees the slot. */
static void release_index(SuRenderer *r, uint32_t index) {
    Res *s = &r->res[index];
    Retired t;
    memset(&t, 0, sizeof t);
    switch (s->type) {
    case RES_MESH:
        t.a = s->u.mesh.vb;
        t.b = s->u.mesh.ib;
        free(s->u.mesh.subs);
        free(s->u.mesh.owned);
        break;
    case RES_TEXTURE:
        t.img = s->u.tex.img;
        t.mem = s->u.tex.mem;
        t.view = s->u.tex.view;
        t.set = s->u.tex.env_set;
        free(s->u.tex.owned);
        break;
    case RES_MATERIAL:
        t.set = s->u.mat.set;
        t.a = s->u.mat.ubo;
        break;
    case RES_MODEL:
        free(s->u.model.nodes);
        free(s->u.model.node_center);
        break;
    default:
        break;
    }
    retire(r, &t);
    free_slot(r, index);
}

void su_rvk_res_shutdown(SuRenderer *r) {
    uint32_t i;
    if (r->res != NULL) {
        for (i = 0; i < r->res_cap; i++) {
            if (r->res[i].type != RES_NONE) {
                release_index(r, i);
            }
        }
    }
    r->completed = r->serial; /* the device is idle here */
    su_rvk_collect_retired(r);
    free(r->retired);
    free(r->res);
    free(r->free_list);
    free(r->upload_q);
    free(r->font_pixels);
    r->retired = NULL;
    r->res = NULL;
    r->free_list = NULL;
    r->upload_q = NULL;
    r->font_pixels = NULL;
}

uint32_t su_render_resource_state(const SuRenderer *r, SuRenderHandle h) {
    Res *s = su_rvk_get(r, h, RES_NONE, NULL);
    if (s == NULL || s->internal) {
        return SU_RENDER_STATE_INVALID;
    }
    if (s->type == RES_MODEL) {
        uint32_t i;
        uint32_t k;
        uint32_t st = SU_RENDER_STATE_READY;
        for (i = 0; i < s->u.model.count; i++) {
            for (k = 0; k < s->u.model.nodes[i].lod_count; k++) {
                Res *m = su_rvk_get(r, s->u.model.nodes[i].lod_mesh[k], RES_MESH, NULL);
                if (m == NULL || m->state == SU_RENDER_STATE_FAILED) {
                    return SU_RENDER_STATE_FAILED;
                }
                if (m->state != SU_RENDER_STATE_READY) {
                    st = SU_RENDER_STATE_PENDING;
                }
            }
        }
        return st;
    }
    return s->state;
}

uint32_t su_render_release(SuRenderer *r, SuRenderHandle h) {
    uint32_t idx = 0;
    Res *s = su_rvk_get(r, h, RES_NONE, &idx);
    if (s == NULL || s->internal) {
        return SU_E_STALE_HANDLE;
    }
    release_index(r, idx);
    r->stats.resources--;
    return SU_OK;
}

/* ---------- meshes ---------- */

uint32_t su_render_mesh_create(SuRenderer *r, const SuRenderMeshDesc *d, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t i;
    Res *s;
    MeshRes *m;
    VkDeviceSize vbytes;
    VkDeviceSize ibytes;
    if (r == NULL || d == NULL || out == NULL || d->vertices == NULL || d->indices == NULL || d->submeshes == NULL ||
        d->vertex_count == 0u || d->index_count < 3u || d->index_count % 3u != 0u || d->submesh_count == 0u ||
        (d->flags & ~SU_RENDER_UPLOAD_COPY) != 0u || !(d->radius >= 0.0f)) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0;
    for (i = 0; i < d->index_count; i++) {
        if (d->indices[i] >= d->vertex_count) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "mesh index %u out of range", (unsigned)i);
        }
    }
    for (i = 0; i < d->submesh_count; i++) {
        const SuAssetSubmesh *sm = &d->submeshes[i];
        if (sm->index_count == 0u || sm->first_index > d->index_count || sm->index_count > d->index_count - sm->first_index ||
            (d->materials != NULL && d->materials[i] != 0u && su_rvk_get(r, d->materials[i], RES_MATERIAL, NULL) == NULL)) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "submesh %u: range or material handle invalid", (unsigned)i);
        }
    }
    if (alloc_slot(r, RES_MESH, &idx) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    m = &s->u.mesh;
    vbytes = (VkDeviceSize)d->vertex_count * sizeof(SuAssetVertex);
    ibytes = (VkDeviceSize)d->index_count * 4u;
    m->subs = malloc((size_t)d->submesh_count * sizeof *m->subs);
    if (m->subs == NULL) {
        free_slot(r, idx);
        return SU_E_RESOURCE_LIMIT;
    }
    for (i = 0; i < d->submesh_count; i++) {
        m->subs[i].first_index = d->submeshes[i].first_index;
        m->subs[i].index_count = d->submeshes[i].index_count;
        m->subs[i].material = d->materials != NULL ? d->materials[i] : 0u;
        memcpy(m->subs[i].center, d->submeshes[i].center, sizeof m->subs[i].center);
        m->subs[i].radius = d->submeshes[i].radius;
    }
    m->sub_count = d->submesh_count;
    m->vertex_count = d->vertex_count;
    m->index_count = d->index_count;
    memcpy(m->center, d->center, sizeof m->center);
    m->radius = d->radius;
    if ((d->flags & SU_RENDER_UPLOAD_COPY) != 0u) {
        m->owned = malloc((size_t)(vbytes + ibytes));
        if (m->owned == NULL) {
            free(m->subs);
            free_slot(r, idx);
            return SU_E_RESOURCE_LIMIT;
        }
        memcpy(m->owned, d->vertices, (size_t)vbytes);
        memcpy(m->owned + vbytes, d->indices, (size_t)ibytes);
        m->src_v = m->owned;
        m->src_i = m->owned + vbytes;
    } else {
        m->src_v = (const uint8_t *)d->vertices;
        m->src_i = (const uint8_t *)d->indices;
    }
    if (!su_rvk_make_buffer(r, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m->vb) ||
        !su_rvk_make_buffer(r, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m->ib)) {
        Retired t;
        memset(&t, 0, sizeof t);
        t.a = m->vb;
        t.b = m->ib;
        destroy_retired(r, &t);
        free(m->subs);
        free(m->owned);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "device memory for a mesh of %llu bytes",
                           (unsigned long long)(vbytes + ibytes));
    }
    enqueue(r, idx);
    r->stats.resources++;
    *out = make_handle(RES_MESH, s->gen, idx);
    return SU_OK;
}

/* ---------- textures ---------- */

static VkFormat vk_format(uint32_t f) {
    switch (f) {
    case SU_TEX_RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case SU_TEX_RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case SU_TEX_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case SU_TEX_BC1_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case SU_TEX_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case SU_TEX_BC3_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case SU_TEX_BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case SU_TEX_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case SU_TEX_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case SU_TEX_BC7_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
    case SU_TEX_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
    case SU_TEX_BC2_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case SU_TEX_BC6H_UF16: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case SU_TEX_BC6H_SF16: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case SU_TEX_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TEXFMT_ENV: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static int is_bc(uint32_t f) {
    return (f >= SU_TEX_BC1_UNORM && f <= SU_TEX_BC7_SRGB) || (f >= SU_TEX_BC2_UNORM && f <= SU_TEX_BC6H_SF16);
}

static uint64_t level_bytes(uint32_t format, uint32_t w, uint32_t h) {
    return format == TEXFMT_ENV ? (uint64_t)w * h * 8u : su_asset_texture_level_size(format, w, h);
}

static int make_texture_image_usage(SuRenderer *r, TexRes *t, VkImageUsageFlags extra);

static int make_texture_image(SuRenderer *r, TexRes *t) {
    return make_texture_image_usage(r, t, 0);
}

static int make_texture_image_usage(SuRenderer *r, TexRes *t, VkImageUsageFlags extra) {
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vi;
    uint32_t type = 0;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = t->depth > 1u ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = t->vkfmt;
    ii.extent.width = t->width;
    ii.extent.height = t->height;
    ii.extent.depth = t->depth > 1u ? t->depth : 1u;
    ii.mipLevels = t->mips;
    ii.arrayLayers = t->layers;
    ii.flags = t->layers == 6u ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (r->f.vkCreateImage(r->dev, &ii, NULL, &t->img) != VK_SUCCESS) {
        return 0;
    }
    r->f.vkGetImageMemoryRequirements(r->dev, t->img, &req);
    if (!su_rvk_find_memory(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        return 0;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (r->f.vkAllocateMemory(r->dev, &ai, NULL, &t->mem) != VK_SUCCESS ||
        r->f.vkBindImageMemory(r->dev, t->img, t->mem, 0) != VK_SUCCESS) {
        return 0;
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->img;
    vi.viewType = t->depth > 1u ? VK_IMAGE_VIEW_TYPE_3D :
                  t->layers == 6u ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->vkfmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = t->mips;
    vi.subresourceRange.layerCount = t->layers;
    return r->f.vkCreateImageView(r->dev, &vi, NULL, &t->view) == VK_SUCCESS;
}

/* The sampler set of a cube texture (env_dsl, binding 0): linear, mipmapped, clamp to edge. 1 on success. */
static int make_cube_set(SuRenderer *r, TexRes *t) {
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = r->pool_mat;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r->env_dsl;
    if (r->f.vkAllocateDescriptorSets(r->dev, &ai, &t->env_set) != VK_SUCCESS) {
        t->env_set = VK_NULL_HANDLE;
        return 0;
    }
    ii.sampler = r->samplers[4];
    ii.imageView = t->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = t->env_set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
    return 1;
}

static uint32_t texture_create(SuRenderer *r, const SuRenderTextureDesc *d, int internal, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t i;
    uint32_t maxm = 1;
    uint32_t big;
    uint32_t layers;
    uint32_t l;
    Res *s;
    TexRes *t;
    VkFormatProperties fp;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (r == NULL || d == NULL || out == NULL || d->mip_data == NULL || d->width == 0u || d->height == 0u ||
        d->width > 16384u || d->height > 16384u || vk_format(d->format) == VK_FORMAT_UNDEFINED || d->mip_count == 0u ||
        (d->flags & ~SU_RENDER_UPLOAD_COPY) != 0u || (d->layers > 1u && (d->layers != 6u || d->width != d->height))) {
        return SU_E_INVALID_ARGUMENT;
    }
    layers = d->layers > 1u ? d->layers : 1u;
    if (d->depth > 1u && (d->width > 256u || d->height > 256u || d->depth > 256u ||
                        layers != 1u || d->mip_count != 1u ||
                        d->format != SU_TEX_RGBA8_UNORM)) {
        return SU_E_INVALID_ARGUMENT;
    }
    big = d->width > d->height ? d->width : d->height;
    while ((big >>= 1) != 0u) {
        maxm++;
    }
    if (d->mip_count > maxm || d->mip_count > SU_ASSET_MAX_MIPS) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < d->mip_count * layers; i++) {
        if (d->mip_data[i] == NULL) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    *out = 0;
    if (alloc_slot(r, RES_TEXTURE, &idx) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    s->internal = (uint8_t)internal;
    t = &s->u.tex;
    t->format = d->format;
    t->layers = layers;
    t->cube_mips = d->mip_count;
    t->vkfmt = vk_format(d->format);
    t->width = d->width;
    t->height = d->height;
    t->depth = d->depth > 1u ? d->depth : 1u;
    t->mips = d->mip_count;
    r->f.vkGetPhysicalDeviceFormatProperties(r->pd, t->vkfmt, &fp);
    if ((is_bc(d->format) && !r->bc) || (fp.optimalTilingFeatures & need) != need) {
        s->state = SU_RENDER_STATE_FAILED;
        (void)su_rvk_fail(r, SU_E_UNSUPPORTED_METHOD, "texture format %s is not sampleable on this device",
                          su_asset_texture_format_name(d->format));
        if (!internal) {
            r->stats.resources++;
        }
        *out = make_handle(RES_TEXTURE, s->gen, idx);
        return SU_OK;
    }
    if ((d->flags & SU_RENDER_UPLOAD_COPY) != 0u) {
        uint64_t total = 0;
        uint8_t *p;
        for (i = 0; i < d->mip_count; i++) {
            total += t->depth * layers * su_asset_texture_level_size(d->format, d->width >> i ? d->width >> i : 1u,
                                                            d->height >> i ? d->height >> i : 1u);
        }
        t->owned = malloc((size_t)total);
        if (t->owned == NULL) {
            free_slot(r, idx);
            return SU_E_RESOURCE_LIMIT;
        }
        p = t->owned;
        for (i = 0; i < d->mip_count; i++) {
            uint64_t n = t->depth * su_asset_texture_level_size(d->format, d->width >> i ? d->width >> i : 1u,
                                                     d->height >> i ? d->height >> i : 1u);
            for (l = 0; l < layers; l++) {
                memcpy(p, d->mip_data[i * layers + l], (size_t)n);
                t->src[i * layers + l] = p;
                p += n;
            }
        }
    } else {
        for (i = 0; i < d->mip_count * layers; i++) {
            t->src[i] = d->mip_data[i];
        }
    }
    if (!make_texture_image(r, t)) {
        Retired rt;
        memset(&rt, 0, sizeof rt);
        rt.img = t->img;
        rt.mem = t->mem;
        rt.view = t->view;
        destroy_retired(r, &rt);
        free(t->owned);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "device memory for a %ux%u texture", (unsigned)d->width,
                           (unsigned)d->height);
    }
    if (layers == 6u && !make_cube_set(r, t)) {
        Retired rt;
        memset(&rt, 0, sizeof rt);
        rt.img = t->img;
        rt.mem = t->mem;
        rt.view = t->view;
        destroy_retired(r, &rt);
        free(t->owned);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "descriptor pool exhausted");
    }
    enqueue(r, idx);
    if (!internal) {
        r->stats.resources++;
    }
    *out = make_handle(RES_TEXTURE, s->gen, idx);
    return SU_OK;
}

uint32_t su_render_texture_create(SuRenderer *r, const SuRenderTextureDesc *d, SuRenderHandle *out) {
    return texture_create(r, d, 0, out);
}

/* ---------- uploads ---------- */

static void tex_barrier(SuRenderer *r, VkCommandBuffer cmd, TexRes *t, int to_shader) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = t->mips;
    b.subresourceRange.layerCount = t->layers;
    if (!to_shader) {
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        r->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                  NULL, 1, &b);
    } else {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        r->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                  0, NULL, 1, &b);
    }
}

/* Copies as much of mesh m as fits; returns 1 when it is complete. */
static int upload_mesh(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, MeshRes *m, VkDeviceSize *used) {
    VkDeviceSize vbytes = (VkDeviceSize)m->vertex_count * sizeof(SuAssetVertex);
    VkDeviceSize total = vbytes + (VkDeviceSize)m->index_count * 4u;
    while (m->done < total && *used < fr->staging.size) {
        VkDeviceSize room = fr->staging.size - *used;
        int in_v = m->done < vbytes;
        VkDeviceSize left = in_v ? vbytes - m->done : total - m->done;
        VkDeviceSize n = left < room ? left : room;
        VkBufferCopy c;
        n &= ~(VkDeviceSize)3u;
        if (n == 0u) {
            break;
        }
        memcpy((uint8_t *)fr->staging.map + *used, in_v ? m->src_v + m->done : m->src_i + (m->done - vbytes), (size_t)n);
        c.srcOffset = *used;
        c.dstOffset = in_v ? m->done : m->done - vbytes;
        c.size = n;
        r->f.vkCmdCopyBuffer(cmd, fr->staging.buf, in_v ? m->vb.buf : m->ib.buf, 1, &c);
        m->done += n;
        *used += n;
    }
    return m->done == total;
}

/* Copies whole block rows of the current mips as long as they fit; returns 1 when the texture is complete. */
static int upload_texture(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, TexRes *t, VkDeviceSize *used) {
    uint32_t block = is_bc(t->format) ? 4u : 1u;
    if (!t->transitioned) {
        tex_barrier(r, cmd, t, 0);
        t->transitioned = 1;
    }
    while (t->mip < t->mips) {
        uint32_t w = t->width >> t->mip ? t->width >> t->mip : 1u;
        uint32_t h = t->height >> t->mip ? t->height >> t->mip : 1u;
        uint32_t rows = (h + block - 1u) / block;
        uint64_t lvl_bytes = level_bytes(t->format, w, h);
        uint64_t row_bytes = lvl_bytes / rows;
        VkDeviceSize at = (*used + 15u) & ~(VkDeviceSize)15u;
        uint64_t fit;
        uint32_t n;
        VkBufferImageCopy c;
        if (at >= fr->staging.size) {
            return 0;
        }
        fit = (uint64_t)(fr->staging.size - at) / row_bytes;
        n = rows - t->row < fit ? rows - t->row : (uint32_t)fit;
        if (n == 0u) {
            return 0;
        }
        memcpy((uint8_t *)fr->staging.map + at,
               t->src[t->mip * t->layers + (t->depth > 1u ? 0u : t->layer)] +
                   (uint64_t)t->row * row_bytes + (t->depth > 1u ? t->layer * lvl_bytes : 0u),
               (size_t)(n * row_bytes));
        memset(&c, 0, sizeof c);
        c.bufferOffset = at;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.mipLevel = t->mip;
        c.imageSubresource.baseArrayLayer = t->depth > 1u ? 0u : t->layer;
        c.imageSubresource.layerCount = 1;
        c.imageOffset.y = (int32_t)(t->row * block);
        c.imageOffset.z = t->depth > 1u ? (int32_t)t->layer : 0;
        c.imageExtent.width = w;
        c.imageExtent.height = (t->row + n) * block < h ? n * block : h - t->row * block;
        c.imageExtent.depth = 1;
        r->f.vkCmdCopyBufferToImage(cmd, fr->staging.buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        *used = at + n * row_bytes;
        t->row += n;
        if (t->row == rows) {
            t->row = 0;
            if (++t->layer == (t->depth > 1u ? t->depth : t->layers)) {
                t->layer = 0;
                t->mip++;
            }
        }
    }
    tex_barrier(r, cmd, t, 1);
    return 1;
}

void su_rvk_record_uploads(SuRenderer *r, Frame *fr, VkCommandBuffer cmd) {
    QEntry *q = queue_of(r);
    uint32_t cap = r->res_cap * 2u;
    VkDeviceSize used = 0;
    int buffers = 0;
    r->tex_ready_this_frame = 0;
    while (r->q_count > 0u && used < fr->staging.size) {
        QEntry e = q[r->q_head];
        Res *s = &r->res[e.index];
        int done;
        if (!s->queued || s->gen != e.gen) {
            r->q_head = (r->q_head + 1u) % cap;
            r->q_count--;
            continue;
        }
        if (s->type == RES_MESH) {
            done = upload_mesh(r, fr, cmd, &s->u.mesh, &used);
            buffers = 1;
        } else {
            done = upload_texture(r, fr, cmd, &s->u.tex, &used);
        }
        if (!done) {
            break;
        }
        s->queued = 0;
        s->state = SU_RENDER_STATE_READY;
        if (s->type == RES_MESH) {
            free(s->u.mesh.owned);
            s->u.mesh.owned = NULL;
            s->u.mesh.src_v = s->u.mesh.src_i = NULL;
        } else {
            free(s->u.tex.owned);
            s->u.tex.owned = NULL;
            memset((void *)s->u.tex.src, 0, sizeof s->u.tex.src);
            r->tex_ready_this_frame = 1;
        }
        r->q_head = (r->q_head + 1u) % cap;
        r->q_count--;
    }
    if (buffers) {
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        r->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0,
                                  NULL, 0, NULL);
    }
    r->stats.upload_bytes_frame = used;
    r->stats.upload_bytes_total += used;
    {
        uint32_t k;
        uint32_t pending = 0;
        for (k = 0; k < r->q_count; k++) {
            QEntry x = q[(r->q_head + k) % cap];
            pending += r->res[x.index].queued && r->res[x.index].gen == x.gen;
        }
        r->stats.uploads_pending = pending;
    }
}

/* ---------- materials ---------- */

static uint32_t pipe_of(const SuAssetMaterial *m) {
    uint32_t p = m->alpha_mode == SU_ASSET_ALPHA_MASK    ? PIPE_MASK
                 : m->alpha_mode == SU_ASSET_ALPHA_BLEND ? PIPE_BLEND
                                                         : PIPE_OPAQUE;
    return p + (m->double_sided ? 1u : 0u);
}

static VkImageView view_of(SuRenderer *r, SuRenderHandle h) {
    Res *s = su_rvk_get(r, h, RES_TEXTURE, NULL);
    return s != NULL && s->state == SU_RENDER_STATE_READY && s->u.tex.depth <= 1u ? s->u.tex.view : VK_NULL_HANDLE;
}

/* Allocates and writes a material set; slots without a ready texture get the defaults. Returns the ready mask. */
static VkDescriptorSet write_material_set(SuRenderer *r, const SuRenderHandle *tex, const SuAssetMaterial *f,
                                          uint32_t *ready) {
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[SU_ASSET_SLOT_COUNT];
    VkWriteDescriptorSet w[SU_ASSET_SLOT_COUNT];
    VkImageView white = view_of(r, r->white_tex);
    VkImageView flat = view_of(r, r->normal_tex);
    uint32_t i;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = r->pool_mat;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r->mat_dsl;
    if (r->f.vkAllocateDescriptorSets(r->dev, &ai, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    if (white == VK_NULL_HANDLE) {
        white = r->res[(uint32_t)(r->white_tex & 0xFFFFFFFFu) - 1u].u.tex.view; /* uploaded first in the first frame */
    }
    if (flat == VK_NULL_HANDLE) {
        flat = r->res[(uint32_t)(r->normal_tex & 0xFFFFFFFFu) - 1u].u.tex.view;
    }
    *ready = 0;
    for (i = 0; i < SU_ASSET_SLOT_COUNT; i++) {
        VkImageView v = tex != NULL ? view_of(r, tex[i]) : VK_NULL_HANDLE;
        uint32_t info = f != NULL ? f->tex_info[i] : 0u;
        uint32_t ws = (info >> 8) & 0xFu;
        uint32_t wt = (info >> 12) & 0xFu;
        if (v != VK_NULL_HANDLE) {
            *ready |= 1u << i;
        } else {
            v = i == SU_ASSET_SLOT_NORMAL ? flat : white;
        }
        ii[i].sampler = r->samplers[(ws > 2u ? 0u : ws) * 3u + (wt > 2u ? 0u : wt)];
        ii[i].imageView = v;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w[i], 0, sizeof w[i]);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    r->f.vkUpdateDescriptorSets(r->dev, SU_ASSET_SLOT_COUNT, w, 0, NULL);
    return set;
}

/* Paint slot defaults when a texture is missing or still uploading: normal flat, mask all zero, the rest white. */
static VkDescriptorSet write_paint_set(SuRenderer *r, const SuRenderHandle *tex, const uint32_t *tex_info, const Buffer *ubo,
                                       uint32_t *ready) {
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[SU_RENDER_PAINT_SLOT_COUNT];
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet w[SU_RENDER_PAINT_SLOT_COUNT + 1u];
    VkImageView dflt[3];
    uint32_t i;
    SuRenderHandle dh[3];
    dh[0] = r->white_tex;
    dh[1] = r->normal_tex;
    dh[2] = r->black_tex;
    for (i = 0; i < 3u; i++) {
        dflt[i] = view_of(r, dh[i]);
        if (dflt[i] == VK_NULL_HANDLE) {
            dflt[i] = r->res[(uint32_t)(dh[i] & 0xFFFFFFFFu) - 1u].u.tex.view; /* uploaded first in the first frame */
        }
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = r->pool_mat;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r->paint_dsl;
    if (r->f.vkAllocateDescriptorSets(r->dev, &ai, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    *ready = 0;
    for (i = 0; i < SU_RENDER_PAINT_SLOT_COUNT; i++) {
        VkImageView v = tex != NULL ? view_of(r, tex[i]) : VK_NULL_HANDLE;
        uint32_t info = tex_info != NULL ? tex_info[i] : 0u;
        uint32_t ws = (info >> 8) & 0xFu;
        uint32_t wt = (info >> 12) & 0xFu;
        if (v != VK_NULL_HANDLE) {
            *ready |= 1u << i;
        } else {
            v = i == SU_PAINT_SLOT_NORMAL || i == SU_PAINT_SLOT_DETAIL_NORMAL ? dflt[1] :
                i == SU_PAINT_SLOT_MASK || i == SU_PAINT_SLOT_DETAIL_MASK ? dflt[2] : dflt[0];
        }
        ii[i].sampler = r->samplers[(ws > 2u ? 0u : ws) * 3u + (wt > 2u ? 0u : wt)];
        ii[i].imageView = v;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w[i], 0, sizeof w[i]);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i < SU_PAINT_SLOT_COUNT ? i : i + 1u;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    bi.buffer = ubo->buf;
    bi.offset = 0;
    bi.range = sizeof(PaintUbo);
    memset(&w[SU_RENDER_PAINT_SLOT_COUNT], 0, sizeof w[0]);
    w[SU_RENDER_PAINT_SLOT_COUNT].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[SU_RENDER_PAINT_SLOT_COUNT].dstSet = set;
    w[SU_RENDER_PAINT_SLOT_COUNT].dstBinding = SU_PAINT_SLOT_COUNT;
    w[SU_RENDER_PAINT_SLOT_COUNT].descriptorCount = 1;
    w[SU_RENDER_PAINT_SLOT_COUNT].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[SU_RENDER_PAINT_SLOT_COUNT].pBufferInfo = &bi;
    r->f.vkUpdateDescriptorSets(r->dev, SU_RENDER_PAINT_SLOT_COUNT + 1u, w, 0, NULL);
    return set;
}

static int tex_srgb_format(uint32_t f) {
    return f == SU_TEX_RGBA8_SRGB || f == SU_TEX_BC1_SRGB || f == SU_TEX_BC3_SRGB || f == SU_TEX_BC7_SRGB ||
           f == SU_TEX_BC2_SRGB;
}

static int all_finite(const float *v, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return 0;
        }
    }
    return 1;
}

uint32_t su_render_paint_material_create(SuRenderer *r, const SuRenderPaintDesc *d, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t i;
    uint32_t seen = 0;
    uint32_t glow_mask = 0;
    Res *s;
    PaintUbo u;
    const SuAssetPaint *p;
    if (r == NULL || d == NULL || out == NULL || d->factors.alpha_mode > SU_ASSET_ALPHA_BLEND) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0;
    p = &d->paint;
    if ((d->flags & ~7u) != 0u || !all_finite(d->specular_factor, 3u) ||
        !isfinite(d->detail_strength) || d->detail_strength < 0.0f || d->detail_strength > 1.0f ||
        !all_finite(d->detail_uv_scale, 2u) || !isfinite(d->detail_rotation) || d->detail_layer_mask > 31u ||
        (d->specular_channels & 0xFF000000u) != 0u) {
        return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: extension parameters invalid");
    }
    for (i = 0; i < 3u; i++) {
        if (d->specular_factor[i] < 0.0f || d->specular_factor[i] > 1.0f ||
            ((d->specular_channels >> (8u * i)) & 255u) > 5u) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: specular factor or channel invalid");
        }
    }
    for (i = 0; i < 4u; i++) {
        uint32_t info = d->extra_tex_info[i];
        if ((info & ~0x00003301u) != 0u || ((info >> 8) & 15u) > 2u || ((info >> 12) & 15u) > 2u) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: extra texture coordinates invalid");
        }
    }
    if (!all_finite(d->factors.base_color, 13u) || p->layer_count > SU_PAINT_MAX_LAYERS || !all_finite(&p->specular, 4u)) {
        return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: layer count or factors invalid");
    }
    for (i = 0; i < p->layer_count; i++) {
        const SuAssetPaintLayer *l = &p->layer[i];
        if (p->layer_order[i] >= p->layer_count || (seen & (1u << p->layer_order[i])) != 0u) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: layer_order is not a permutation of the layers");
        }
        seen |= 1u << p->layer_order[i];
        if (!all_finite(l->color, 10u) ||
            (l->flags & ~(SU_PAINT_LAYER_MULTIPLY | SU_PAINT_LAYER_GLOW_TINT | SU_PAINT_LAYER_TINT)) != 0u ||
            (l->flags & (SU_PAINT_LAYER_MULTIPLY | SU_PAINT_LAYER_TINT)) == (SU_PAINT_LAYER_MULTIPLY | SU_PAINT_LAYER_TINT)) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: layer %u invalid", (unsigned)i);
        }
    }
    for (i = 0; i < SU_PAINT_CH_COUNT; i++) {
        if (p->channel[i] > SU_PAINT_CH_NONE) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint material: channel selector %u invalid", (unsigned)i);
        }
    }
    for (i = 0; i < SU_RENDER_PAINT_SLOT_COUNT; i++) {
        const Res *x;
        int need_srgb = i == SU_PAINT_SLOT_ALBEDO; /* glow: either (sRGB colour map or linear mask) */
        if (d->textures[i] == 0u) {
            continue;
        }
        x = su_rvk_get(r, d->textures[i], RES_TEXTURE, NULL);
        if (x == NULL || x->u.tex.layers != 1u || x->u.tex.depth > 1u) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint slot %u: unknown texture handle", (unsigned)i);
        }
        if (i == SU_PAINT_SLOT_GLOW) {
            glow_mask = !tex_srgb_format(x->u.tex.format);
        } else if (i != SU_PAINT_SLOT_SPECULAR && tex_srgb_format(x->u.tex.format) != need_srgb) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "paint slot %u needs a %s texture (got %s)", (unsigned)i,
                               need_srgb ? "sRGB" : "linear", su_asset_texture_format_name(x->u.tex.format));
        }
    }
    if (alloc_slot(r, RES_MATERIAL, &idx) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    s->u.mat.kind = 1u;
    s->u.mat.f = d->factors;
    s->u.mat.paint = d->paint;
    memcpy(s->u.mat.paint_tex_info, p->tex_info, sizeof p->tex_info);
    memcpy(s->u.mat.paint_tex_info + SU_PAINT_SLOT_COUNT, d->extra_tex_info, sizeof d->extra_tex_info);
    memcpy(s->u.mat.tex, d->textures, sizeof s->u.mat.tex);
    s->u.mat.pipeline = d->overlay ? PIPE_OVERLAY + (d->factors.double_sided ? 1u : 0u) : pipe_of(&d->factors);
    if (d->overlay) {
        s->u.mat.f.alpha_mode = SU_ASSET_ALPHA_BLEND;
    }
    memset(&u, 0, sizeof u);
    memcpy(u.specular_factor, d->specular_factor, sizeof d->specular_factor);
    u.detail[0] = d->detail_strength;
    u.detail[1] = d->detail_uv_scale[0] == 0.0f ? 1.0f : d->detail_uv_scale[0];
    u.detail[2] = d->detail_uv_scale[1] == 0.0f ? 1.0f : d->detail_uv_scale[1];
    u.rotation[0] = cosf(d->detail_rotation);
    u.rotation[1] = sinf(d->detail_rotation);
    u.rotation[2] = d->textures[SU_PAINT_SLOT_DETAIL_NORMAL] != 0u ? 1.0f : 0.0f;
    u.extra[0] = d->flags;
    u.extra[1] = d->textures[SU_PAINT_SLOT_SPECULAR] != 0u;
    u.extra[2] = d->detail_layer_mask;
    u.extra[3] = d->specular_channels;
    u.a[0] = p->specular;
    u.a[1] = p->dirt_amount;
    u.a[2] = p->dirt_darken;
    u.a[3] = p->dirt_roughen;
    u.b[0] = p->layer_count;
    u.b[1] = (uint32_t)p->layer_order[0] | ((uint32_t)p->layer_order[1] << 8) | ((uint32_t)p->layer_order[2] << 16) |
             ((uint32_t)p->layer_order[3] << 24);
    u.b[2] = (uint32_t)p->channel[SU_PAINT_CH_ROUGHNESS] | ((uint32_t)p->channel[SU_PAINT_CH_METAL] << 8) |
             ((uint32_t)p->channel[SU_PAINT_CH_SPECULAR] << 16) | ((uint32_t)p->channel[SU_PAINT_CH_DIRT] << 24);
    u.b[3] = (uint32_t)p->channel[SU_PAINT_CH_AO] | ((uint32_t)p->channel[SU_PAINT_CH_GLOW] << 8) | (glow_mask << 16);
    for (i = 0; i < p->layer_count; i++) {
        const SuAssetPaintLayer *l = &p->layer[i];
        memcpy(u.layer[i].color_rough, l->color, 3u * sizeof(float));
        u.layer[i].color_rough[3] = l->roughness;
        u.layer[i].mop[0] = l->metallic;
        u.layer[i].mop[1] = l->opacity;
        u.layer[i].mop[2] = (float)l->flags;
        u.layer[i].mop[3] = l->glow_strength;
        memcpy(u.layer[i].glow, l->glow_color, 3u * sizeof(float));
    }
    if (!su_rvk_make_buffer(r, sizeof u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->u.mat.ubo)) {
        Retired t;
        memset(&t, 0, sizeof t);
        t.a = s->u.mat.ubo;
        destroy_retired(r, &t);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "device memory for a paint material");
    }
    memcpy(s->u.mat.ubo.map, &u, sizeof u);
    s->u.mat.set = write_paint_set(r, s->u.mat.tex, s->u.mat.paint_tex_info, &s->u.mat.ubo, &s->u.mat.bound_ready);
    if (s->u.mat.set == VK_NULL_HANDLE) {
        Retired t;
        memset(&t, 0, sizeof t);
        t.a = s->u.mat.ubo;
        destroy_retired(r, &t);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "material descriptor pool exhausted");
    }
    s->state = SU_RENDER_STATE_READY;
    r->stats.resources++;
    *out = make_handle(RES_MATERIAL, s->gen, idx);
    return SU_OK;
}

uint32_t su_render_fx_material_create(SuRenderer *renderer, const SuRenderFxMaterialDesc *desc, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t layer;
    Res *slot;
    if (out != NULL) {
        *out = 0;
    }
    if (renderer == NULL || desc == NULL || out == NULL || desc->blend > SU_FX_BLEND_PREMUL ||
        desc->wrap_u > SU_FX_WRAP_MIRROR || desc->wrap_v > SU_FX_WRAP_MIRROR ||
        !isfinite(desc->soft_distance) || desc->soft_distance < 0.0f || !su_rvk_fx_layers_valid(&desc->layers)) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (layer = 0; layer < 3u; layer++) {
        Res *texture = su_rvk_get(renderer, desc->layers.textures[layer], RES_TEXTURE, NULL);
        if (desc->layers.textures[layer] != 0u && (texture == NULL || texture->u.tex.layers != 1u ||
                                                        texture->u.tex.depth > 1u)) {
            return SU_E_INVALID_ARGUMENT;
        }
    }
    if (alloc_slot(renderer, RES_MATERIAL, &idx) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    slot = &renderer->res[idx];
    slot->u.mat.kind = 2u;
    slot->u.mat.fx = *desc;
    slot->u.mat.pipeline = PIPE_BLEND + desc->blend * 2u + (desc->two_sided != 0u ? 1u : 0u);
    slot->u.mat.f.alpha_mode = SU_ASSET_ALPHA_BLEND;
    slot->state = SU_RENDER_STATE_READY;
    renderer->stats.resources++;
    *out = make_handle(RES_MATERIAL, slot->gen, idx);
    return SU_OK;
}

uint32_t su_render_material_create(SuRenderer *r, const SuRenderMaterialDesc *d, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t i;
    Res *s;
    if (r == NULL || d == NULL || out == NULL || d->factors.alpha_mode > SU_ASSET_ALPHA_BLEND) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0;
    for (i = 0; i < SU_ASSET_SLOT_COUNT; i++) {
        if (d->textures[i] != 0u && su_rvk_get(r, d->textures[i], RES_TEXTURE, NULL) == NULL) {
            return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "material slot %u: unknown texture handle", (unsigned)i);
        }
    }
    if (alloc_slot(r, RES_MATERIAL, &idx) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    s->u.mat.f = d->factors;
    memcpy(s->u.mat.tex, d->textures, sizeof d->textures);
    s->u.mat.pipeline = d->overlay ? PIPE_OVERLAY + (d->factors.double_sided ? 1u : 0u) : pipe_of(&d->factors);
    if (d->overlay) {
        s->u.mat.f.alpha_mode = SU_ASSET_ALPHA_BLEND;
    }
    s->u.mat.set = write_material_set(r, s->u.mat.tex, &s->u.mat.f, &s->u.mat.bound_ready);
    if (s->u.mat.set == VK_NULL_HANDLE) {
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "material descriptor pool exhausted");
    }
    s->state = SU_RENDER_STATE_READY;
    r->stats.resources++;
    *out = make_handle(RES_MATERIAL, s->gen, idx);
    return SU_OK;
}

void su_rvk_refresh_materials(SuRenderer *r) {
    uint32_t i;
    if (!r->tex_ready_this_frame) {
        return;
    }
    for (i = 0; i < r->res_cap; i++) {
        Res *s = &r->res[i];
        uint32_t want = 0;
        uint32_t k;
        if (s->type != RES_MATERIAL || s->u.mat.kind == 2u) {
            continue;
        }
        for (k = 0; k < (s->u.mat.kind != 0u ? SU_RENDER_PAINT_SLOT_COUNT : SU_ASSET_SLOT_COUNT); k++) {
            if (view_of(r, s->u.mat.tex[k]) != VK_NULL_HANDLE) {
                want |= 1u << k;
            }
        }
        if (want != s->u.mat.bound_ready) {
            uint32_t ready = 0;
            VkDescriptorSet n = s->u.mat.kind != 0u ? write_paint_set(r, s->u.mat.tex, s->u.mat.paint_tex_info, &s->u.mat.ubo, &ready)
                                                    : write_material_set(r, s->u.mat.tex, &s->u.mat.f, &ready);
            if (n != VK_NULL_HANDLE) {
                Retired t;
                memset(&t, 0, sizeof t);
                t.set = s->u.mat.set; /* in-flight frames may still bind it */
                retire(r, &t);
                s->u.mat.set = n;
                s->u.mat.bound_ready = ready;
            }
        }
    }
}

VkDescriptorSet su_rvk_material_set(SuRenderer *r, uint32_t mat_index) {
    return mat_index < r->res_cap && r->res[mat_index].type == RES_MATERIAL ? r->res[mat_index].u.mat.set
                                                                            : r->default_mat_set;
}

/* ---------- models ---------- */

static float max_scale(const float *m) {
    float s = 0.0f;
    int c;
    for (c = 0; c < 3; c++) {
        float l = sqrtf(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]);
        s = l > s ? l : s;
    }
    return s;
}

uint32_t su_render_model_create(SuRenderer *r, const SuRenderModelNode *nodes, uint32_t count, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t i;
    uint32_t k;
    Res *s;
    ModelRes *m;
    float lo[3] = {INFINITY, INFINITY, INFINITY};
    float hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    float rr = 0.0f;
    if (r == NULL || nodes == NULL || out == NULL || count == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = 0;
    for (i = 0; i < count; i++) {
        if (nodes[i].lod_count == 0u || nodes[i].lod_count > SU_ASSET_MAX_LODS) {
            return SU_E_INVALID_ARGUMENT;
        }
        for (k = 0; k < nodes[i].lod_count; k++) {
            if (su_rvk_get(r, nodes[i].lod_mesh[k], RES_MESH, NULL) == NULL) {
                return su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "model node %u LOD %u: unknown mesh handle", (unsigned)i,
                                   (unsigned)k);
            }
        }
    }
    if (alloc_slot(r, RES_MODEL, &idx) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    m = &s->u.model;
    m->nodes = malloc((size_t)count * sizeof *m->nodes);
    m->node_center = malloc((size_t)count * 4u * sizeof(float));
    if (m->nodes == NULL || m->node_center == NULL) {
        free(m->nodes);
        free(m->node_center);
        free_slot(r, idx);
        return SU_E_RESOURCE_LIMIT;
    }
    memcpy(m->nodes, nodes, (size_t)count * sizeof *nodes);
    m->count = count;
    for (i = 0; i < count; i++) {
        const MeshRes *mesh = &su_rvk_get(r, nodes[i].lod_mesh[0], RES_MESH, NULL)->u.mesh;
        const float *t = nodes[i].transform;
        float *c = &m->node_center[i * 4u];
        int j;
        for (j = 0; j < 3; j++) {
            c[j] = t[j] * mesh->center[0] + t[4 + j] * mesh->center[1] + t[8 + j] * mesh->center[2] + t[12 + j];
        }
        c[3] = mesh->radius * max_scale(t);
        for (j = 0; j < 3; j++) {
            lo[j] = c[j] - c[3] < lo[j] ? c[j] - c[3] : lo[j];
            hi[j] = c[j] + c[3] > hi[j] ? c[j] + c[3] : hi[j];
        }
    }
    for (k = 0; k < 3u; k++) {
        m->center[k] = 0.5f * (lo[k] + hi[k]);
    }
    for (i = 0; i < count; i++) {
        const float *c = &m->node_center[i * 4u];
        float dx = c[0] - m->center[0];
        float dy = c[1] - m->center[1];
        float dz = c[2] - m->center[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz) + c[3];
        rr = d > rr ? d : rr;
    }
    m->radius = rr;
    s->state = SU_RENDER_STATE_READY;
    r->stats.resources++;
    *out = make_handle(RES_MODEL, s->gen, idx);
    return SU_OK;
}

/* ---------- environment cubes ---------- */

/* Takes ownership of half (size and mips as su_env_half_count / su_env_mip_count; freed on failure too). */
static uint32_t env_create(SuRenderer *r, uint32_t size, uint32_t mips, uint16_t *half, int internal, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t m;
    Res *s;
    TexRes *t;
    VkFormatProperties fp;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    uint8_t *p = (uint8_t *)half;
    *out = 0;
    if (alloc_slot(r, RES_TEXTURE, &idx) != SU_OK) {
        free(half);
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    s->internal = (uint8_t)internal;
    t = &s->u.tex;
    t->format = TEXFMT_ENV;
    t->vkfmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    t->width = t->height = size;
    t->mips = mips;
    t->cube_mips = mips;
    t->layers = 6u;
    r->f.vkGetPhysicalDeviceFormatProperties(r->pd, t->vkfmt, &fp);
    if ((fp.optimalTilingFeatures & need) != need) {
        free(half);
        s->state = SU_RENDER_STATE_FAILED;
        (void)su_rvk_fail(r, SU_E_UNSUPPORTED_METHOD, "RGBA16F cube images cannot be filtered on this device");
        if (!internal) {
            r->stats.resources++;
        }
        *out = make_handle(RES_TEXTURE, s->gen, idx);
        return SU_OK;
    }
    t->owned = p;
    for (m = 0; m < mips; m++) {
        uint32_t f;
        for (f = 0; f < 6u; f++) {
            t->src[m * 6u + f] = p;
            p += level_bytes(TEXFMT_ENV, size >> m, size >> m);
        }
    }
    if (!make_texture_image(r, t)) {
        Retired rt;
        memset(&rt, 0, sizeof rt);
        rt.img = t->img;
        rt.mem = t->mem;
        rt.view = t->view;
        destroy_retired(r, &rt);
        free(t->owned);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "device memory for a %ux%u environment cube", (unsigned)size, (unsigned)size);
    }
    if (!make_cube_set(r, t)) {
        Retired rt;
        memset(&rt, 0, sizeof rt);
        rt.img = t->img;
        rt.mem = t->mem;
        rt.view = t->view;
        destroy_retired(r, &rt);
        free(t->owned);
        free_slot(r, idx);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "descriptor pool exhausted");
    }
    enqueue(r, idx);
    if (!internal) {
        r->stats.resources++;
    }
    *out = make_handle(RES_TEXTURE, s->gen, idx);
    return SU_OK;
}

static void image_barrier(SuRenderer *r, VkCommandBuffer cmd, VkImage img, uint32_t mip0, uint32_t mips, VkImageLayout from,
                          VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = mip0;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = 6;
    r->f.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* The GPU prefilter (see render.h): source chain by blits, mip 0 copied, mips 1.. rendered face by face. */
static uint32_t env_create_gpu(SuRenderer *r, uint32_t size, uint32_t mips, const float *faces, SuRenderHandle *out) {
    uint32_t idx = 0;
    uint32_t m;
    uint32_t f;
    uint32_t nviews = 0;
    uint32_t st = SU_OK;
    Res *s;
    TexRes *t;
    TexRes src;
    Buffer staging;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkImageView views[6u * 16u];
    VkFramebuffer fbs[6u * 16u];
    size_t texels = (size_t)6u * size * size;
    size_t i;
    uint16_t *h;
    *out = 0;
    memset(&src, 0, sizeof src);
    memset(&staging, 0, sizeof staging);
    memset(views, 0, sizeof views);
    memset(fbs, 0, sizeof fbs);
    if (!su_rvk_make_buffer(r, (VkDeviceSize)texels * 8u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) {
        su_rvk_free_buffer(r, &staging);
        return su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "staging memory for a %ux%u environment cube", (unsigned)size, (unsigned)size);
    }
    h = (uint16_t *)staging.map;
    for (i = 0; i < texels; i++) { /* the CPU path's input rule: NaN, negative and > 65000 read as 0 */
        int k;
        for (k = 0; k < 3; k++) {
            float v = faces[i * 4u + (size_t)k];
            h[i * 4u + (size_t)k] = su_float_to_half(v == v && v < 65000.0f ? (v > 0.0f ? v : 0.0f) : 0.0f);
        }
        h[i * 4u + 3u] = su_float_to_half(1.0f);
    }
    if (alloc_slot(r, RES_TEXTURE, &idx) != SU_OK) {
        su_rvk_free_buffer(r, &staging);
        return SU_E_RESOURCE_LIMIT;
    }
    s = &r->res[idx];
    t = &s->u.tex;
    t->format = TEXFMT_ENV;
    t->vkfmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    t->width = t->height = size;
    t->mips = mips;
    t->cube_mips = mips;
    t->layers = 6u;
    src = *t;
    if (!make_texture_image_usage(r, t, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !make_texture_image_usage(r, &src, VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        st = su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "device memory for a %ux%u environment cube", (unsigned)size, (unsigned)size);
        goto done;
    }
    {
        VkDescriptorSetAllocateInfo ai;
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet w;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = r->pool_mat;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->tex_dsl;
        if (r->f.vkAllocateDescriptorSets(r->dev, &ai, &set) != VK_SUCCESS) {
            set = VK_NULL_HANDLE;
            st = su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "descriptor pool exhausted");
            goto done;
        }
        ii.sampler = r->samplers[4];
        ii.imageView = src.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
    }
    for (m = 1; m < mips && st == SU_OK; m++) {
        for (f = 0; f < 6u; f++) {
            VkImageViewCreateInfo vi;
            VkFramebufferCreateInfo fi;
            memset(&vi, 0, sizeof vi);
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = t->img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = t->vkfmt;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel = m;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.baseArrayLayer = f;
            vi.subresourceRange.layerCount = 1;
            if (r->f.vkCreateImageView(r->dev, &vi, NULL, &views[nviews]) != VK_SUCCESS) {
                st = su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "environment prefilter views");
                break;
            }
            memset(&fi, 0, sizeof fi);
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = r->bloom_pass;
            fi.attachmentCount = 1;
            fi.pAttachments = &views[nviews];
            fi.width = fi.height = size >> m;
            fi.layers = 1;
            nviews++;
            if (r->f.vkCreateFramebuffer(r->dev, &fi, NULL, &fbs[nviews - 1u]) != VK_SUCCESS) {
                st = su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "environment prefilter framebuffers");
                break;
            }
        }
    }
    if (st != SU_OK) {
        goto done;
    }
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = r->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (r->f.vkAllocateCommandBuffers(r->dev, &cai, &cmd) != VK_SUCCESS) {
        cmd = VK_NULL_HANDLE;
        st = su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "command buffer for the environment prefilter");
        goto done;
    }
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    (void)r->f.vkBeginCommandBuffer(cmd, &bi);
    /* source mip 0 from the staging buffer, then the box chain by 2:1 linear blits */
    image_barrier(r, cmd, src.img, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    {
        VkBufferImageCopy c;
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 6;
        c.imageExtent.width = c.imageExtent.height = size;
        c.imageExtent.depth = 1;
        r->f.vkCmdCopyBufferToImage(cmd, staging.buf, src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    for (m = 1; m < mips; m++) {
        VkImageBlit b;
        image_barrier(r, cmd, src.img, m - 1u, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = m - 1u;
        b.dstSubresource.mipLevel = m;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 6;
        b.srcOffsets[1].x = b.srcOffsets[1].y = (int32_t)(size >> (m - 1u));
        b.dstOffsets[1].x = b.dstOffsets[1].y = (int32_t)(size >> m);
        b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
        r->f.vkCmdBlitImage(cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                            &b, VK_FILTER_LINEAR);
    }
    image_barrier(r, cmd, src.img, mips - 1u, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* target mip 0 = the source; mips 1.. rendered (the pass leaves each shader-readable) */
    image_barrier(r, cmd, t->img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    {
        VkImageCopy c;
        memset(&c, 0, sizeof c);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 6;
        c.extent.width = c.extent.height = size;
        c.extent.depth = 1;
        r->f.vkCmdCopyImage(cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                            &c);
    }
    image_barrier(r, cmd, t->img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(r, cmd, src.img, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->env_pf_pipe);
    r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->bloom_layout, 0, 1, &set, 0, NULL);
    nviews = 0;
    for (m = 1; m < mips; m++) {
        uint32_t ms = size >> m;
        for (f = 0; f < 6u; f++) {
            VkRenderPassBeginInfo rp;
            VkViewport vp;
            VkRect2D sc;
            BloomPush pp;
            memset(&rp, 0, sizeof rp);
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = r->bloom_pass;
            rp.framebuffer = fbs[nviews++];
            rp.renderArea.extent.width = rp.renderArea.extent.height = ms;
            r->f.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            memset(&vp, 0, sizeof vp);
            vp.width = vp.height = (float)ms;
            vp.maxDepth = 1.0f;
            memset(&sc, 0, sizeof sc);
            sc.extent.width = sc.extent.height = ms;
            r->f.vkCmdSetViewport(cmd, 0, 1, &vp);
            r->f.vkCmdSetScissor(cmd, 0, 1, &sc);
            memset(&pp, 0, sizeof pp);
            pp.a[0] = (float)f;
            pp.a[1] = (float)ms;
            pp.a[2] = (float)m / (float)(mips - 1u);
            pp.a[3] = 4.0f * 3.14159265358979f / (6.0f * (float)size * (float)size);
            pp.b[0] = (float)(mips - 1u);
            r->f.vkCmdPushConstants(cmd, r->bloom_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof pp, &pp);
            r->f.vkCmdDraw(cmd, 3, 1, 0, 0);
            r->f.vkCmdEndRenderPass(cmd);
        }
    }
    (void)r->f.vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (r->f.vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS || r->f.vkQueueWaitIdle(r->queue) != VK_SUCCESS) {
        st = su_rvk_fail(r, SU_E_IO, "the environment prefilter submission failed");
        goto done;
    }
    if (!make_cube_set(r, t)) {
        st = su_rvk_fail(r, SU_E_RESOURCE_LIMIT, "descriptor pool exhausted");
        goto done;
    }
done:
    if (cmd != VK_NULL_HANDLE) {
        r->f.vkFreeCommandBuffers(r->dev, r->pool, 1, &cmd);
    }
    for (i = 0; i < (size_t)(6u * 16u); i++) {
        if (fbs[i] != VK_NULL_HANDLE) {
            r->f.vkDestroyFramebuffer(r->dev, fbs[i], NULL);
        }
        if (views[i] != VK_NULL_HANDLE) {
            r->f.vkDestroyImageView(r->dev, views[i], NULL);
        }
    }
    if (set != VK_NULL_HANDLE) {
        (void)r->f.vkFreeDescriptorSets(r->dev, r->pool_mat, 1, &set);
    }
    {
        Retired rt;
        memset(&rt, 0, sizeof rt);
        rt.img = src.img;
        rt.mem = src.mem;
        rt.view = src.view;
        destroy_retired(r, &rt);
    }
    su_rvk_free_buffer(r, &staging);
    if (st != SU_OK) {
        Retired rt;
        memset(&rt, 0, sizeof rt);
        rt.img = t->img;
        rt.mem = t->mem;
        rt.view = t->view;
        destroy_retired(r, &rt);
        free_slot(r, idx);
        return st;
    }
    s->state = SU_RENDER_STATE_READY;
    r->stats.resources++;
    *out = make_handle(RES_TEXTURE, s->gen, idx);
    return SU_OK;
}

uint32_t su_render_env_create_ex(SuRenderer *r, uint32_t size, const float *faces, uint32_t flags, SuRenderHandle *out) {
    uint32_t mips = su_env_mip_count(size);
    uint16_t *half;
    uint32_t st;
    double t0;
    if (r == NULL || faces == NULL || out == NULL || mips == 0u || (flags & ~SU_ENV_CPU) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    t0 = su_rvk_now_ms();
    if ((flags & SU_ENV_CPU) == 0u && r->env_gpu) {
        st = env_create_gpu(r, size, mips, faces, out);
        r->stats.env_prefilter_ms = su_rvk_now_ms() - t0;
        r->stats.env_prefilter_gpu = 1;
        return st;
    }
    half = malloc((size_t)su_env_half_count(size) * sizeof *half);
    if (half == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (!su_env_prefilter(size, faces, half)) {
        free(half);
        return SU_E_RESOURCE_LIMIT;
    }
    r->stats.env_prefilter_ms = su_rvk_now_ms() - t0;
    r->stats.env_prefilter_gpu = 0;
    return env_create(r, size, mips, half, 0, out);
}

uint32_t su_render_env_create(SuRenderer *r, uint32_t size, const float *faces, SuRenderHandle *out) {
    return su_render_env_create_ex(r, size, faces, 0u, out);
}

uint32_t su_render_env_from_sky(SuRenderer *r, uint32_t size, const SuRenderLighting *light, SuRenderHandle *out) {
    float *faces;
    uint32_t st;
    if (r == NULL || light == NULL || out == NULL || su_env_mip_count(size) == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    faces = malloc((size_t)6u * size * size * 4u * sizeof *faces);
    if (faces == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    su_env_sky_faces(size, light, faces);
    st = su_render_env_create(r, size, faces, out);
    free(faces);
    return st;
}

uint32_t su_render_texture_create_dds(SuRenderer *r, const SuDdsImage *img, uint32_t flags, SuRenderHandle *out) {
    const uint8_t *ptrs[SU_ASSET_MAX_MIPS * 6u];
    SuRenderTextureDesc d;
    if (r == NULL || img == NULL || out == NULL || (img->faces != 1u && img->faces != 6u) || img->mip_count == 0u ||
        img->mip_count > SU_ASSET_MAX_MIPS) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_dds_level_table(img, ptrs);
    memset(&d, 0, sizeof d);
    d.width = img->width;
    d.height = img->height;
    d.format = img->format;
    d.mip_count = img->mip_count;
    d.mip_data = ptrs;
    d.flags = flags;
    d.layers = img->faces;
    return texture_create(r, &d, 0, out);
}

uint32_t su_render_env_from_dds_ex(SuRenderer *r, const SuDdsImage *img, uint32_t max_size, SuRenderHandle *out) {
    uint32_t m;
    uint32_t f;
    uint32_t pick = UINT32_MAX;
    uint32_t src = 0;
    uint32_t size;
    uint32_t st = SU_OK;
    float *faces;
    float *tmp = NULL;
    if (max_size == 0u) {
        max_size = 1024u;
    }
    if (r == NULL || img == NULL || out == NULL || img->faces != 6u || su_env_mip_count(max_size) == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    /* the largest square power-of-two mip in 8..max_size, else the smallest larger one (downsampled) */
    for (m = 0; m < img->mip_count; m++) {
        uint32_t w = img->width >> m;
        uint32_t h = img->height >> m;
        if (w != h || w < 8u || (w & (w - 1u)) != 0u) {
            continue;
        }
        if (w <= max_size) {
            pick = m;
            src = w;
            break;
        }
        pick = m;
        src = w;
    }
    if (pick == UINT32_MAX) {
        return SU_E_INVALID_ARGUMENT;
    }
    size = src < max_size ? src : max_size;
    faces = malloc((size_t)6u * size * size * 4u * sizeof *faces);
    if (faces == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (src != size) {
        tmp = malloc((size_t)src * src * 4u * sizeof *tmp);
        if (tmp == NULL) {
            free(faces);
            return SU_E_RESOURCE_LIMIT;
        }
    }
    for (f = 0; f < 6u && st == SU_OK; f++) {
        float *dst = faces + (size_t)f * size * size * 4u;
        if (tmp == NULL) {
            st = su_dds_decode_rgba32f(img, f, pick, dst);
            continue;
        }
        st = su_dds_decode_rgba32f(img, f, pick, tmp);
        if (st == SU_OK) { /* box filter: each output texel is the mean of a k x k block (k a power of two) */
            uint32_t k = src / size;
            float inv = 1.0f / (float)(k * k);
            uint32_t x;
            uint32_t y;
            for (y = 0; y < size; y++) {
                for (x = 0; x < size; x++) {
                    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    uint32_t i;
                    uint32_t j;
                    uint32_t c;
                    for (j = 0; j < k; j++) {
                        const float *row = tmp + ((size_t)(y * k + j) * src + (size_t)x * k) * 4u;
                        for (i = 0; i < k; i++) {
                            for (c = 0; c < 4u; c++) {
                                acc[c] += row[i * 4u + c];
                            }
                        }
                    }
                    for (c = 0; c < 4u; c++) {
                        dst[((size_t)y * size + x) * 4u + c] = acc[c] * inv;
                    }
                }
            }
        }
    }
    free(tmp);
    if (st == SU_OK) {
        st = su_render_env_create(r, size, faces, out);
    }
    free(faces);
    return st;
}

uint32_t su_render_env_from_dds(SuRenderer *r, const SuDdsImage *img, SuRenderHandle *out) {
    return su_render_env_from_dds_ex(r, img, 0u, out);
}

VkDescriptorSet su_rvk_env_set(SuRenderer *r, SuRenderHandle env, float intensity, float *lod_out, float *intensity_out) {
    Res *s = env != 0u ? su_rvk_get(r, env, RES_TEXTURE, NULL) : NULL;
    if (s != NULL && !s->internal && s->u.tex.layers == 6u && s->state == SU_RENDER_STATE_READY) {
        *lod_out = (float)(s->u.tex.cube_mips - 1u);
        *intensity_out = intensity > 0.0f ? intensity : 1.0f;
        return s->u.tex.env_set;
    }
    *lod_out = 0.0f;
    *intensity_out = 0.0f;
    return r->res[(uint32_t)(r->default_env & 0xFFFFFFFFu) - 1u].u.tex.env_set;
}

/* ---------- built-in textures and sets ---------- */

uint32_t su_rvk_internal_textures(SuRenderer *r) {
    static const uint8_t white[4] = {255, 255, 255, 255};
    static const uint8_t flat[4] = {128, 128, 255, 255};
    const uint8_t *mips[1];
    SuRenderTextureDesc d;
    uint32_t i;
    uint32_t ready = 0;
    memset(&d, 0, sizeof d);
    d.width = d.height = 1;
    d.format = SU_TEX_RGBA8_UNORM;
    d.mip_count = 1;
    d.mip_data = mips;
    mips[0] = white;
    if (texture_create(r, &d, 1, &r->white_tex) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    mips[0] = flat;
    if (texture_create(r, &d, 1, &r->normal_tex) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    {
        static const uint8_t clear[4] = {0, 0, 0, 0};
        uint16_t *black = calloc((size_t)6u * 4u * 4u * 4u, sizeof *black);
        mips[0] = clear;
        if (texture_create(r, &d, 1, &r->black_tex) != SU_OK || black == NULL ||
            env_create(r, 4u, 1u, black, 1, &r->default_env) != SU_OK ||
            r->res[(uint32_t)(r->default_env & 0xFFFFFFFFu) - 1u].state == SU_RENDER_STATE_FAILED) {
            return SU_E_RESOURCE_LIMIT;
        }
    }
    r->font_pixels = malloc(SU_FONT_ATLAS_W * SU_FONT_ATLAS_H * 4u);
    if (r->font_pixels == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    {
        uint8_t *cov = malloc(SU_FONT_ATLAS_W * SU_FONT_ATLAS_H);
        if (cov == NULL) {
            return SU_E_RESOURCE_LIMIT;
        }
        su_font_atlas(cov);
        for (i = 0; i < SU_FONT_ATLAS_W * SU_FONT_ATLAS_H; i++) {
            r->font_pixels[i * 4u] = r->font_pixels[i * 4u + 1u] = r->font_pixels[i * 4u + 2u] = cov[i];
            r->font_pixels[i * 4u + 3u] = 255u;
        }
        free(cov);
    }
    d.width = SU_FONT_ATLAS_W;
    d.height = SU_FONT_ATLAS_H;
    mips[0] = r->font_pixels;
    if (texture_create(r, &d, 1, &r->font_tex) != SU_OK) {
        return SU_E_RESOURCE_LIMIT;
    }
    r->default_mat_set = write_material_set(r, NULL, NULL, &ready);
    if (r->default_mat_set == VK_NULL_HANDLE) {
        return SU_E_RESOURCE_LIMIT;
    }
    {
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet w;
        ii.sampler = r->nearest;
        ii.imageView = r->res[(uint32_t)(r->font_tex & 0xFFFFFFFFu) - 1u].u.tex.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = r->font_set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        r->f.vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
    }
    return SU_OK;
}

/* ---------- packs ---------- */

static uint32_t pak_add(SuRenderPak *p, SuRenderHandle h, uint32_t cap) {
    if (p->count >= cap) {
        return SU_E_INTERNAL;
    }
    p->handles[p->count++] = h;
    return SU_OK;
}

/* Handle of a pack resource id among those created so far (linear; packs are small at this stage). */
static SuRenderHandle pak_lookup(const SuRenderPak *p, const uint64_t *ids, uint64_t id) {
    uint32_t i;
    for (i = 0; i < p->count; i++) {
        if (ids[i] == id) {
            return p->handles[i];
        }
    }
    return 0;
}

uint32_t su_render_upload_pak(SuRenderer *r, const SuAssetPak *pak, uint64_t model_id, SuRenderPak *out) {
    uint32_t n;
    uint32_t i;
    uint32_t pass;
    uint64_t *ids;
    uint32_t st = SU_OK;
    if (r == NULL || pak == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    n = su_asset_pak_entry_count(pak);
    out->handles = malloc((size_t)(n + 1u) * sizeof *out->handles);
    ids = malloc((size_t)(n + 1u) * sizeof *ids);
    if (out->handles == NULL || ids == NULL) {
        free(out->handles);
        free(ids);
        out->handles = NULL;
        return SU_E_RESOURCE_LIMIT;
    }
    out->ids = ids;
    /* dependency order: textures, materials, meshes, models */
    for (pass = SU_ASSET_TEXTURE; st == SU_OK; pass = pass == SU_ASSET_TEXTURE    ? SU_ASSET_MATERIAL
                                                    : pass == SU_ASSET_MATERIAL ? SU_ASSET_MESH
                                                    : pass == SU_ASSET_MESH     ? SU_ASSET_MODEL
                                                                                : 0u) {
        if (pass == 0u) {
            break;
        }
        for (i = 0; i < n && st == SU_OK; i++) {
            SuAssetEntry e;
            SuRenderHandle h = 0;
            (void)su_asset_pak_entry(pak, i, &e);
            if (e.type != pass) {
                continue;
            }
            if (pass == SU_ASSET_TEXTURE) {
                SuAssetTextureView tv;
                SuRenderTextureDesc d;
                (void)su_asset_pak_texture(pak, e.id, &tv);
                memset(&d, 0, sizeof d);
                d.width = tv.header->width;
                d.height = tv.header->height;
                d.format = tv.header->format;
                d.mip_count = tv.header->mip_count;
                d.mip_data = tv.mip_data;
                st = su_render_texture_create(r, &d, &h);
                out->textures++;
            } else if (pass == SU_ASSET_MATERIAL) {
                SuRenderMaterialDesc d;
                uint32_t k;
                memset(&d, 0, sizeof d);
                SuRenderPaintDesc pd;
                memset(&pd, 0, sizeof pd);
                (void)su_asset_pak_material(pak, e.id, &d.factors);
                if (su_asset_pak_paint(pak, e.id, &pd.paint) == SU_OK) {
                    pd.factors = d.factors;
                    for (k = 0; k < SU_PAINT_SLOT_COUNT; k++) {
                        pd.textures[k] = pd.paint.texture[k] != 0u ? pak_lookup(out, ids, pd.paint.texture[k]) : 0u;
                    }
                    st = su_render_paint_material_create(r, &pd, &h);
                } else {
                    for (k = 0; k < SU_ASSET_SLOT_COUNT; k++) {
                        d.textures[k] = d.factors.texture[k] != 0u ? pak_lookup(out, ids, d.factors.texture[k]) : 0u;
                    }
                    st = su_render_material_create(r, &d, &h);
                }
                out->materials++;
            } else if (pass == SU_ASSET_MESH) {
                SuAssetMeshView mv;
                SuRenderMeshDesc d;
                SuRenderHandle *mats;
                uint32_t k;
                (void)su_asset_pak_mesh(pak, e.id, &mv);
                mats = malloc((size_t)mv.header->submesh_count * sizeof *mats);
                if (mats == NULL) {
                    st = SU_E_RESOURCE_LIMIT;
                    break;
                }
                for (k = 0; k < mv.header->submesh_count; k++) {
                    mats[k] = mv.submeshes[k].material != 0u ? pak_lookup(out, ids, mv.submeshes[k].material) : 0u;
                }
                memset(&d, 0, sizeof d);
                d.vertices = mv.vertices;
                d.vertex_count = mv.header->vertex_count;
                d.indices = mv.indices;
                d.index_count = mv.header->index_count;
                d.submeshes = mv.submeshes;
                d.submesh_count = mv.header->submesh_count;
                d.materials = mats;
                memcpy(d.center, mv.header->center, sizeof d.center);
                d.radius = mv.header->radius;
                st = su_render_mesh_create(r, &d, &h);
                free(mats);
                out->meshes++;
            } else {
                SuAssetModelView mv;
                SuRenderModelNode *nodes;
                uint32_t k;
                uint32_t j;
                (void)su_asset_pak_model(pak, e.id, &mv);
                nodes = calloc(mv.header->node_count, sizeof *nodes);
                if (nodes == NULL) {
                    st = SU_E_RESOURCE_LIMIT;
                    break;
                }
                for (k = 0; k < mv.header->node_count; k++) {
                    memcpy(nodes[k].transform, mv.nodes[k].transform, sizeof nodes[k].transform);
                    nodes[k].lod_count = mv.nodes[k].lod_count;
                    for (j = 0; j < mv.nodes[k].lod_count; j++) {
                        nodes[k].lod_mesh[j] = pak_lookup(out, ids, mv.nodes[k].lod_mesh[j]);
                        nodes[k].lod_screen[j] = mv.nodes[k].lod_screen[j];
                    }
                }
                st = su_render_model_create(r, nodes, mv.header->node_count, &h);
                free(nodes);
                out->models++;
                if (st == SU_OK && (out->model == 0u && (model_id == 0u || model_id == e.id))) {
                    out->model = h;
                }
            }
            if (st == SU_OK) {
                ids[out->count] = e.id;
                st = pak_add(out, h, n + 1u);
            }
        }
    }
    out->ids = ids;
    if (st == SU_OK && out->model == 0u) {
        st = su_rvk_fail(r, SU_E_INVALID_ARGUMENT, "the pack has no model%s", model_id != 0u ? " with that id" : "");
    }
    if (st != SU_OK) {
        su_render_pak_release(r, out);
    }
    return st;
}

uint32_t su_render_pak_state(const SuRenderer *r, const SuRenderPak *p) {
    uint32_t i;
    uint32_t st = SU_RENDER_STATE_READY;
    if (r == NULL || p == NULL || p->handles == NULL) {
        return SU_RENDER_STATE_INVALID;
    }
    for (i = 0; i < p->count; i++) {
        uint32_t s = su_render_resource_state(r, p->handles[i]);
        if (s == SU_RENDER_STATE_FAILED || s == SU_RENDER_STATE_INVALID) {
            return SU_RENDER_STATE_FAILED;
        }
        if (s != SU_RENDER_STATE_READY) {
            st = SU_RENDER_STATE_PENDING;
        }
    }
    return st;
}

void su_render_pak_release(SuRenderer *r, SuRenderPak *p) {
    uint32_t i;
    if (r == NULL || p == NULL) {
        return;
    }
    /* models first, then what they reference (any order is safe: retirement waits for the frames) */
    for (i = p->count; i > 0u; i--) {
        (void)su_render_release(r, p->handles[i - 1u]);
    }
    free(p->handles);
    free(p->ids);
    memset(p, 0, sizeof *p);
}

SuRenderHandle su_render_pak_handle(const SuRenderPak *p, uint64_t id) {
    return p != NULL && p->handles != NULL && p->ids != NULL ? pak_lookup(p, p->ids, id) : 0u;
}
