// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Asset meshes: 56-byte vertices; per instance the rows of a camera-relative affine transform (any scale or
// mirror), a tint and an emissive multiplier. Normals use the cofactor matrix (inverse transpose up to scale),
// with the determinant's sign so mirrored transforms keep outward normals.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv0;
layout(location = 4) in vec2 in_uv1;
layout(location = 5) in vec4 i_r0;
layout(location = 6) in vec4 i_r1;
layout(location = 7) in vec4 i_r2;
layout(location = 8) in vec4 i_tint;
layout(location = 9) in vec4 i_extra;

layout(location = 0) out vec3 v_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec4 v_uv;
layout(location = 4) out vec4 v_tint;
layout(location = 5) out float v_emissive;

void main() {
    mat3 m = mat3(vec3(i_r0.x, i_r1.x, i_r2.x), vec3(i_r0.y, i_r1.y, i_r2.y), vec3(i_r0.z, i_r1.z, i_r2.z));
    vec3 p = m * in_pos + vec3(i_r0.w, i_r1.w, i_r2.w);
    vec3 c0 = cross(m[1], m[2]);
    vec3 c1 = cross(m[2], m[0]);
    vec3 c2 = cross(m[0], m[1]);
    float det = dot(m[0], c0);
    v_pos = p;
    v_normal = (c0 * in_normal.x + c1 * in_normal.y + c2 * in_normal.z) * (det < 0.0 ? -1.0 : 1.0);
    v_tangent = vec4(m * in_tangent.xyz, in_tangent.w);
    v_uv = vec4(in_uv0, in_uv1);
    v_tint = i_tint;
    v_emissive = i_extra.x;
    gl_Position = fr.view_proj * vec4(p, 1.0);
}
