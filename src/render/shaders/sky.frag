// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Procedural sky: the zenith/horizon/nadir gradient, the sun's glow and disc, and a sparse hashed star field.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

float hash3(vec3 c) {
    return fract(sin(dot(c, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

void main() {
    vec4 p = fr.inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 d = normalize(p.xyz / p.w);
    vec3 c = sky_radiance(d);
    if (fr.sun_dir.w > 0.0 && dot(d, -fr.sun_dir.xyz) > fr.sun_dir.w) {
        c += fr.sun_color.rgb * 40.0;
    }
    vec3 cell = floor(d * 300.0);
    float h = hash3(cell);
    if (h > 0.9975) {
        vec3 centre = (cell + 0.5) / 300.0;
        float k = 1.0 - smoothstep(0.0, 0.0018, length(d - normalize(centre)));
        c += vec3(0.8, 0.85, 1.0) * k * (h - 0.9975) * 400.0;
    }
    o_color = vec4(c, 1.0);
}
