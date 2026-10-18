/* Portable part of sutekh/render.h: math, cameras, procedural meshes, debug lines and the PPM writer. No GPU. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include "sutekh/render.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "render/su_font5x7.h"

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

/* ---------- large worlds ---------- */

#define Q32_ONE 4294967296.0

SuWorldPos su_wpos(double x, double y, double z) {
    SuWorldPos p;
    p.x = (int64_t)llround(x * Q32_ONE);
    p.y = (int64_t)llround(y * Q32_ONE);
    p.z = (int64_t)llround(z * Q32_ONE);
    return p;
}

static int64_t wdiff(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a - (uint64_t)b); /* defined wrapping, reinterpreted as signed */
}

SuVec3f su_wpos_delta(SuWorldPos a, SuWorldPos origin) {
    return v3((float)((double)wdiff(a.x, origin.x) / Q32_ONE), (float)((double)wdiff(a.y, origin.y) / Q32_ONE),
              (float)((double)wdiff(a.z, origin.z) / Q32_ONE));
}

SuWorldPos su_wpos_lerp(SuWorldPos a, SuWorldPos b, float t) {
    SuWorldPos r;
    r.x = a.x + (int64_t)llround((double)wdiff(b.x, a.x) * (double)t);
    r.y = a.y + (int64_t)llround((double)wdiff(b.y, a.y) * (double)t);
    r.z = a.z + (int64_t)llround((double)wdiff(b.z, a.z) * (double)t);
    return r;
}

SuWorldPos su_wpos_add(SuWorldPos a, SuVec3f d) {
    a.x += (int64_t)llround((double)d.x * Q32_ONE);
    a.y += (int64_t)llround((double)d.y * Q32_ONE);
    a.z += (int64_t)llround((double)d.z * Q32_ONE);
    return a;
}

SuQuatf su_quatf_mul(SuQuatf a, SuQuatf b) {
    SuQuatf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

SuQuatf su_quatf_axis_angle(SuVec3f axis, float radians) {
    SuQuatf r;
    SuVec3f n = norm(axis);
    float s = sinf(radians * 0.5f);
    r.x = n.x * s;
    r.y = n.y * s;
    r.z = n.z * s;
    r.w = cosf(radians * 0.5f);
    return r;
}

SuMat4f su_mat4f_from_quat(SuQuatf q) {
    SuMat4f r = su_mat4f_identity();
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);
    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);
    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    return r;
}

SuMat4f su_mat4f_perspective_rz(float fov_y, float aspect, float near_z) {
    SuMat4f r;
    float f = 1.0f / tanf(fov_y * 0.5f);
    memset(&r, 0, sizeof r);
    r.m[0] = f / aspect;
    r.m[5] = -f; /* Vulkan clip space: y down */
    r.m[11] = -1.0f;
    r.m[14] = near_z;
    return r;
}

/* ---------- culling and LOD ---------- */

void su_frustum_from_matrix(SuFrustum *f, SuMat4f vp, int infinite_far) {
    float row[4][4];
    int r;
    int k;
    uint32_t n;
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 4; k++) {
            row[r][k] = vp.m[k * 4 + r];
        }
    }
    for (k = 0; k < 4; k++) {
        f->planes[0][k] = row[3][k] + row[0][k]; /* x >= -w */
        f->planes[1][k] = row[3][k] - row[0][k]; /* x <= w */
        f->planes[2][k] = row[3][k] + row[1][k]; /* y >= -w */
        f->planes[3][k] = row[3][k] - row[1][k]; /* y <= w */
        f->planes[4][k] = row[3][k] - row[2][k]; /* z <= w (the near plane under reversed Z) */
        f->planes[5][k] = row[2][k];             /* z >= 0 */
    }
    n = infinite_far ? 5u : 6u;
    for (r = 0; r < (int)n; r++) {
        float l = sqrtf(f->planes[r][0] * f->planes[r][0] + f->planes[r][1] * f->planes[r][1] +
                        f->planes[r][2] * f->planes[r][2]);
        if (l > 0.0f) {
            for (k = 0; k < 4; k++) {
                f->planes[r][k] /= l;
            }
        }
    }
    f->count = n;
}

int su_frustum_sphere(const SuFrustum *f, SuVec3f c, float radius) {
    uint32_t i;
    for (i = 0; i < f->count; i++) {
        const float *p = f->planes[i];
        if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -radius) {
            return 0;
        }
    }
    return 1;
}

