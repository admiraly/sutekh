/* Portable part of sutekh/render.h: math, cameras, procedural meshes, debug lines and the PPM writer. No GPU. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- vectors ---------- */

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}
static SuVec3f add(SuVec3f a, SuVec3f b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static SuVec3f sub(SuVec3f a, SuVec3f b) {
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static SuVec3f scale(SuVec3f a, float s) {
    return v3(a.x * s, a.y * s, a.z * s);
}
static float dot(SuVec3f a, SuVec3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static SuVec3f cross(SuVec3f a, SuVec3f b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static SuVec3f norm(SuVec3f a) {
    float l = sqrtf(dot(a, a));
    return l > 0.0f ? scale(a, 1.0f / l) : a;
}

SuVec3f su_vec3f_lerp(SuVec3f a, SuVec3f b, float t) {
    return add(a, scale(sub(b, a), t));
}

/* ---------- matrices ---------- */

SuMat4f su_mat4f_identity(void) {
    SuMat4f r;
    memset(&r, 0, sizeof r);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

SuMat4f su_mat4f_mul(SuMat4f a, SuMat4f b) {
    SuMat4f r;
    int c;
    int row;
    for (c = 0; c < 4; c++) {
        for (row = 0; row < 4; row++) {
            r.m[c * 4 + row] = a.m[0 * 4 + row] * b.m[c * 4 + 0] + a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                               a.m[2 * 4 + row] * b.m[c * 4 + 2] + a.m[3 * 4 + row] * b.m[c * 4 + 3];
        }
    }
    return r;
}

SuMat4f su_mat4f_look_at(SuVec3f eye, SuVec3f target, SuVec3f up) {
    SuVec3f f = norm(sub(target, eye));
    SuVec3f s = norm(cross(f, up));
    SuVec3f u = cross(s, f);
    SuMat4f r = su_mat4f_identity();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

SuMat4f su_mat4f_perspective(float fov_y, float aspect, float near_z, float far_z) {
    SuMat4f r;
    float f = 1.0f / tanf(fov_y * 0.5f);
    memset(&r, 0, sizeof r);
    r.m[0] = f / aspect;
    r.m[5] = -f; /* Vulkan clip space: y down */
    r.m[10] = far_z / (near_z - far_z);
    r.m[11] = -1.0f;
    r.m[14] = near_z * far_z / (near_z - far_z);
    return r;
}

SuVec3f su_mat4f_transform_point(SuMat4f m, SuVec3f p, float *w_out) {
    float x = m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12];
    float y = m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13];
    float z = m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14];
    float w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
    if (w_out != NULL) {
        *w_out = w;
    }
    return w != 0.0f ? v3(x / w, y / w, z / w) : v3(x, y, z);
}

SuVec3f su_quatf_rotate(SuQuatf q, SuVec3f v) {
    SuVec3f u = v3(q.x, q.y, q.z);
    SuVec3f t = scale(cross(u, v), 2.0f);
    return add(add(v, scale(t, q.w)), cross(u, t));
}

SuQuatf su_quatf_nlerp(SuQuatf a, SuQuatf b, float t) {
    SuQuatf r;
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    float s = d < 0.0f ? -1.0f : 1.0f;
    float l;
    r.x = a.x + (s * b.x - a.x) * t;
    r.y = a.y + (s * b.y - a.y) * t;
    r.z = a.z + (s * b.z - a.z) * t;
    r.w = a.w + (s * b.w - a.w) * t;
    l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (l > 0.0f) {
        r.x /= l;
        r.y /= l;
        r.z /= l;
        r.w /= l;
    }
    return r;
}

/* ---------- cameras ---------- */

SuMat4f su_chase_camera_update(SuChaseCamera *c, SuVec3f target_pos, SuQuatf target_rot, float dt_s) {
    SuVec3f eye = add(target_pos, su_quatf_rotate(target_rot, c->offset));
    SuVec3f at = add(target_pos, su_quatf_rotate(target_rot, c->look_ahead));
    SuVec3f up = su_quatf_rotate(target_rot, v3(0.0f, 1.0f, 0.0f));
    if (!c->initialized) {
        c->eye = eye;
        c->at = at;
        c->initialized = 1;
    } else {
        float k = 1.0f - expf(-c->stiffness * (dt_s > 0.0f ? dt_s : 0.0f));
        c->eye = su_vec3f_lerp(c->eye, eye, k);
        c->at = su_vec3f_lerp(c->at, at, k);
    }
    return su_mat4f_look_at(c->eye, c->at, up);
}

SuMat4f su_free_camera_update(SuFreeCamera *c, SuVec3f move, float turn_yaw, float turn_pitch) {
    SuVec3f fwd;
    SuVec3f right;
    SuVec3f up;
    c->yaw += turn_yaw;
    c->pitch += turn_pitch;
    if (c->pitch > 1.5f) {
        c->pitch = 1.5f;
    }
    if (c->pitch < -1.5f) {
        c->pitch = -1.5f;
    }
    fwd = v3(-sinf(c->yaw) * cosf(c->pitch), sinf(c->pitch), -cosf(c->yaw) * cosf(c->pitch));
    right = v3(cosf(c->yaw), 0.0f, -sinf(c->yaw));
    up = cross(right, fwd);
    c->pos = add(c->pos, add(add(scale(right, move.x), scale(up, move.y)), scale(fwd, move.z)));
    return su_mat4f_look_at(c->pos, add(c->pos, fwd), v3(0.0f, 1.0f, 0.0f));
}

/* ---------- meshes ---------- */

typedef struct MeshOut {
    SuMeshVertex *v;
    uint32_t vcap;
    uint32_t vn;
    uint16_t *i;
    uint32_t icap;
    uint32_t in;
} MeshOut;

static void put_vertex(MeshOut *o, SuVec3f p, SuVec3f n) {
    if (o->v != NULL && o->vn < o->vcap) {
        o->v[o->vn].pos[0] = p.x;
        o->v[o->vn].pos[1] = p.y;
        o->v[o->vn].pos[2] = p.z;
        o->v[o->vn].normal[0] = n.x;
        o->v[o->vn].normal[1] = n.y;
        o->v[o->vn].normal[2] = n.z;
    }
    o->vn++;
}

static void put_index(MeshOut *o, uint32_t idx) {
    if (o->i != NULL && o->in < o->icap) {
        o->i[o->in] = (uint16_t)idx;
    }
    o->in++;
}

static void build_box(MeshOut *o) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int f;
    for (f = 0; f < 6; f++) {
        SuVec3f nn = v3(n[f][0], n[f][1], n[f][2]);
        /* Two in-plane axes with s x t = n, so (0,0) (1,0) (1,1) (0,1) runs counter-clockwise seen from outside. */
        SuVec3f s = fabsf(nn.y) > 0.5f ? v3(1, 0, 0) : v3(0, 1, 0);
        SuVec3f t = cross(nn, s);
        uint32_t base = o->vn;
        SuVec3f c = scale(nn, 0.5f);
        s = scale(s, 0.5f);
        t = scale(t, 0.5f);
        put_vertex(o, sub(sub(c, s), t), nn);
        put_vertex(o, sub(add(c, s), t), nn);
        put_vertex(o, add(add(c, s), t), nn);
        put_vertex(o, add(sub(c, s), t), nn);
        put_index(o, base);
        put_index(o, base + 1u);
        put_index(o, base + 2u);
        put_index(o, base);
        put_index(o, base + 2u);
        put_index(o, base + 3u);
    }
}

static void build_sphere(MeshOut *o) {
    const uint32_t seg = 24u;
    const uint32_t rings = 16u;
    const float pi = 3.14159265358979f;
    uint32_t r;
    uint32_t s;
    for (r = 0; r <= rings; r++) {
        float phi = pi * (float)r / (float)rings; /* 0 at +y */
        for (s = 0; s <= seg; s++) {
            float th = 2.0f * pi * (float)s / (float)seg;
            SuVec3f n = v3(sinf(phi) * cosf(th), cosf(phi), -sinf(phi) * sinf(th));
            put_vertex(o, scale(n, 0.5f), n);
        }
    }
    for (r = 0; r < rings; r++) {
        for (s = 0; s < seg; s++) {
            uint32_t a = r * (seg + 1u) + s;
            uint32_t b = a + seg + 1u;
            /* (b - a) runs toward -y at the equator and (a + 1 - a) toward -z at th = 0: their cross product is +x,
             * the outward normal there, so the triangles are counter-clockwise seen from outside. */
            put_index(o, a);
            put_index(o, b);
            put_index(o, a + 1u);
            put_index(o, a + 1u);
            put_index(o, b);
            put_index(o, b + 1u);
        }
    }
}

static void build_ship(MeshOut *o) {
    static const float p[6][3] = {
        {0.0f, 0.0f, -0.6f},  /* nose */
        {-0.45f, 0.0f, 0.4f}, /* left wing */
        {0.45f, 0.0f, 0.4f},  /* right wing */
        {0.0f, 0.18f, 0.3f},  /* top */
        {0.0f, -0.1f, 0.3f},  /* bottom */
        {0.0f, 0.0f, 0.0f},   /* unused */
    };
    static const int tri[6][3] = {{0, 1, 3}, {0, 3, 2}, {0, 4, 1}, {0, 2, 4}, {1, 2, 3}, {1, 4, 2}};
    SuVec3f center = v3(0.0f, 0.016f, 0.16f); /* mean of the five hull points */
    int t;
    for (t = 0; t < 6; t++) {
        SuVec3f a = v3(p[tri[t][0]][0], p[tri[t][0]][1], p[tri[t][0]][2]);
        SuVec3f b = v3(p[tri[t][1]][0], p[tri[t][1]][1], p[tri[t][1]][2]);
        SuVec3f c = v3(p[tri[t][2]][0], p[tri[t][2]][1], p[tri[t][2]][2]);
        SuVec3f n = norm(cross(sub(b, a), sub(c, a)));
        uint32_t base = o->vn;
        SuVec3f mid = scale(add(add(a, b), c), 1.0f / 3.0f);
        if (dot(n, sub(mid, center)) < 0.0f) {
            SuVec3f tmp = b;
            b = c;
            c = tmp;
            n = scale(n, -1.0f);
        }
        put_vertex(o, a, n);
        put_vertex(o, b, n);
        put_vertex(o, c, n);
        put_index(o, base);
        put_index(o, base + 1u);
        put_index(o, base + 2u);
    }
}

uint32_t su_mesh_build(uint32_t mesh, SuMeshVertex *vertices, uint32_t vertex_cap, uint32_t *vertex_count,
                       uint16_t *indices, uint32_t index_cap, uint32_t *index_count) {
    MeshOut o;
    if (vertex_count == NULL || index_count == NULL || mesh >= SU_MESH_COUNT) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&o, 0, sizeof o);
    o.v = vertices;
    o.vcap = vertex_cap;
    o.i = indices;
    o.icap = index_cap;
    switch (mesh) {
    case SU_MESH_BOX:
        build_box(&o);
        break;
    case SU_MESH_SPHERE:
        build_sphere(&o);
        break;
    default:
        build_ship(&o);
        break;
    }
    *vertex_count = o.vn;
    *index_count = o.in;
    if ((vertices != NULL && vertex_cap < o.vn) || (indices != NULL && index_cap < o.in)) {
        return SU_E_INVALID_ARGUMENT;
    }
    return SU_OK;
}

/* ---------- debug lines ---------- */

void su_debug_line(SuDebugLines *d, SuVec3f a, SuVec3f b, uint32_t rgba) {
    if (d == NULL) {
        return;
    }
    if (d->v == NULL || d->count + 2u > d->cap) {
        d->dropped++;
        return;
    }
    d->v[d->count].pos[0] = a.x;
    d->v[d->count].pos[1] = a.y;
    d->v[d->count].pos[2] = a.z;
    d->v[d->count].rgba = rgba;
    d->v[d->count + 1u].pos[0] = b.x;
    d->v[d->count + 1u].pos[1] = b.y;
    d->v[d->count + 1u].pos[2] = b.z;
    d->v[d->count + 1u].rgba = rgba;
    d->count += 2u;
}

void su_debug_vector(SuDebugLines *d, SuVec3f o, SuVec3f vec, uint32_t rgba) {
    SuVec3f tip = add(o, vec);
    float len = sqrtf(dot(vec, vec));
    SuVec3f dir;
    SuVec3f side;
    SuVec3f up;
    float h;
    su_debug_line(d, o, tip, rgba);
    if (len <= 0.0f) {
        return;
    }
    dir = scale(vec, 1.0f / len);
    side = fabsf(dir.y) < 0.9f ? norm(cross(dir, v3(0, 1, 0))) : norm(cross(dir, v3(1, 0, 0)));
    up = cross(side, dir);
    h = len * 0.15f;
    su_debug_line(d, tip, add(sub(tip, scale(dir, h)), scale(side, h * 0.5f)), rgba);
    su_debug_line(d, tip, sub(sub(tip, scale(dir, h)), scale(side, h * 0.5f)), rgba);
    su_debug_line(d, tip, add(sub(tip, scale(dir, h)), scale(up, h * 0.5f)), rgba);
    su_debug_line(d, tip, sub(sub(tip, scale(dir, h)), scale(up, h * 0.5f)), rgba);
}

void su_debug_ellipse(SuDebugLines *d, SuVec3f c, SuVec3f u, SuVec3f v, uint32_t segments, uint32_t rgba) {
    uint32_t i;
    const float two_pi = 6.28318530717959f;
    if (segments < 3u) {
        segments = 3u;
    }
    for (i = 0; i < segments; i++) {
        float t0 = two_pi * (float)i / (float)segments;
        float t1 = two_pi * (float)(i + 1u) / (float)segments;
        SuVec3f a = add(c, add(scale(u, cosf(t0)), scale(v, sinf(t0))));
        SuVec3f b = add(c, add(scale(u, cosf(t1)), scale(v, sinf(t1))));
        su_debug_line(d, a, b, rgba);
    }
}

void su_debug_plane(SuDebugLines *d, SuVec3f c, SuVec3f u, SuVec3f v, uint32_t cells, uint32_t rgba) {
    uint32_t i;
    SuVec3f n;
    float lu;
    float lv;
    if (cells == 0u) {
        cells = 1u;
    }
    for (i = 0; i <= cells; i++) {
        float t = -1.0f + 2.0f * (float)i / (float)cells;
        su_debug_line(d, add(c, add(scale(u, t), scale(v, -1.0f))), add(c, add(scale(u, t), v)), rgba);
        su_debug_line(d, add(c, add(scale(u, -1.0f), scale(v, t))), add(c, add(u, scale(v, t))), rgba);
    }
    lu = sqrtf(dot(u, u));
    lv = sqrtf(dot(v, v));
    n = scale(norm(cross(u, v)), 0.5f * (lu < lv ? lu : lv));
    su_debug_vector(d, c, n, rgba);
}

void su_debug_box(SuDebugLines *d, SuVec3f lo, SuVec3f hi, uint32_t rgba) {
    int i;
    for (i = 0; i < 4; i++) {
        float x0 = (i & 1) ? hi.x : lo.x;
        float y0 = (i & 2) ? hi.y : lo.y;
        su_debug_line(d, v3(x0, y0, lo.z), v3(x0, y0, hi.z), rgba);
        su_debug_line(d, v3(lo.x, (i & 1) ? hi.y : lo.y, (i & 2) ? hi.z : lo.z),
                      v3(hi.x, (i & 1) ? hi.y : lo.y, (i & 2) ? hi.z : lo.z), rgba);
        su_debug_line(d, v3((i & 1) ? hi.x : lo.x, lo.y, (i & 2) ? hi.z : lo.z),
                      v3((i & 1) ? hi.x : lo.x, hi.y, (i & 2) ? hi.z : lo.z), rgba);
    }
}

void su_debug_axes(SuDebugLines *d, SuVec3f o, SuQuatf rot, float length) {
    su_debug_line(d, o, add(o, su_quatf_rotate(rot, v3(length, 0, 0))), 0xFF3030FFu);
    su_debug_line(d, o, add(o, su_quatf_rotate(rot, v3(0, length, 0))), 0xFF30FF30u);
    su_debug_line(d, o, add(o, su_quatf_rotate(rot, v3(0, 0, length))), 0xFFFF3030u);
}

/* ---------- PPM ---------- */

uint32_t su_render_write_ppm(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height) {
    FILE *f;
    uint32_t i;
    if (path == NULL || rgba == NULL || width == 0u || height == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return SU_E_IO;
    }
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height);
    for (i = 0; i < width * height; i++) {
        if (fwrite(rgba + 4u * (size_t)i, 1, 3, f) != 3u) {
            fclose(f);
            return SU_E_IO;
        }
    }
    return fclose(f) == 0 ? SU_OK : SU_E_IO;
}
