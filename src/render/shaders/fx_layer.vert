#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec3 v_pos;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    v_pos = in_pos;
    gl_Position = fr.view_proj * vec4(in_pos, 1.0);
}
