/* Camera-relative scene of su_render_frame_ex: per-object and per-node frustum culling of bounding spheres,
 * distance LOD by projected size, and batching of node x submesh instances into instanced draws.
 *
 * Every object position is a Q32.32 world position; the camera's is subtracted in integers (su_wpos_delta) before
 * anything becomes a float, so precision depends on the distance to the camera, not on the distance to the world
 * origin. Draw items are sorted by pipeline, material, mesh and submesh (blended ones back to front) and consecutive
 * equal items share one vkCmdDrawIndexed; their instance records are written in that order. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "render/su_render_vk_int.h"

static const DrawItem *g_sort_items; /* qsort context (one renderer per thread) */

static void fx_mesh_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t first, uint32_t run,
                         const SuRenderFrameDesc *frame);

static int cmp_items(const void *a, const void *b) {
    const DrawItem *x = &g_sort_items[*(const uint32_t *)a];
    const DrawItem *y = &g_sort_items[*(const uint32_t *)b];
    int xb = x->pipe >= PIPE_BLEND ? 2 : x->pipe >= PIPE_OVERLAY ? 1 : 0;
    int yb = y->pipe >= PIPE_BLEND ? 2 : y->pipe >= PIPE_OVERLAY ? 1 : 0;
    if (xb != yb) {
        return xb - yb;
    }
    if (xb) {
        if (x->dist2 != y->dist2) {
            return x->dist2 > y->dist2 ? -1 : 1; /* far first */
        }
        /* equal distance: keep submission order, so the result never depends on qsort's tie handling */
        return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : *(const uint32_t *)a > *(const uint32_t *)b;
    }
    return x->key < y->key ? -1 : x->key > y->key;
}

