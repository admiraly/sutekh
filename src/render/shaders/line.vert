// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Debug lines: world-space position and an RGBA8 colour per vertex.
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(push_constant) uniform PC {
    mat4 view_proj;
    vec4 light_dir_ambient;
} pc;
layout(location = 0) out vec4 v_color;

void main() {
    v_color = in_color;
    gl_Position = pc.view_proj * vec4(in_pos, 1.0);
}
