#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
layout(location=0) in mat4 in_transform;
layout(location=4) in vec4 in_colour;
layout(location=5) in vec4 in_noise;
layout(location=6) in vec4 in_control;
layout(location=7) in vec4 in_shape;
layout(location=8) in vec4 in_motion;
layout(location=0) out vec3 ray;
layout(location=1) flat out mat4 world_to_local;
layout(location=5) flat out vec4 colour;
layout(location=6) flat out vec4 noise;
layout(location=7) flat out vec4 control;
layout(location=8) flat out vec4 shape;
layout(location=9) flat out vec4 motion;
void main() {
    vec2 lower = vec2(1.0);
    vec2 upper = vec2(-1.0);
    float nearest = 0.0;
    bool clipped = false;
    for (int corner = 0; corner < 8; corner++) {
        vec3 local = vec3((corner & 1) != 0 ? 1.0 : -1.0,
                          (corner & 2) != 0 ? 1.0 : -1.0,
                          (corner & 4) != 0 ? 0.0 : -1.0);
        vec4 clip = fr.view_proj * in_transform * vec4(local, 1.0);
        if (clip.w <= 0.0 || clip.z >= clip.w) {
            clipped = true;
        } else {
            lower = min(lower, clip.xy / clip.w);
            upper = max(upper, clip.xy / clip.w);
            nearest = max(nearest, clip.z / clip.w);
        }
    }
    if (clipped) {
        lower = vec2(-1.0);
        upper = vec2(1.0);
        nearest = 1.0;
    }
    lower = clamp(lower, vec2(-1.0), vec2(1.0));
    upper = clamp(upper, vec2(-1.0), vec2(1.0));
    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    vec2 ndc = mix(lower, upper, corners[gl_VertexIndex]);
    vec4 point = fr.inv_view_proj * vec4(ndc, 1.0, 1.0);
    ray = point.xyz / point.w;
    world_to_local = inverse(in_transform);
    colour = in_colour;
    noise = in_noise;
    control = in_control;
    shape = in_shape;
    motion = in_motion;
    gl_Position = vec4(ndc, nearest, 1.0);
}