static float len3(const float *v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void su_rvk_scene_build(SuRenderer *r, Frame *fr, const SuRenderFrameDesc *f, const SuMat4f *view_proj, float proj_y,
                        uint32_t *item_count) {
    SuFrustum fru;
    uint32_t n = 0;
    uint32_t dropped = 0;
    uint32_t o;
    uint32_t i;
    DrawInstance *dst = fr->draw_inst.map;
    su_frustum_from_matrix(&fru, *view_proj, 1);
    r->stats.objects = f->object_count;
    r->stats.objects_culled = r->stats.nodes_drawn = r->stats.nodes_culled = r->stats.nodes_pending = 0;
    memset(r->stats.lod_nodes, 0, sizeof r->stats.lod_nodes);
    for (o = 0; o < f->object_count; o++) {
        const SuRenderObject *obj = &f->objects[o];
        Res *ms = su_rvk_get(r, obj->model, RES_MODEL, NULL);
        const ModelRes *model;
        SuVec3f rel;
        SuMat4f rot;
        float sr[9]; /* scale * rotation, column-major 3x3 */
        float s = obj->scale > 0.0f ? obj->scale : 1.0f;
        float axis[3] = {1.0f, 1.0f, 1.0f}; /* scale3, all zero = uniform */
        float smax;
        SuVec3f c;
        int k;
        if (ms == NULL || !(obj->tint[3] > 0.0f)) { /* unknown model, or fully transparent (or NaN) */
            r->stats.objects_culled++;
            continue;
        }
        if (obj->scale3[0] != 0.0f || obj->scale3[1] != 0.0f || obj->scale3[2] != 0.0f) {
            if (!(obj->scale3[0] > 0.0f && obj->scale3[1] > 0.0f && obj->scale3[2] > 0.0f) ||
                !(obj->scale3[0] < 1e30f && obj->scale3[1] < 1e30f && obj->scale3[2] < 1e30f)) {
                r->stats.objects_culled++; /* a zero, negative or non-finite axis: not drawn */
                continue;
            }
            axis[0] = obj->scale3[0];
            axis[1] = obj->scale3[1];
            axis[2] = obj->scale3[2];
        }
        smax = s * (axis[0] > axis[1] ? (axis[0] > axis[2] ? axis[0] : axis[2]) : (axis[1] > axis[2] ? axis[1] : axis[2]));
        model = &ms->u.model;
        rel = su_wpos_delta(obj->pos, f->camera.pos);
        rot = su_mat4f_from_quat(obj->rot);
        for (k = 0; k < 3; k++) { /* R * diag(s * scale3): column j of R times the scale of local axis j */
            sr[k] = rot.m[k] * s * axis[0];
            sr[3 + k] = rot.m[4 + k] * s * axis[1];
            sr[6 + k] = rot.m[8 + k] * s * axis[2];
        }
        c.x = rel.x + sr[0] * model->center[0] + sr[3] * model->center[1] + sr[6] * model->center[2];
        c.y = rel.y + sr[1] * model->center[0] + sr[4] * model->center[1] + sr[7] * model->center[2];
        c.z = rel.z + sr[2] * model->center[0] + sr[5] * model->center[1] + sr[8] * model->center[2];
        if (!su_frustum_sphere(&fru, c, model->radius * smax)) {
            r->stats.objects_culled++;
            continue;
        }
        for (i = 0; i < model->count; i++) {
            const SuRenderModelNode *nd = &model->nodes[i];
            const float *nc = &model->node_center[i * 4u];
            const float *t = nd->transform;
            float ncw[3];
            float nr = nc[3] * smax;
            uint32_t lod;
            uint32_t pick = UINT32_MAX;
            uint32_t mesh_index = 0;
            Res *mesh = NULL;
            float m[12]; /* rows of the camera-relative affine transform */
            uint32_t sub;
            uint32_t light_mask[4];
            int a;
            int b;
            for (k = 0; k < 3; k++) {
                ncw[k] = (&rel.x)[k] + sr[k] * nc[0] + sr[3 + k] * nc[1] + sr[6 + k] * nc[2];
            }
            if (model->count > 1u) {
                SuVec3f p;
                p.x = ncw[0];
                p.y = ncw[1];
                p.z = ncw[2];
                if (!su_frustum_sphere(&fru, p, nr)) {
                    r->stats.nodes_culled++;
                    continue;
                }
            }
            lod = su_lod_select(nd->lod_screen, nd->lod_count, nr, len3(ncw), proj_y);
            /* the selected LOD, else the nearest READY one (finer first) */
            for (k = 0; k < (int)nd->lod_count && pick == UINT32_MAX; k++) {
                int cand[2];
                int q;
                cand[0] = (int)lod - k;
                cand[1] = (int)lod + k;
                for (q = 0; q < (k == 0 ? 1 : 2); q++) {
                    Res *x;
                    uint32_t xi;
                    if (cand[q] < 0 || cand[q] >= (int)nd->lod_count || pick != UINT32_MAX) {
                        continue;
                    }
                    x = su_rvk_get(r, nd->lod_mesh[cand[q]], RES_MESH, &xi);
                    if (x != NULL && x->state == SU_RENDER_STATE_READY) {
                        pick = (uint32_t)cand[q];
                        mesh = x;
                        mesh_index = xi;
                    }
                }
            }
            if (mesh == NULL) {
                r->stats.nodes_pending++;
                continue;
            }
            r->stats.nodes_drawn++;
            r->stats.lod_nodes[pick]++;
            su_point_lights_mask(((const FrameUbo *)fr->ubo.map)->point_lights, f->point_light_count, ncw, nr,
                                 light_mask);
            /* M = [sR | rel] * node transform */
            for (a = 0; a < 3; a++) {
                for (b = 0; b < 3; b++) {
                    m[a * 4 + b] = sr[a] * t[b * 4] + sr[3 + a] * t[b * 4 + 1] + sr[6 + a] * t[b * 4 + 2];
                }
                m[a * 4 + 3] = sr[a] * t[12] + sr[3 + a] * t[13] + sr[6 + a] * t[14] + (&rel.x)[a];
            }
            for (sub = 0; sub < mesh->u.mesh.sub_count; sub++) {
                DrawItem *it;
                uint32_t mat_index = UINT32_MAX;
                Res *mat = su_rvk_get(r, mesh->u.mesh.subs[sub].material, RES_MATERIAL, &mat_index);
                if (n == r->max_draw) {
                    dropped++;
                    continue;
                }
                if (mat == NULL) {
                    mat_index = UINT32_MAX;
                }
                it = &r->items[n];
                it->pipe = mat != NULL ? mat->u.mat.pipeline : PIPE_OPAQUE;
                it->kind = mat != NULL ? mat->u.mat.kind : 0u;
                it->forced = 0;
                if (obj->tint[3] < 1.0f && it->pipe < PIPE_OVERLAY) {
                    /* translucent instance of an opaque or masked material: the blend pass, same face culling */
                    it->pipe = (it->pipe & 1u) != 0u ? PIPE_BLEND_2S : PIPE_BLEND;
                    it->forced = 1;
                }
                it->mat = mat_index;
                it->mesh = mesh_index;
                it->sub = sub;
                it->key = ((uint64_t)it->kind << 63) | ((uint64_t)it->pipe << 60) | ((uint64_t)(mat_index & 0xFFFFFu) << 40) |
                          ((uint64_t)(mesh_index & 0xFFFFFFu) << 16) | (uint64_t)(sub & 0xFFFFu);
                it->dist2 = ncw[0] * ncw[0] + ncw[1] * ncw[1] + ncw[2] * ncw[2];
                memcpy(it->inst.r0, &m[0], sizeof it->inst.r0);
                memcpy(it->inst.r1, &m[4], sizeof it->inst.r1);
                memcpy(it->inst.r2, &m[8], sizeof it->inst.r2);
                memcpy(it->inst.tint, obj->tint, sizeof it->inst.tint);
                it->inst.extra[0] = obj->emissive;
                it->inst.extra[1] = it->inst.extra[2] = it->inst.extra[3] = 0.0f;
                memcpy(it->inst.light_mask, light_mask, sizeof light_mask);
                r->order[n] = n;
                n++;
            }
        }
    }
    g_sort_items = r->items;
    qsort(r->order, n, sizeof *r->order, cmp_items);
    for (i = 0; i < n; i++) {
        dst[i] = r->items[r->order[i]].inst;
    }
    r->stats.draw_instances = n;
    r->stats.dropped += dropped;
    *item_count = n;
}

/* Push constants of a paint material (paint.frag, block Material). */
static void paint_push(SuRenderer *r, uint32_t mat_index, uint32_t forced, MatPush *p) {
    const MatRes *m = &r->res[mat_index].u.mat;
    uint32_t k;
    memset(p, 0, sizeof *p);
    memcpy(p->base_color, m->f.base_color, sizeof p->base_color);
    memcpy(p->emissive, m->f.emissive, 3u * sizeof(float));
    p->mr[0] = m->f.metallic;
    p->mr[1] = m->f.roughness;
    p->mr[2] = m->f.normal_scale;
    p->mr[3] = m->f.occlusion_strength;
    p->misc[0] = m->f.alpha_cutoff;
    p->misc[1] = forced != 0u ? 2.0f : (float)m->f.alpha_mode;
    p->misc[2] = m->f.emissive_strength;
    p->misc[3] = (m->bound_ready & (1u << SU_PAINT_SLOT_NORMAL)) != 0u ? 1.0f : 0.0f;
    for (k = 0; k < SU_RENDER_PAINT_SLOT_COUNT; k++) {
        if ((m->paint_tex_info[k] & 0xFFu) == 1u) {
            p->uvset[0] |= 1u << k;
        }
    }
}

static void mat_push(SuRenderer *r, uint32_t mat_index, uint32_t forced, MatPush *p) {
    memset(p, 0, sizeof *p);
    if (mat_index < r->res_cap && r->res[mat_index].type == RES_MATERIAL) {
        const MatRes *m = &r->res[mat_index].u.mat;
        uint32_t k;
        memcpy(p->base_color, m->f.base_color, sizeof p->base_color);
        p->emissive[0] = m->f.emissive[0] * m->f.emissive_strength;
        p->emissive[1] = m->f.emissive[1] * m->f.emissive_strength;
        p->emissive[2] = m->f.emissive[2] * m->f.emissive_strength;
        p->mr[0] = m->f.metallic;
        p->mr[1] = m->f.roughness;
        p->mr[2] = m->f.normal_scale;
        p->mr[3] = m->f.occlusion_strength;
        p->misc[0] = m->f.alpha_cutoff;
        p->misc[1] = (float)m->f.alpha_mode;
        p->misc[2] = (float)m->f.unlit;
        p->misc[3] = (m->bound_ready & (1u << SU_ASSET_SLOT_NORMAL)) != 0u ? 1.0f : 0.0f;
        for (k = 0; k < SU_ASSET_SLOT_COUNT; k++) {
            if ((m->f.tex_info[k] & 0xFFu) == 1u) {
                p->uvset[0] |= 1u << k;
            }
        }
    } else {
        /* the default material: light grey dielectric */
        p->base_color[0] = p->base_color[1] = p->base_color[2] = 0.7f;
        p->base_color[3] = 1.0f;
        p->mr[1] = 0.6f;
        p->mr[2] = 1.0f;
        p->mr[3] = 1.0f;
        p->misc[0] = 0.5f;
    }
    if (forced != 0u) {
        p->misc[1] = 2.0f; /* the shader returns base alpha (material alpha x texture alpha x instance tint) */
    }
}

void su_rvk_scene_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t item_count, int blend_pass,
                       const SuRenderFrameDesc *frame) {
    uint32_t i = 0;
    uint32_t bound_pipe = UINT32_MAX;
    uint32_t bound_kind = UINT32_MAX;
    uint32_t bound_mat = UINT32_MAX - 1u;
    uint32_t bound_forced = 0;
    uint32_t bound_mesh = UINT32_MAX;
    while (i < item_count) {
        const DrawItem *it = &r->items[r->order[i]];
        uint32_t run = 1;
        const SubGpu *sub;
        int pass = it->pipe >= PIPE_BLEND ? 2 : it->pipe >= PIPE_OVERLAY ? 1 : 0;
        if (pass != blend_pass) {
            i++;
            continue;
        }
        while (i + run < item_count) {
            const DrawItem *nx = &r->items[r->order[i + run]];
            if (nx->kind != it->kind || nx->pipe != it->pipe || nx->mat != it->mat || nx->forced != it->forced || nx->mesh != it->mesh || nx->sub != it->sub) {
                break;
            }
            run++;
        }
        if (it->kind == 2u) {
            fx_mesh_draw(r, fr, cmd, i, run, frame);
            bound_kind = bound_pipe = bound_mesh = UINT32_MAX;
            bound_mat = UINT32_MAX - 1u;
            i += run;
            continue;
        }
        if (it->pipe != bound_pipe || it->kind != bound_kind) {
            VkPipelineLayout layout = it->kind != 0u ? r->paint_layout : r->scene_layout;
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   it->kind != 0u ? r->paint_pipe[it->pipe] : r->pbr_pipe[it->pipe]);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &fr->frame_set, 0, NULL);
            if (it->kind != bound_kind) {
                bound_mat = UINT32_MAX - 1u; /* the layouts differ from set 1 on: bind the material again */
            }
            bound_pipe = it->pipe;
            bound_kind = it->kind;
        }
        if (it->mat != bound_mat || it->forced != bound_forced) {
            MatPush mp;
            VkDescriptorSet sets[2];
            VkPipelineLayout lay = it->kind != 0u ? r->paint_layout : r->scene_layout;
            sets[0] = su_rvk_material_set(r, it->mat);
            sets[1] = r->frame_env_set;
            if (it->kind != 0u) {
                paint_push(r, it->mat, it->forced, &mp);
            } else {
                mat_push(r, it->mat, it->forced, &mp);
            }
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 1, it->kind != 0u ? 2u : 1u, sets, 0, NULL);
            r->f.vkCmdPushConstants(cmd, lay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof mp, &mp);
            bound_mat = it->mat;
            bound_forced = it->forced;
        }
        if (it->mesh != bound_mesh) {
            const MeshRes *m = &r->res[it->mesh].u.mesh;
            VkBuffer vbs[2];
            VkDeviceSize offs[2] = {0, 0};
            vbs[0] = m->vb.buf;
            vbs[1] = fr->draw_inst.buf;
            r->f.vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            r->f.vkCmdBindIndexBuffer(cmd, m->ib.buf, 0, VK_INDEX_TYPE_UINT32);
            bound_mesh = it->mesh;
        }
        sub = &r->res[it->mesh].u.mesh.subs[it->sub];
        r->f.vkCmdDrawIndexed(cmd, sub->index_count, run, sub->first_index, 0, i);
        r->stats.draws++;
        i += run;
    }
}