float su_lod_screen_fraction(float radius, float distance, float proj_y) {
    if (distance <= radius || distance <= 0.0f) {
        return INFINITY;
    }
    return radius * proj_y / distance;
}

uint32_t su_lod_select(const float *lod_screen, uint32_t lod_count, float radius, float distance, float proj_y) {
    float frac = su_lod_screen_fraction(radius, distance, proj_y);
    uint32_t i;
    uint32_t best = 0;
    for (i = 1; i < lod_count; i++) {
        if (frac < lod_screen[i]) {
            best = i;
        }
    }
    return best;
}

/* ---------- effects ---------- */

static void fx_vertex(SuFxVertex *o, SuVec3f p, float u, float v, const float *c) {
    o->pos[0] = p.x;
    o->pos[1] = p.y;
    o->pos[2] = p.z;
    o->uv[0] = u;
    o->uv[1] = v;
    memcpy(o->color, c, sizeof o->color);
}

/* Quad a-b-c-d (a, b at v0; c, d at v1) as two triangles. */
static void fx_quad(SuFxBuffer *b, SuVec3f a, SuVec3f bb, SuVec3f c, SuVec3f d, float v0, float v1, const float *c0,
                    const float *c1) {
    if (b->count + 6u > b->cap) {
        b->dropped += 2u;
        return;
    }
    fx_vertex(&b->v[b->count++], a, 0.0f, v0, c0);
    fx_vertex(&b->v[b->count++], bb, 1.0f, v0, c0);
    fx_vertex(&b->v[b->count++], c, 1.0f, v1, c1);
    fx_vertex(&b->v[b->count++], a, 0.0f, v0, c0);
    fx_vertex(&b->v[b->count++], c, 1.0f, v1, c1);
    fx_vertex(&b->v[b->count++], d, 0.0f, v1, c1);
}

/* Billboards are marked by v < 0 (v = -1 at one edge, -2 at the other): the shader draws a round spot. */
void su_fx_billboard(SuFxBuffer *b, SuVec3f pos, float size, float rotation, const float color[4], SuMat4f view) {
    SuVec3f right = v3(view.m[0], view.m[4], view.m[8]);
    SuVec3f up = v3(view.m[1], view.m[5], view.m[9]);
    float cs = cosf(rotation) * size;
    float sn = sinf(rotation) * size;
    SuVec3f rx = add(scale(right, cs), scale(up, sn));
    SuVec3f ry = add(scale(right, -sn), scale(up, cs));
    fx_quad(b, sub(sub(pos, rx), ry), sub(add(pos, rx), ry), add(add(pos, rx), ry), add(sub(pos, rx), ry), -1.0f, -2.0f,
            color, color);
}

void su_fx_beam(SuFxBuffer *b, SuVec3f a, SuVec3f c, float width, const float color[4]) {
    SuVec3f dir = sub(c, a);
    SuVec3f mid = scale(add(a, c), 0.5f);
    SuVec3f side = cross(dir, mid);
    SuVec3f s;
    if (dot(side, side) < 1e-20f) {
        side = v3(0.0f, 1.0f, 0.0f);
    }
    s = scale(norm(side), width * 0.5f);
    fx_quad(b, sub(a, s), add(a, s), add(c, s), sub(c, s), 0.0f, 0.0f, color, color);
}

void su_fx_glare(SuFxBuffer *b, SuVec3f pos, float size, float streak, const float color[4], SuMat4f view) {
    SuVec3f axis[2];
    float halo[4];
    float sc[4];
    float len = streak > 0.0f ? streak : size * 8.0f;
    uint32_t a;
    uint32_t sgn;
    axis[0] = v3(view.m[0], view.m[4], view.m[8]);
    axis[1] = v3(view.m[1], view.m[5], view.m[9]);
    su_fx_billboard(b, pos, size, 0.0f, color, view);
    halo[0] = color[0];
    halo[1] = color[1];
    halo[2] = color[2];
    halo[3] = color[3] * 0.06f;
    su_fx_billboard(b, pos, size * 5.0f, 0.0f, halo, view);
    sc[0] = color[0];
    sc[1] = color[1];
    sc[2] = color[2];
    sc[3] = color[3] * 0.3f;
    for (a = 0; a < 2u; a++) {
        SuVec3f side = scale(axis[1u - a], size * 0.35f);
        for (sgn = 0; sgn < 2u; sgn++) {
            SuVec3f end = add(pos, scale(axis[a], sgn ? len : -len));
            fx_quad(b, sub(pos, side), add(pos, side), add(end, side), sub(end, side), 0.0f, 1.0f, sc, sc);
        }
    }
}

