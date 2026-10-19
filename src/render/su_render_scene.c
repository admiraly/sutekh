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

static int cmp_items(const void *a, const void *b) {
    const DrawItem *x = &g_sort_items[*(const uint32_t *)a];
    const DrawItem *y = &g_sort_items[*(const uint32_t *)b];
    int xb = x->pipe >= PIPE_BLEND;
    int yb = y->pipe >= PIPE_BLEND;
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
                if (obj->tint[3] < 1.0f && it->pipe < PIPE_BLEND) {
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
    for (k = 0; k < SU_PAINT_SLOT_COUNT; k++) {
        if ((m->paint.tex_info[k] & 0xFFu) == 1u) {
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

void su_rvk_scene_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t item_count, int blend_pass) {
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
        if ((it->pipe >= PIPE_BLEND) != (blend_pass != 0)) {
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
        if (it->pipe != bound_pipe || it->kind != bound_kind) {
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   it->kind != 0u ? r->paint_pipe[it->pipe] : r->pbr_pipe[it->pipe]);
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

uint32_t su_rvk_shadow_build(SuRenderer *r, Frame *fr, const SuRenderFrameDesc *f, const SuMat4f *view_proj, float proj_y,
                             FrameUbo *ubo) {
    const SuRenderShadow *sh = &f->shadow;
    SuFrustum fru;
    float lz[3];
    float lx[3];
    float ly[3];
    float b[6] = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f}; /* receivers: x0 x1 y0 y1 z0 z1 in light space */
    float zmin;
    float x0;
    float y0;
    float ext;
    float texel;
    float zd;
    double cw[3];
    uint32_t size = r->shadow_size;
    uint32_t o;
    uint32_t n = 0;
    uint32_t i;
    float len;
    DrawInstance *dst = fr->shadow_inst.map;
    su_frustum_from_matrix(&fru, *view_proj, 1);
    /* the sun's frame: z along the light's travel, x and y across it */
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
    {
        float h[3] = {0.0f, 1.0f, 0.0f};
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
    /* receivers: visible objects (within distance of the camera when asked) */
    for (o = 0; o < f->object_count; o++) {
        const ModelRes *model;
        float sr[9];
        float rel[3];
        float c[3];
        float rad;
        float smax;
        SuVec3f cv;
        float p[3];
        int k;
        if (!object_frame(r, f, &f->objects[o], &model, sr, rel, c, &rad, &smax)) {
            continue;
        }
        cv.x = c[0];
        cv.y = c[1];
        cv.z = c[2];
        if (!su_frustum_sphere(&fru, cv, rad) || (sh->distance > 0.0f && sqrtf(dot3(c, c)) - rad > sh->distance)) {
            continue;
        }
        p[0] = dot3(c, lx);
        p[1] = dot3(c, ly);
        p[2] = dot3(c, lz);
        for (k = 0; k < 3; k++) {
            b[2 * k] = p[k] - rad < b[2 * k] ? p[k] - rad : b[2 * k];
            b[2 * k + 1] = p[k] + rad > b[2 * k + 1] ? p[k] + rad : b[2 * k + 1];
        }
    }
    if (b[0] > b[1]) {
        return 0; /* nothing visible receives a shadow */
    }
    if (sh->distance > 0.0f) {
        /* intersect with the view frustum slice near .. distance */
        float s[6] = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f};
        float ty = tanf(f->camera.fov_y * 0.5f);
        float tx = ty * (float)r->width / (float)(r->height > 0u ? r->height : 1u);
        int corner;
        int k;
        for (corner = 0; corner < 8; corner++) {
            float t = (corner & 4) != 0 ? sh->distance : f->camera.near_z;
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
    cw[0] = (double)f->camera.pos.x / 4294967296.0;
    cw[1] = (double)f->camera.pos.y / 4294967296.0;
    cw[2] = (double)f->camera.pos.z / 4294967296.0;
    {
        double cx = cw[0] * lx[0] + cw[1] * lx[1] + cw[2] * lx[2];
        double cy = cw[0] * ly[0] + cw[1] * ly[1] + cw[2] * ly[2];
        x0 = (float)(floor((cx + (double)b[0]) / (double)texel - 2.0) * (double)texel - cx);
        y0 = (float)(floor((cy + (double)b[2]) / (double)texel - 2.0) * (double)texel - cy);
    }
    ext = texel * (float)size;
    /* casters: everything that may throw a shadow into the box from the sun's side */
    zmin = b[4];
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
        if (p[0] + rad < x0 || p[0] - rad > x0 + ext || p[1] + rad < y0 || p[1] - rad > y0 + ext || p[2] - rad > b[5]) {
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
                                      np[2] - nr > b[5])) {
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
                if (n == r->max_draw) {
                    r->stats.dropped++;
                    continue;
                }
                it = &r->sitems[n];
                it->mesh = mesh_index;
                it->sub = sub;
                it->key = ((uint64_t)mesh_index << 16) | (uint64_t)(sub & 0xFFFFu);
                memcpy(it->inst.r0, &m[0], sizeof it->inst.r0);
                memcpy(it->inst.r1, &m[4], sizeof it->inst.r1);
                memcpy(it->inst.r2, &m[8], sizeof it->inst.r2);
                memset(it->inst.tint, 0, sizeof it->inst.tint);
                memset(it->inst.extra, 0, sizeof it->inst.extra);
                r->sorder[n] = n;
                n++;
            }
        }
    }
    /* the sun's orthographic projection: x, y across (-1..1), depth 0 at zmin (toward the sun) .. 1 at the far receiver */
    zd = b[5] - zmin;
    zd = zd > 1e-3f ? zd : 1e-3f;
    memset(ubo->shadow_vp, 0, sizeof ubo->shadow_vp);
    for (i = 0; i < 3u; i++) {
        ubo->shadow_vp[i * 4u + 0u] = 2.0f * lx[i] / ext;
        ubo->shadow_vp[i * 4u + 1u] = 2.0f * ly[i] / ext;
        ubo->shadow_vp[i * 4u + 2u] = lz[i] / zd;
    }
    ubo->shadow_vp[12] = -2.0f * x0 / ext - 1.0f;
    ubo->shadow_vp[13] = -2.0f * y0 / ext - 1.0f;
    ubo->shadow_vp[14] = -zmin / zd;
    ubo->shadow_vp[15] = 1.0f;
    ubo->shadow_params[0] = sh->strength > 0.0f ? (sh->strength < 1.0f ? sh->strength : 1.0f) : 1.0f;
    ubo->shadow_params[1] = (sh->bias > 0.0f ? sh->bias : 1.5f) * texel / zd;
    ubo->shadow_params[2] = 1.0f / (float)size;
    ubo->shadow_params[3] = (sh->normal_offset > 0.0f ? sh->normal_offset : 1.5f) * texel;
    g_sort_shadow = r->sitems;
    qsort(r->sorder, n, sizeof *r->sorder, cmp_shadow);
    for (i = 0; i < n; i++) {
        dst[i] = r->sitems[r->sorder[i]].inst;
    }
    r->stats.shadow_casters = n;
    r->stats.shadow_texel = texel;
    return n;
}

void su_rvk_shadow_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t count) {
    uint32_t i = 0;
    uint32_t bound_mesh = UINT32_MAX;
    while (i < count) {
        const ShadowItem *it = &r->sitems[r->sorder[i]];
        const SubGpu *sub;
        uint32_t run = 1;
        while (i + run < count && r->sitems[r->sorder[i + run]].key == it->key) {
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