/* ---------- sun shadows ---------- */

static void fx_mesh_draw(SuRenderer *renderer, Frame *fr, VkCommandBuffer cmd, uint32_t first, uint32_t run,
                         const SuRenderFrameDesc *frame) {
    const DrawItem *draw = &renderer->items[renderer->order[first]];
    const SuRenderFxMaterialDesc *material;
    const MeshRes *mesh;
    const SubGpu *sub;
    VkDescriptorSet sets[3];
    VkBuffer buffers[2];
    VkDeviceSize offsets[2] = {0, 0};
    FxPush push;
    material = &renderer->res[draw->mat].u.mat.fx;
    sets[0] = fr->frame_set;
    sets[1] = su_rvk_fx_set(renderer, fr, &material->layers, 0, material->wrap_u, material->wrap_v);
    sets[2] = renderer->depth_in_set;
    if (sets[1] == VK_NULL_HANDLE) {
        renderer->stats.fx_batches_skipped++;
        return;
    }
    su_rvk_fx_push(&push, &material->layers, frame->time_seconds, material->soft_distance, frame->camera.near_z,
                   material->wrap_u, material->wrap_v);
    mesh = &renderer->res[draw->mesh].u.mesh;
    sub = &mesh->subs[draw->sub];
    buffers[0] = mesh->vb.buf;
    buffers[1] = fr->draw_inst.buf;
    renderer->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->fxmesh_pipe[draw->pipe - PIPE_BLEND]);
    renderer->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->fxtex_layout, 0, 3, sets, 0, NULL);
    renderer->f.vkCmdPushConstants(cmd, renderer->fxtex_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    renderer->f.vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    renderer->f.vkCmdBindIndexBuffer(cmd, mesh->ib.buf, 0, VK_INDEX_TYPE_UINT32);
    renderer->f.vkCmdDrawIndexed(cmd, sub->index_count, run, sub->first_index, 0, first);
    renderer->stats.draws++;
}

