// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// HUD: font-atlas coverage times the vertex colour, straight alpha.
#version 450
layout(set = 0, binding = 0) uniform sampler2D t_font;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(v_color.rgb, v_color.a * texture(t_font, v_uv).r);
}
