// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Cube-map sky background: radiance = cube(R * d, lod) * intensity, where R (rows r0..r2) takes a world direction into
// the cube's frame (the inverse of the sky rotation).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(set = 1, binding = 0) uniform samplerCube u_cube;
layout(push_constant) uniform Sky {
    vec4 r0;
    vec4 r1;
    vec4 r2;
    vec4 p;        // x: intensity, y: lod
} sk;

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 p = fr.inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 d = normalize(p.xyz / p.w);
    vec3 c = vec3(dot(sk.r0.xyz, d), dot(sk.r1.xyz, d), dot(sk.r2.xyz, d));
    o_color = vec4(textureLod(u_cube, c, sk.p.y).rgb * sk.p.x, 1.0);
}
