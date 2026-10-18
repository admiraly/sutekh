// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// HUD quads in pixels (origin top-left). Colours arrive sRGB-encoded and are decoded here, because the sRGB
// attachment encodes the shader's linear output.
#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(push_constant) uniform Post {
    vec4 p;   // zw: 1 / output size
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

vec3 to_linear(vec3 s) {
    return mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), s));
}

void main() {
    v_uv = in_uv;
    v_color = vec4(to_linear(in_color.rgb), in_color.a);
    gl_Position = vec4(in_pos * pc.p.zw * 2.0 - 1.0, 0.0, 1.0);
}