/* The object's scale * rotation (column-major 3x3), its camera-relative position, and its bounding sphere. 0 when the
 * object is not drawn at all (unknown model, invisible, invalid scale3). */
static int object_frame(SuRenderer *r, const SuRenderFrameDesc *f, const SuRenderObject *obj, const ModelRes **model,
                        float sr[9], float rel[3], float c[3], float *radius, float *smax) {
    Res *ms = su_rvk_get(r, obj->model, RES_MODEL, NULL);
    float s = obj->scale > 0.0f ? obj->scale : 1.0f;
    float axis[3] = {1.0f, 1.0f, 1.0f};
    SuVec3f d;
    SuMat4f rot;
    int k;
    if (ms == NULL || !(obj->tint[3] > 0.0f)) {
        return 0;
    }
    if (obj->scale3[0] != 0.0f || obj->scale3[1] != 0.0f || obj->scale3[2] != 0.0f) {
        if (!(obj->scale3[0] > 0.0f && obj->scale3[1] > 0.0f && obj->scale3[2] > 0.0f) ||
            !(obj->scale3[0] < 1e30f && obj->scale3[1] < 1e30f && obj->scale3[2] < 1e30f)) {
            return 0;
        }
        axis[0] = obj->scale3[0];
        axis[1] = obj->scale3[1];
        axis[2] = obj->scale3[2];
    }
    *smax = s * (axis[0] > axis[1] ? (axis[0] > axis[2] ? axis[0] : axis[2]) : (axis[1] > axis[2] ? axis[1] : axis[2]));
    *model = &ms->u.model;
    d = su_wpos_delta(obj->pos, f->camera.pos);
    rel[0] = d.x;
    rel[1] = d.y;
    rel[2] = d.z;
    rot = su_mat4f_from_quat(obj->rot);
    for (k = 0; k < 3; k++) {
        sr[k] = rot.m[k] * s * axis[0];
        sr[3 + k] = rot.m[4 + k] * s * axis[1];
        sr[6 + k] = rot.m[8 + k] * s * axis[2];
    }
    for (k = 0; k < 3; k++) {
        c[k] = rel[k] + sr[k] * (*model)->center[0] + sr[3 + k] * (*model)->center[1] + sr[6 + k] * (*model)->center[2];
    }
    *radius = (*model)->radius * *smax;
    return 1;
}

static float dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static const ShadowItem *g_sort_shadow; /* qsort context (one renderer per thread) */

static int cmp_shadow(const void *a, const void *b) {
    const ShadowItem *x = &g_sort_shadow[*(const uint32_t *)a];
    const ShadowItem *y = &g_sort_shadow[*(const uint32_t *)b];
    if (x->key != y->key) {
        return x->key < y->key ? -1 : 1;
    }
    return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : *(const uint32_t *)a > *(const uint32_t *)b;
}

