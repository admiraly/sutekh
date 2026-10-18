// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Output pass with bloom and optional auto exposure: the HDR scene plus the bloom chain's result times its
// scale, then exposure (manual times automatic), then the same tonemap as post.frag. Intensity 0 gives exactly the
// scene without bloom (x + 0 * y == x).
#version 450
layout(set = 0, binding = 0) uniform sampler2D t_hdr;
layout(set = 0, binding = 1) uniform sampler2D t_bloom;
layout(set = 0, binding = 2) uniform sampler2D t_small;
layout(push_constant) uniform Post {
    vec4 a;   // x: manual exposure, y: tonemap mode, z: bloom scale (intensity / levels), w: auto exposure on
    vec4 b;   // x: auto key (target average luminance), y: minimum auto exposure, z: maximum auto exposure
} pc;
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(t_hdr, 0));
    vec3 c = texelFetch(t_hdr, ivec2(gl_FragCoord.xy), 0).rgb + texture(t_bloom, uv).rgb * pc.a.z;
    float ex = pc.a.x;
    if (pc.a.w > 0.5) {
        ex *= clamp(pc.b.x / exp2(texture(t_small, vec2(0.5)).a), pc.b.y, pc.b.z);
    }
    c *= ex;
    o_color = vec4(pc.a.y == 0.0 ? aces(c) : clamp(c, 0.0, 1.0), 1.0);
}
