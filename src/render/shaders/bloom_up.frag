// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Bloom upsample: a 3x3 tent filter of the smaller level (radius scales the tap spacing) added to the matching
// downsampled level. The chain is energy-preserving per level; the output pass divides by the level count.
#version 450
layout(set = 0, binding = 0) uniform sampler2D t_low;
layout(set = 0, binding = 1) uniform sampler2D t_hi;
layout(push_constant) uniform Bloom {
    vec4 a;   // x: radius (tap spacing in texels of the smaller level)
    vec4 b;   // xy: 1 / destination size
} pc;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = gl_FragCoord.xy * pc.b.xy;
    vec2 t = pc.a.x / vec2(textureSize(t_low, 0));
    vec3 s = texture(t_low, uv + t * vec2(-1, -1)).rgb + texture(t_low, uv + t * vec2(1, -1)).rgb +
             texture(t_low, uv + t * vec2(-1, 1)).rgb + texture(t_low, uv + t * vec2(1, 1)).rgb;
    vec3 e = texture(t_low, uv + t * vec2(0, -1)).rgb + texture(t_low, uv + t * vec2(0, 1)).rgb +
             texture(t_low, uv + t * vec2(-1, 0)).rgb + texture(t_low, uv + t * vec2(1, 0)).rgb;
    vec3 c = texture(t_low, uv).rgb;
    vec3 tent = (s + 2.0 * e + 4.0 * c) * (1.0 / 16.0);
    o_color = vec4(tent + texelFetch(t_hi, ivec2(gl_FragCoord.xy), 0).rgb, 0.0);
}
