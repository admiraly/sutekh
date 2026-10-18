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
        return x->dist2 > y->dist2 ? -1 : x->dist2 < y->dist2; /* far first */
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
        SuVec3f c;
        int k;
        if (ms == NULL) {
            r->stats.objects_culled++;
            continue;
        }
        model = &ms->u.model;
        rel = su_wpos_delta(obj->pos, f->camera.pos);
        rot = su_mat4f_from_quat(obj->rot);
        for (k = 0; k < 3; k++) {
            sr[k] = rot.m[k] * s;
            sr[3 + k] = rot.m[4 + k] * s;
            sr[6 + k] = rot.m[8 + k] * s;
        }
        c.x = rel.x + sr[0] * model->center[0] + sr[3] * model->center[1] + sr[6] * model->center[2];
        c.y = rel.y + sr[1] * model->center[0] + sr[4] * model->center[1] + sr[7] * model->center[2];
        c.z = rel.z + sr[2] * model->center[0] + sr[5] * model->center[1] + sr[8] * model->center[2];
        if (!su_frustum_sphere(&fru, c, model->radius * s)) {
            r->stats.objects_culled++;
            continue;
        }
        for (i = 0; i < model->count; i++) {
            const SuRenderModelNode *nd = &model->nodes[i];
            const float *nc = &model->node_center[i * 4u];
            const float *t = nd->transform;
            float ncw[3];
            float nr = nc[3] * s;
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
                it->mat = mat_index;
                it->mesh = mesh_index;
                it->sub = sub;
                it->key = ((uint64_t)it->pipe << 60) | ((uint64_t)(mat_index & 0xFFFFFu) << 40) |
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

static void mat_push(SuRenderer *r, uint32_t mat_index, MatPush *p) {
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
}

void su_rvk_scene_draw(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, uint32_t item_count, int blend_pass) {
    uint32_t i = 0;
    uint32_t bound_pipe = UINT32_MAX;
    uint32_t bound_mat = UINT32_MAX - 1u;
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
            if (nx->pipe != it->pipe || nx->mat != it->mat || nx->mesh != it->mesh || nx->sub != it->sub) {
                break;
            }
            run++;
        }
        if (it->pipe != bound_pipe) {
            r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pbr_pipe[it->pipe]);
            bound_pipe = it->pipe;
        }
        if (it->mat != bound_mat) {
            VkDescriptorSet set = su_rvk_material_set(r, it->mat);
            MatPush mp;
            mat_push(r, it->mat, &mp);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->scene_layout, 1, 1, &set, 0, NULL);
            r->f.vkCmdPushConstants(cmd, r->scene_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof mp, &mp);
            bound_mat = it->mat;
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