void su_trail_init(SuTrail *t, SuTrailPoint *storage, uint32_t cap, float lifetime_s, float spacing) {
    memset(t, 0, sizeof *t);
    t->p = storage;
    t->cap = cap;
    t->lifetime_s = lifetime_s;
    t->spacing = spacing;
}

void su_trail_reset(SuTrail *t) {
    t->head = 0;
    t->count = 0;
}

static SuTrailPoint *trail_at(const SuTrail *t, uint32_t age_index) {
    return &t->p[(t->head + t->cap - age_index) % t->cap];
}

void su_trail_push(SuTrail *t, SuWorldPos pos, double time_s) {
    if (t->cap < 2u) {
        return;
    }
    while (t->count > 0u && time_s - trail_at(t, t->count - 1u)->time_s > (double)t->lifetime_s) {
        t->count--;
    }
    if (t->count >= 2u) {
        SuVec3f d = su_wpos_delta(pos, trail_at(t, 1)->pos);
        if (dot(d, d) < t->spacing * t->spacing) {
            trail_at(t, 0)->pos = pos; /* the head follows the emitter until it is spacing away */
            trail_at(t, 0)->time_s = time_s;
            return;
        }
    }
    t->head = (t->head + 1u) % t->cap;
    t->p[t->head].pos = pos;
    t->p[t->head].time_s = time_s;
    if (t->count < t->cap) {
        t->count++;
    }
}

void su_trail_build(const SuTrail *t, double now_s, SuWorldPos camera, float width_head, float width_tail,
                    const float color_head[4], const float color_tail[4], SuFxBuffer *out) {
    uint32_t i;
    SuVec3f prev_l = v3(0, 0, 0);
    SuVec3f prev_r = v3(0, 0, 0);
    float prev_v = 0.0f;
    float prev_c[4] = {0, 0, 0, 0};
    int have = 0;
    for (i = 0; i < t->count; i++) {
        const SuTrailPoint *p = trail_at(t, i);
        float k = t->lifetime_s > 0.0f ? (float)(now_s - p->time_s) / t->lifetime_s : 0.0f;
        SuVec3f rel = su_wpos_delta(p->pos, camera);
        SuVec3f dir;
        SuVec3f side;
        float w;
        float c[4];
        int j;
        if (k > 1.0f) {
            break;
        }
        k = k < 0.0f ? 0.0f : k;
        if (i + 1u < t->count) {
            dir = sub(rel, su_wpos_delta(trail_at(t, i + 1u)->pos, camera));
        } else if (i > 0u) {
            dir = sub(su_wpos_delta(trail_at(t, i - 1u)->pos, camera), rel);
        } else {
            return;
        }
        side = cross(dir, rel);
        if (dot(side, side) < 1e-20f) {
            side = v3(0.0f, 1.0f, 0.0f);
        }
        w = (width_head + (width_tail - width_head) * k) * 0.5f;
        side = scale(norm(side), w);
        for (j = 0; j < 4; j++) {
            c[j] = color_head[j] + (color_tail[j] - color_head[j]) * k;
        }
        if (have) {
            fx_quad(out, prev_l, prev_r, add(rel, side), sub(rel, side), prev_v, k, prev_c, c);
        }
        prev_l = sub(rel, side);
        prev_r = add(rel, side);
        prev_v = k;
        memcpy(prev_c, c, sizeof c);
        have = 1;
    }
}

/* ---------- HUD and font ---------- */

#define GLYPH_ADVANCE 6.0f
#define LINE_ADVANCE 9.0f

const uint8_t *su_font_glyph(char c) {
    unsigned char u = (unsigned char)c;
    if (u < 32u || u > 126u) {
        u = '?';
    }
    return su_font5x7[u - 32u];
}

