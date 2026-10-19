// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Textured additive effects: dst + t.rgb * t.a * c.rgb * c.a (linear HDR; the texture's format decides sRGB or linear).
#version 450
layout(set = 1, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 t = texture(u_tex, v_uv);
    o_color = vec4(t.rgb * t.a * v_color.rgb * v_color.a, 0.0);
}
