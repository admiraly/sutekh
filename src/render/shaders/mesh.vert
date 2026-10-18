// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Instanced mesh: per-vertex position and normal, per-instance position + uniform scale, rotation quaternion, colour.
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 i_pos_scale;
layout(location = 3) in vec4 i_rot;
layout(location = 4) in vec4 i_color;
layout(push_constant) uniform PC {
    mat4 view_proj;
    vec4 light_dir_ambient;
} pc;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_color;

vec3 qrot(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

void main() {
    vec3 p = qrot(i_rot, in_pos * i_pos_scale.w) + i_pos_scale.xyz;
    v_normal = qrot(i_rot, in_normal);
    v_color = i_color;
    gl_Position = pc.view_proj * vec4(p, 1.0);
}
