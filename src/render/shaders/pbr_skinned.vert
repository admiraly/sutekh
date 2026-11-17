// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Skinned asset meshes: 56-byte base vertices + 24-byte skin attributes (joints + weights), per-frame joint palette.
#version 450
#extension GL_GOOGLE_include_directive : require
#define SU_SKINNED
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
layout(location = 10) in uvec4 i_light_mask;
layout(location = 11) in uvec4 in_joints;
layout(location = 12) in vec4 in_weights;

layout(location = 0) out vec3 v_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec4 v_uv;
layout(location = 4) out vec4 v_tint;
layout(location = 5) out float v_emissive;
layout(location = 6) flat out uvec4 v_light_mask;

void main() {
    vec3 p_local = in_pos;
    vec3 n_local = in_normal;
    vec3 t_local = in_tangent.xyz;
    float handedness = in_tangent.w;
    uint skin_base = floatBitsToUint(i_extra.y);
    uint skin_cnt = floatBitsToUint(i_extra.z);
    if (skin_cnt > 0u) {
        float wmax = max(max(in_weights.x, in_weights.y), max(in_weights.z, in_weights.w));
        if (wmax > 0.0) {
            /* Power-of-two scaling avoids both an overflowing sum and a subnormal reciprocal. */
            int exponent;
            frexp(wmax, exponent);
            vec4 weights = ldexp(in_weights, ivec4(-exponent));
            float wsum = weights.x + weights.y + weights.z + weights.w;
            uint j0 = min(in_joints.x, skin_cnt - 1u);
            uint j1 = min(in_joints.y, skin_cnt - 1u);
            uint j2 = min(in_joints.z, skin_cnt - 1u);
            uint j3 = min(in_joints.w, skin_cnt - 1u);
            mat4 sm = (skin_mats[skin_base + j0] * weights.x +
                       skin_mats[skin_base + j1] * weights.y +
                       skin_mats[skin_base + j2] * weights.z +
                       skin_mats[skin_base + j3] * weights.w) / wsum;
            p_local = (sm * vec4(in_pos, 1.0)).xyz;
            mat3 sm3 = mat3(sm);
            vec3 sc0 = cross(sm3[1], sm3[2]);
            vec3 sc1 = cross(sm3[2], sm3[0]);
            vec3 sc2 = cross(sm3[0], sm3[1]);
            float sdet = dot(sm3[0], sc0);
            if (abs(sdet) > 1e-20) {
                n_local = (sc0 * in_normal.x + sc1 * in_normal.y + sc2 * in_normal.z) * (sdet < 0.0 ? -1.0 : 1.0);
            } else {
                n_local = sm3 * in_normal;
            }
            t_local = sm3 * in_tangent.xyz;
            if (sdet < 0.0) handedness = -handedness;
        }
    }
    mat3 m = mat3(vec3(i_r0.x, i_r1.x, i_r2.x), vec3(i_r0.y, i_r1.y, i_r2.y), vec3(i_r0.z, i_r1.z, i_r2.z));
    vec3 p = m * p_local + vec3(i_r0.w, i_r1.w, i_r2.w);
    vec3 c0 = cross(m[1], m[2]);
    vec3 c1 = cross(m[2], m[0]);
    vec3 c2 = cross(m[0], m[1]);
    float det = dot(m[0], c0);
    v_pos = p;
    v_normal = (c0 * n_local.x + c1 * n_local.y + c2 * n_local.z) * (det < 0.0 ? -1.0 : 1.0);
    v_tangent = vec4(m * t_local, handedness);
    v_uv = vec4(in_uv0, in_uv1);
    v_tint = i_tint;
    v_emissive = i_extra.x;
    v_light_mask = i_light_mask;
    gl_Position = fr.view_proj * vec4(p, 1.0);
}