/* The sun's frame: lz along the light's travel, lx and ly across it. */
static void light_basis(const SuRenderFrameDesc *f, float lx[3], float ly[3], float lz[3]) {
    float h[3] = {0.0f, 1.0f, 0.0f};
    float len;
    lz[0] = f->light.sun_dir.x;
    lz[1] = f->light.sun_dir.y;
    lz[2] = f->light.sun_dir.z;
    len = sqrtf(dot3(lz, lz));
    if (!(len > 0.0f)) {
        lz[0] = 0.0f;
        lz[1] = -1.0f;
        lz[2] = 0.0f;
        len = 1.0f;
    }
    lz[0] /= len;
    lz[1] /= len;
    lz[2] /= len;
    if (fabsf(lz[1]) > 0.99f) {
        h[0] = 1.0f;
        h[1] = 0.0f;
    }
    lx[0] = h[1] * lz[2] - h[2] * lz[1];
    lx[1] = h[2] * lz[0] - h[0] * lz[2];
    lx[2] = h[0] * lz[1] - h[1] * lz[0];
    len = sqrtf(dot3(lx, lx));
    lx[0] /= len;
    lx[1] /= len;
    lx[2] /= len;
    ly[0] = lz[1] * lx[2] - lz[2] * lx[1];
    ly[1] = lz[2] * lx[0] - lz[0] * lx[2];
    ly[2] = lz[0] * lx[1] - lz[1] * lx[0];
}

/* Light-space bounds (x0 x1 y0 y1 z0 z1) of the view frustum slice between view depths t0 and t1. */
static void slice_bounds(SuRenderer *r, const SuRenderFrameDesc *f, const float lx[3], const float ly[3], const float lz[3],
                         float t0, float t1, float s[6]) {
    float ty = tanf(f->camera.fov_y * 0.5f);
    float tx = ty * (float)r->width / (float)(r->height > 0u ? r->height : 1u);
    int corner;
    int k;
    s[0] = s[2] = s[4] = 1e30f;
    s[1] = s[3] = s[5] = -1e30f;
    for (corner = 0; corner < 8; corner++) {
        float t = (corner & 4) != 0 ? t1 : t0;
        SuVec3f v;
        float p[3];
        v.x = ((corner & 1) != 0 ? tx : -tx) * t;
        v.y = ((corner & 2) != 0 ? ty : -ty) * t;
        v.z = -t;
        v = su_quatf_rotate(f->camera.rot, v);
        p[0] = v.x * lx[0] + v.y * lx[1] + v.z * lx[2];
        p[1] = v.x * ly[0] + v.y * ly[1] + v.z * ly[2];
        p[2] = v.x * lz[0] + v.y * lz[1] + v.z * lz[2];
        for (k = 0; k < 3; k++) {
            s[2 * k] = p[k] < s[2 * k] ? p[k] : s[2 * k];
            s[2 * k + 1] = p[k] > s[2 * k + 1] ? p[k] : s[2 * k + 1];
        }
    }
}

/* Snaps the square box origin (light space, camera-relative) to whole texels of a world-space grid. */
static void snap_origin(const SuRenderFrameDesc *f, const float lx[3], const float ly[3], float texel, float bx0, float by0,
                        float *x0, float *y0) {
    double cw[3];
    double cx;
    double cy;
    cw[0] = (double)f->camera.pos.x / 4294967296.0;
    cw[1] = (double)f->camera.pos.y / 4294967296.0;
    cw[2] = (double)f->camera.pos.z / 4294967296.0;
    cx = cw[0] * lx[0] + cw[1] * lx[1] + cw[2] * lx[2];
    cy = cw[0] * ly[0] + cw[1] * ly[1] + cw[2] * ly[2];
    *x0 = (float)(floor((cx + (double)bx0) / (double)texel - 2.0) * (double)texel - cx);
    *y0 = (float)(floor((cy + (double)by0) / (double)texel - 2.0) * (double)texel - cy);
}

/* Casters of one cascade box (square x0..x0+ext, y0..y0+ext, receivers up to zmax): appends their node x submesh
 * instances to r->sitems (tagged with the cascade) and returns the nearest caster depth (toward the sun). */
