// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Sun shadow map: asset mesh casters (the PBR vertex layout and instance rows), depth only, in the sun's clip space.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 5) in vec4 i_r0;
layout(location = 6) in vec4 i_r1;
layout(location = 7) in vec4 i_r2;
layout(push_constant) uniform Cascade {
    uint index;
} pc;

void main() {
    vec3 p = vec3(dot(i_r0.xyz, in_pos) + i_r0.w, dot(i_r1.xyz, in_pos) + i_r1.w, dot(i_r2.xyz, in_pos) + i_r2.w);
    gl_Position = fr.shadow_vp[pc.index] * vec4(p, 1.0);
}
