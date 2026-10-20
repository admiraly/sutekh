#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 3) in vec2 in_uv0;
layout(location = 5) in vec4 i_r0;
layout(location = 6) in vec4 i_r1;
layout(location = 7) in vec4 i_r2;
layout(location = 8) in vec4 i_tint;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec3 v_pos;

void main() {
    vec4 point = vec4(in_pos, 1.0);
    v_pos = vec3(dot(i_r0, point), dot(i_r1, point), dot(i_r2, point));
    v_uv = in_uv0;
    v_color = i_tint;
    gl_Position = fr.view_proj * vec4(v_pos, 1.0);
}