static float gather_casters(SuRenderer *r, const SuRenderFrameDesc *f, float proj_y, const float lx[3], const float ly[3],
                            const float lz[3], float x0, float y0, float ext, float zmin, float zmax, uint32_t cascade,
                            uint32_t *n) {
    uint32_t o;
    uint32_t i;
    for (o = 0; o < f->object_count; o++) {
        const SuRenderObject *obj = &f->objects[o];
        const ModelRes *model;
        float sr[9];
        float rel[3];
        float c[3];
        float rad;
        float smax;
        float p[3];
        if ((obj->flags & SU_RENDER_OBJECT_NO_SHADOW) != 0u || !(obj->tint[3] >= 1.0f) ||
            !object_frame(r, f, obj, &model, sr, rel, c, &rad, &smax)) {
            continue;
        }
        p[0] = dot3(c, lx);
        p[1] = dot3(c, ly);
        p[2] = dot3(c, lz);
        if (p[0] + rad < x0 || p[0] - rad > x0 + ext || p[1] + rad < y0 || p[1] - rad > y0 + ext || p[2] - rad > zmax) {
            continue;
        }
        zmin = p[2] - rad < zmin ? p[2] - rad : zmin;
        for (i = 0; i < model->count; i++) {
            const SuRenderModelNode *nd = &model->nodes[i];
            const float *nc = &model->node_center[i * 4u];
            const float *t = nd->transform;
            float ncw[3];
            float np[3];
            float nr = nc[3] * smax;
            uint32_t lod;
            uint32_t mesh_index = 0;
            Res *mesh = NULL;
            float m[12];
            uint32_t sub;
            int k;
            int a;
            int bb;
            for (k = 0; k < 3; k++) {
                ncw[k] = rel[k] + sr[k] * nc[0] + sr[3 + k] * nc[1] + sr[6 + k] * nc[2];
            }
            np[0] = dot3(ncw, lx);
            np[1] = dot3(ncw, ly);
            np[2] = dot3(ncw, lz);
            if (model->count > 1u && (np[0] + nr < x0 || np[0] - nr > x0 + ext || np[1] + nr < y0 || np[1] - nr > y0 + ext ||
                                      np[2] - nr > zmax)) {
                continue;
            }
            /* the LOD the camera selects, else the nearest READY one (finer first), as the main pass */
            lod = su_lod_select(nd->lod_screen, nd->lod_count, nr, sqrtf(dot3(ncw, ncw)), proj_y);
            for (k = 0; k < (int)nd->lod_count && mesh == NULL; k++) {
                int cand[2];
                int q;
                cand[0] = (int)lod - k;
                cand[1] = (int)lod + k;
                for (q = 0; q < (k == 0 ? 1 : 2) && mesh == NULL; q++) {
                    Res *x;
                    uint32_t xi;
                    if (cand[q] < 0 || cand[q] >= (int)nd->lod_count) {
                        continue;
                    }
                    x = su_rvk_get(r, nd->lod_mesh[cand[q]], RES_MESH, &xi);
                    if (x != NULL && x->state == SU_RENDER_STATE_READY) {
                        mesh = x;
                        mesh_index = xi;
                    }
                }
            }
            if (mesh == NULL) {
                continue;
            }
            for (a = 0; a < 3; a++) {
                for (bb = 0; bb < 3; bb++) {
                    m[a * 4 + bb] = sr[a] * t[bb * 4] + sr[3 + a] * t[bb * 4 + 1] + sr[6 + a] * t[bb * 4 + 2];
                }
                m[a * 4 + 3] = sr[a] * t[12] + sr[3 + a] * t[13] + sr[6 + a] * t[14] + rel[a];
            }
            for (sub = 0; sub < mesh->u.mesh.sub_count; sub++) {
                Res *mat = su_rvk_get(r, mesh->u.mesh.subs[sub].material, RES_MATERIAL, NULL);
                ShadowItem *it;
                if (mat != NULL && mat->u.mat.f.alpha_mode == SU_ASSET_ALPHA_BLEND) {
                    continue; /* blended surfaces do not cast */
                }
                if (*n == r->max_draw) {
                    r->stats.dropped++;
                    continue;
                }
                it = &r->sitems[*n];
                it->mesh = mesh_index;
                it->sub = sub;
                it->key = ((uint64_t)cascade << 56) | ((uint64_t)mesh_index << 16) | (uint64_t)(sub & 0xFFFFu);
                memcpy(it->inst.r0, &m[0], sizeof it->inst.r0);
                memcpy(it->inst.r1, &m[4], sizeof it->inst.r1);
                memcpy(it->inst.r2, &m[8], sizeof it->inst.r2);
                memset(it->inst.tint, 0, sizeof it->inst.tint);
                memset(it->inst.extra, 0, sizeof it->inst.extra);
                r->sorder[*n] = *n;
                (*n)++;
            }
        }
    }
    return zmin;
}

/* The orthographic projection of one cascade: x, y across (-1..1), depth 0 at zmin (toward the sun) .. 1 at zmax. */
static void cascade_matrix(const float lx[3], const float ly[3], const float lz[3], float x0, float y0, float ext, float zmin,
                           float zmax, float out[16]) {
    float zd = zmax - zmin > 1e-3f ? zmax - zmin : 1e-3f;
    uint32_t i;
    memset(out, 0, 16u * sizeof(float));
    for (i = 0; i < 3u; i++) {
        out[i * 4u + 0u] = 2.0f * lx[i] / ext;
        out[i * 4u + 1u] = 2.0f * ly[i] / ext;
        out[i * 4u + 2u] = lz[i] / zd;
    }
    out[12] = -2.0f * x0 / ext - 1.0f;
    out[13] = -2.0f * y0 / ext - 1.0f;
    out[14] = -zmin / zd;
    out[15] = 1.0f;
}

