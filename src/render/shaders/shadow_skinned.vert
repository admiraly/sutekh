// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Sun shadow map: skinned asset mesh casters, depth only, in the sun's clip space.
#version 450
#extension GL_GOOGLE_include_directive : require
#define SU_SKINNED
#include "frame.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 5) in vec4 i_r0;
layout(location = 6) in vec4 i_r1;
layout(location = 7) in vec4 i_r2;
layout(location = 9) in vec4 i_extra;
layout(location = 11) in uvec4 in_joints;
layout(location = 12) in vec4 in_weights;
layout(push_constant) uniform Cascade {
    uint index;
} pc;

void main() {
    vec3 p_local = in_pos;
    uint skin_base = floatBitsToUint(i_extra.y);
    uint skin_cnt = floatBitsToUint(i_extra.z);
    if (skin_cnt > 0u) {
        float wmax = max(max(in_weights.x, in_weights.y), max(in_weights.z, in_weights.w));
        if (wmax > 0.0) {
            /* Split -exponent across two ldexp calls so each shift stays inside GLSL's
             * [-126, +128] guaranteed range even for FLT_MAX (128) or subnormals (-148). */
            int exponent;
            frexp(wmax, exponent);
            int half_exp = exponent / 2;
            vec4 weights = ldexp(ldexp(in_weights, ivec4(-half_exp)), ivec4(-(exponent - half_exp)));
            float wsum = weights.x + weights.y + weights.z + weights.w;
            if (wsum > 0.0) {
                uint j0 = min(in_joints.x, skin_cnt - 1u);
                uint j1 = min(in_joints.y, skin_cnt - 1u);
                uint j2 = min(in_joints.z, skin_cnt - 1u);
                uint j3 = min(in_joints.w, skin_cnt - 1u);
                mat4 sm = (skin_mats[skin_base + j0] * weights.x +
                           skin_mats[skin_base + j1] * weights.y +
                           skin_mats[skin_base + j2] * weights.z +
                           skin_mats[skin_base + j3] * weights.w) / wsum;
                p_local = (sm * vec4(in_pos, 1.0)).xyz;
            }
        }
    }
    vec3 p = vec3(dot(i_r0.xyz, p_local) + i_r0.w, dot(i_r1.xyz, p_local) + i_r1.w, dot(i_r2.xyz, p_local) + i_r2.w);
    gl_Position = fr.shadow_vp[pc.index] * vec4(p, 1.0);
}
