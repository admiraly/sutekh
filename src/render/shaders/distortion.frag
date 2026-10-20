#version 450
#extension GL_GOOGLE_include_directive : require
#define SU_DISTORTION
#include "fx_layers.glsl"

void main() {
    vec2 point = v_uv * 2.0 - 1.0;
    float radius = length(point);
    float envelope = pc.distortion.y > 0.0 ? pow(max(1.0 - radius, 0.0), pc.distortion.y) : 1.0;
    vec2 direction;
    float opacity = clamp(v_color.a, 0.0, 1.0) * layer_fade();
    if (pc.distortion.w < 1.5) {
        vec4 normal = layer_sample();
        direction = clamp(normal.rg, 0.0, 1.0) * 2.0 - 1.0;
        opacity *= clamp(normal.a, 0.0, 1.0);
    } else {
        vec2 radial = radius > 1e-6 ? point / radius : vec2(0.0);
        direction = pc.distortion.w < 2.5 ? vec2(-radial.y, radial.x) : radial;
        if (pc.distortion.w > 2.5) {
            envelope *= max(1.0 - abs(radius - pc.ring.x) / pc.ring.y, 0.0);
        }
        if (radius >= 1.0) discard;
    }
    vec2 offset = direction * pc.distortion.x * envelope * opacity;
    if (opacity <= 0.0 || dot(offset, offset) < 1e-12) discard;
    o_color = vec4(offset, pc.distortion.z, gl_FragCoord.z);
}