uint32_t su_rvk_shadow_build(SuRenderer *r, Frame *fr, const SuRenderFrameDesc *f, const SuMat4f *view_proj, float proj_y,
                             FrameUbo *ubo) {
    const SuRenderShadow *sh = &f->shadow;
    SuFrustum fru;
    float lz[3];
    float lx[3];
    float ly[3];
    float b[6] = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f}; /* receivers: x0 x1 y0 y1 z0 z1 in light space */
    float far_t = 0.0f;                                          /* farthest receiver along the view axis */
    uint32_t size = r->shadow_size;
    uint32_t nc = r->shadow_layers;
    uint32_t o;
    uint32_t n = 0;
    uint32_t i;
    uint32_t c;
    float fwd[3];
    float bias_texels = sh->bias > 0.0f ? sh->bias : 1.5f;
    float noff_texels = sh->normal_offset > 0.0f ? sh->normal_offset : 1.5f;
    DrawInstance *dst = fr->shadow_inst.map;
    su_frustum_from_matrix(&fru, *view_proj, 1);
    light_basis(f, lx, ly, lz);
    {
        SuVec3f v;
        v.x = 0.0f;
        v.y = 0.0f;
        v.z = -1.0f;
        v = su_quatf_rotate(f->camera.rot, v);
        fwd[0] = v.x;
        fwd[1] = v.y;
        fwd[2] = v.z;
    }
    memset(r->shadow_first, 0, sizeof r->shadow_first);
    memset(r->shadow_count, 0, sizeof r->shadow_count);
    /* receivers: visible objects (within distance of the camera when asked) */
    for (o = 0; o < f->object_count; o++) {
        const ModelRes *model;
        float sr[9];
        float rel[3];
        float cc[3];
        float rad;
        float smax;
        SuVec3f cv;
        float p[3];
        int k;
        if (!object_frame(r, f, &f->objects[o], &model, sr, rel, cc, &rad, &smax)) {
            continue;
        }
        cv.x = cc[0];
        cv.y = cc[1];
        cv.z = cc[2];
        if (!su_frustum_sphere(&fru, cv, rad) || (sh->distance > 0.0f && sqrtf(dot3(cc, cc)) - rad > sh->distance)) {
            continue;
        }
        p[0] = dot3(cc, lx);
        p[1] = dot3(cc, ly);
        p[2] = dot3(cc, lz);
        for (k = 0; k < 3; k++) {
            b[2 * k] = p[k] - rad < b[2 * k] ? p[k] - rad : b[2 * k];
            b[2 * k + 1] = p[k] + rad > b[2 * k + 1] ? p[k] + rad : b[2 * k + 1];
        }
        far_t = dot3(cc, fwd) + rad > far_t ? dot3(cc, fwd) + rad : far_t;
    }
    for (o = 0; o < f->haze_count; o++) {
        const SuHazeVolume *volume = &f->haze[o];
        SuVec3f offset;
        uint32_t corner;
        if (volume->density <= 0.0f || (volume->flags & SU_HAZE_SHADOW) == 0u) continue;
        offset = su_wpos_delta(volume->pos, f->camera.pos);
        for (corner = 0; corner < 8u; corner++) {
            SuVec3f point = {(corner & 1u) != 0u ? 1.0f : -1.0f, (corner & 2u) != 0u ? 1.0f : -1.0f,
                            (corner & 4u) != 0u ? 1.0f : -1.0f};
            float position[3];
            float projected[3];
            int axis;
            point = su_mat4f_transform_point(volume->transform, point, NULL);
            position[0] = point.x + offset.x;
            position[1] = point.y + offset.y;
            position[2] = point.z + offset.z;
            projected[0] = dot3(position, lx);
            projected[1] = dot3(position, ly);
            projected[2] = dot3(position, lz);
            for (axis = 0; axis < 3; axis++) {
                b[2 * axis] = fminf(b[2 * axis], projected[axis]);
                b[2 * axis + 1] = fmaxf(b[2 * axis + 1], projected[axis]);
            }
            far_t = fmaxf(far_t, dot3(position, fwd));
        }
    }
    if (b[0] > b[1]) {
        return 0; /* nothing visible receives a shadow */
    }
    memset(ubo->shadow_split, 0, sizeof ubo->shadow_split);
    if (nc <= 1u) {
        /* one map fitted to the receivers (and the slice up to distance) */
        float x0;
        float y0;
        float ext;
        float texel;
        float zmin;
        float zd;
        if (sh->distance > 0.0f) {
            float s[6];
            int k;
            slice_bounds(r, f, lx, ly, lz, f->camera.near_z, sh->distance, s);
            for (k = 0; k < 3; k++) {
                b[2 * k] = s[2 * k] > b[2 * k] ? s[2 * k] : b[2 * k];
                b[2 * k + 1] = s[2 * k + 1] < b[2 * k + 1] ? s[2 * k + 1] : b[2 * k + 1];
            }
            if (b[0] >= b[1] || b[2] >= b[3] || b[4] >= b[5]) {
                return 0;
            }
        }
        /* a square map, texels of a quantized size (steps of 2^(1/8)) snapped to a world-space grid: no shimmer */
        ext = b[1] - b[0] > b[3] - b[2] ? b[1] - b[0] : b[3] - b[2];
        texel = ext / (float)(size - 4u);
        texel = exp2f(ceilf(log2f(texel > 1e-6f ? texel : 1e-6f) * 8.0f) / 8.0f);
        snap_origin(f, lx, ly, texel, b[0], b[2], &x0, &y0);
        ext = texel * (float)size;
        zmin = gather_casters(r, f, proj_y, lx, ly, lz, x0, y0, ext, b[4], b[5], 0u, &n);
        zd = b[5] - zmin > 1e-3f ? b[5] - zmin : 1e-3f;
        cascade_matrix(lx, ly, lz, x0, y0, ext, zmin, b[5], ubo->shadow_vp);
        ubo->shadow_bias[0] = bias_texels * texel / zd;
        ubo->shadow_noff[0] = noff_texels * texel;
        ubo->shadow_split[0] = 1e30f;
        r->stats.shadow_texel = texel;
    } else {
        /* cascades: slices of the view frustum split between the uniform and the logarithmic scheme by lambda, each
         * fitted by the bounding sphere of its slice (a size that does not change as the camera turns) */
        float lambda = sh->split_lambda > 0.0f ? (sh->split_lambda < 1.0f ? sh->split_lambda : 1.0f) : 0.75f;
        float tfar = sh->distance > 0.0f ? sh->distance : far_t;
        float tnear = f->camera.near_z;
        float t0;
        tnear = tnear > tfar * 1e-4f ? tnear : tfar * 1e-4f;
        if (!(tfar > tnear)) {
            return 0;
        }
        t0 = f->camera.near_z;
        for (c = 0; c < nc; c++) {
            float q = (float)(c + 1u) / (float)nc;
            float t1 = lambda * tnear * powf(tfar / tnear, q) + (1.0f - lambda) * (tnear + (tfar - tnear) * q);
            float s[6];
            float ctr[3];
            float rad = 0.0f;
            float x0;
            float y0;
            float ext;
            float texel;
            float zmin;
            float zmax;
            float zd;
            float ty = tanf(f->camera.fov_y * 0.5f);
            float tx = ty * (float)r->width / (float)(r->height > 0u ? r->height : 1u);
            if (c + 1u == nc) {
                t1 = tfar;
            }
            /* the slice's bounding sphere (centre on the view axis), in light space */
            {
                float tm = 0.5f * (t0 + t1);
                float d2n = (t0 - tm) * (t0 - tm) + (tx * t0) * (tx * t0) + (ty * t0) * (ty * t0);
                float d2f = (t1 - tm) * (t1 - tm) + (tx * t1) * (tx * t1) + (ty * t1) * (ty * t1);
                rad = sqrtf(d2n > d2f ? d2n : d2f);
                ctr[0] = dot3(fwd, lx) * tm;
                ctr[1] = dot3(fwd, ly) * tm;
                ctr[2] = dot3(fwd, lz) * tm;
            }
            slice_bounds(r, f, lx, ly, lz, t0, t1, s);
            texel = 2.0f * rad / (float)(size - 4u);
            snap_origin(f, lx, ly, texel, ctr[0] - rad, ctr[1] - rad, &x0, &y0);
            ext = texel * (float)size;
            zmax = s[5] < b[5] ? s[5] : b[5]; /* nothing beyond the farthest receiver needs the map */
            zmax = zmax > s[4] ? zmax : s[5];
            zmin = gather_casters(r, f, proj_y, lx, ly, lz, x0, y0, ext, s[4], zmax, c, &n);
            zd = zmax - zmin > 1e-3f ? zmax - zmin : 1e-3f;
            cascade_matrix(lx, ly, lz, x0, y0, ext, zmin, zmax, ubo->shadow_vp + 16u * c);
            ubo->shadow_bias[c] = bias_texels * texel / zd;
            ubo->shadow_noff[c] = noff_texels * texel;
            ubo->shadow_split[c] = t1;
            r->stats.shadow_cascade_texel[c] = texel;
            if (c == 0u) {
                r->stats.shadow_texel = texel;
            }
            t0 = t1;
        }
    }
    ubo->shadow_params[0] = sh->strength > 0.0f ? (sh->strength < 1.0f ? sh->strength : 1.0f) : 1.0f;
    ubo->shadow_params[1] = 0.0f;
    ubo->shadow_params[2] = 1.0f / (float)size;
    ubo->shadow_params[3] = (float)(nc > 1u ? nc : 1u);
    ubo->shadow_fwd[0] = fwd[0];
    ubo->shadow_fwd[1] = fwd[1];
    ubo->shadow_fwd[2] = fwd[2];
    ubo->shadow_fwd[3] = 0.0f;
    g_sort_shadow = r->sitems;
    qsort(r->sorder, n, sizeof *r->sorder, cmp_shadow);
    for (i = 0; i < n; i++) {
        const ShadowItem *it = &r->sitems[r->sorder[i]];
        uint32_t cs = (uint32_t)(it->key >> 56);
        dst[i] = it->inst;
        if (r->shadow_count[cs] == 0u) {
            r->shadow_first[cs] = i;
        }
        r->shadow_count[cs]++;
    }
    r->stats.shadow_casters = n;
    return n;
}

void su_rvk_shadow_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t first, uint32_t count) {
    uint32_t i = first;
    uint32_t end = first + count;
    uint32_t bound_mesh = UINT32_MAX;
    while (i < end) {
        const ShadowItem *it = &r->sitems[r->sorder[i]];
        const SubGpu *sub;
        uint32_t run = 1;
        while (i + run < end && r->sitems[r->sorder[i + run]].key == it->key) {
            run++;
        }
        if (it->mesh != bound_mesh) {
            const MeshRes *m = &r->res[it->mesh].u.mesh;
            VkBuffer vbs[2];
            VkDeviceSize offs[2] = {0, 0};
            vbs[0] = m->vb.buf;
            vbs[1] = fr->shadow_inst.buf;
            r->f.vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            r->f.vkCmdBindIndexBuffer(cmd, m->ib.buf, 0, VK_INDEX_TYPE_UINT32);
            bound_mesh = it->mesh;
        }
        sub = &r->res[it->mesh].u.mesh.subs[it->sub];
        r->f.vkCmdDrawIndexed(cmd, sub->index_count, run, sub->first_index, 0, i);
        r->stats.shadow_draws++;
        r->stats.draws++;
        i += run;
    }
}