void su_font_atlas(uint8_t atlas[SU_FONT_ATLAS_W * SU_FONT_ATLAS_H]) {
    uint32_t g;
    uint32_t x;
    uint32_t y;
    memset(atlas, 0, SU_FONT_ATLAS_W * SU_FONT_ATLAS_H);
    for (g = 0; g < 95u; g++) {
        uint32_t cx = (g % 16u) * SU_FONT_CELL;
        uint32_t cy = (g / 16u) * SU_FONT_CELL;
        for (y = 0; y < 7u; y++) {
            for (x = 0; x < 5u; x++) {
                if ((su_font5x7[g][y] >> (4u - x)) & 1u) {
                    atlas[(cy + y) * SU_FONT_ATLAS_W + cx + x] = 255u;
                }
            }
        }
    }
    /* cell 95 (the last of row 5): solid, for rectangles */
    for (y = 0; y < SU_FONT_CELL; y++) {
        for (x = 0; x < SU_FONT_CELL; x++) {
            atlas[(5u * SU_FONT_CELL + y) * SU_FONT_ATLAS_W + 15u * SU_FONT_CELL + x] = 255u;
        }
    }
}

void su_hud_begin(SuHud *h, SuHudVertex *storage, uint32_t cap, uint32_t width, uint32_t height) {
    memset(h, 0, sizeof *h);
    h->v = storage;
    h->cap = cap;
    h->width = (float)width;
    h->height = (float)height;
}

static void hud_quad(SuHud *h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                     uint32_t rgba) {
    SuHudVertex q[4];
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    int i;
    if (h->count + 6u > h->cap) {
        h->dropped++;
        return;
    }
    q[0].pos[0] = x0, q[0].pos[1] = y0, q[0].uv[0] = u0, q[0].uv[1] = v0;
    q[1].pos[0] = x1, q[1].pos[1] = y0, q[1].uv[0] = u1, q[1].uv[1] = v0;
    q[2].pos[0] = x1, q[2].pos[1] = y1, q[2].uv[0] = u1, q[2].uv[1] = v1;
    q[3].pos[0] = x0, q[3].pos[1] = y1, q[3].uv[0] = u0, q[3].uv[1] = v1;
    for (i = 0; i < 4; i++) {
        q[i].rgba = rgba;
    }
    for (i = 0; i < 6; i++) {
        h->v[h->count++] = q[order[i]];
    }
}

void su_hud_rect(SuHud *h, float x, float y, float w, float hgt, uint32_t rgba) {
    /* the centre of the solid cell */
    float u = (15.0f * SU_FONT_CELL + 4.0f) / (float)SU_FONT_ATLAS_W;
    float v = (5.0f * SU_FONT_CELL + 4.0f) / (float)SU_FONT_ATLAS_H;
    hud_quad(h, x, y, x + w, y + hgt, u, v, u, v, rgba);
}

float su_hud_text(SuHud *h, float x, float y, float scale, uint32_t rgba, const char *text) {
    float cx = x;
    float cy = y;
    float widest = 0.0f;
    const char *s;
    for (s = text; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        uint32_t g;
        float u0;
        float v0;
        if (c == '\n') {
            widest = cx - x > widest ? cx - x : widest;
            cx = x;
            cy += LINE_ADVANCE * scale;
            continue;
        }
        if (c < 32u || c > 126u) {
            c = '?';
        }
        if (c != ' ') {
            g = (uint32_t)c - 32u;
            u0 = (float)((g % 16u) * SU_FONT_CELL) / (float)SU_FONT_ATLAS_W;
            v0 = (float)((g / 16u) * SU_FONT_CELL) / (float)SU_FONT_ATLAS_H;
            hud_quad(h, cx, cy, cx + GLYPH_ADVANCE * scale, cy + (float)SU_FONT_CELL * scale, u0, v0,
                     u0 + GLYPH_ADVANCE / (float)SU_FONT_ATLAS_W, v0 + (float)SU_FONT_CELL / (float)SU_FONT_ATLAS_H,
                     rgba);
        }
        cx += GLYPH_ADVANCE * scale;
    }
    widest = cx - x > widest ? cx - x : widest;
    return widest;
}

float su_hud_textf(SuHud *h, float x, float y, float scale, uint32_t rgba, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return su_hud_text(h, x, y, scale, rgba, buf);
}

void su_hud_measure(const char *text, float scale, float *w, float *hgt) {
    float line = 0.0f;
    float widest = 0.0f;
    uint32_t lines = 1;
    const char *s;
    for (s = text; *s != '\0'; s++) {
        if (*s == '\n') {
            widest = line > widest ? line : widest;
            line = 0.0f;
            lines++;
        } else {
            line += GLYPH_ADVANCE * scale;
        }
    }
    widest = line > widest ? line : widest;
    if (w != NULL) {
        *w = widest;
    }
    if (hgt != NULL) {
        *hgt = ((float)(lines - 1u) * LINE_ADVANCE + 7.0f) * scale;
    }
}
