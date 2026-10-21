// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Tonemapping from the linear HDR scene to the sRGB output (the attachment format applies the sRGB encoding).
// Mode 0: exposure, then the ACES-fitted curve of Narkowicz (2015); mode 1: exposure, then clamp.
#version 450
layout(set = 0, binding = 0) uniform sampler2D t_hdr;
layout(push_constant) uniform Post {
    vec4 p;   // x: exposure, y: mode (plus 2 to retain coverage), zw: 1 / output size
} pc;
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec4 scene = texelFetch(t_hdr, ivec2(gl_FragCoord.xy), 0);
    vec3 c = scene.rgb * pc.p.x;
    o_color = vec4(mod(pc.p.y, 2.0) == 0.0 ? aces(c) : clamp(c, 0.0, 1.0),
                   pc.p.y >= 2.0 ? clamp(scene.a, 0.0, 1.0) : 1.0);
}
