// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Lambert lighting with an ambient floor; output is linear (the target format applies sRGB encoding).
#version 450
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(push_constant) uniform PC {
    mat4 view_proj;
    vec4 light_dir_ambient;
} pc;
layout(location = 0) out vec4 o_color;

void main() {
    float d = max(dot(normalize(v_normal), -normalize(pc.light_dir_ambient.xyz)), 0.0);
    float a = pc.light_dir_ambient.w;
    o_color = vec4(v_color.rgb * (a + (1.0 - a) * d), v_color.a);
}
