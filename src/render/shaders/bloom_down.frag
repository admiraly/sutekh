// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Bloom downsample: the 13-tap filter of Jimenez (2014, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare"), five overlapping 2x2 groups of bilinear taps. Mode 1 (the first level) also applies the thresholded
// soft knee to every tap and weights each group by 1 / (1 + luma) so single hot pixels do not flicker (Karis).
// The alpha channel carries log2(luma) of the unthresholded taps; averaged down the chain it gives the
// log-average scene luminance that the optional auto exposure reads from the smallest level.
#version 450
layout(set = 0, binding = 0) uniform sampler2D t_src;
layout(push_constant) uniform Bloom {
    vec4 a;   // x: threshold, y: knee (absolute, same unit as the threshold), z: mode (1: prefilter)
    vec4 b;   // xy: 1 / destination size
} pc;
layout(location = 0) out vec4 o_color;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 knee_cut(vec3 c) {
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - pc.a.x + pc.a.y, 0.0, 2.0 * pc.a.y);
    soft = soft * soft / (4.0 * pc.a.y + 1e-5);
    float contrib = max(soft, br - pc.a.x) / max(br, 1e-5);
    return c * max(contrib, 0.0);
}

vec4 tap(vec2 uv, vec2 o) {
    vec4 s = texture(t_src, uv + o);
    s.rgb = min(s.rgb, vec3(60000.0));
    if (pc.a.z > 0.5) {
        s.a = log2(max(luma(s.rgb), 1e-4));
        s.rgb = knee_cut(s.rgb);
    }
    return s;
}

void main() {
    vec2 uv = gl_FragCoord.xy * pc.b.xy;
    vec2 t = 1.0 / vec2(textureSize(t_src, 0));
    vec4 a = tap(uv, t * vec2(-2, -2)), b = tap(uv, t * vec2(0, -2)), c = tap(uv, t * vec2(2, -2));
    vec4 d = tap(uv, t * vec2(-2, 0)), e = tap(uv, vec2(0)), f = tap(uv, t * vec2(2, 0));
    vec4 g = tap(uv, t * vec2(-2, 2)), h = tap(uv, t * vec2(0, 2)), i = tap(uv, t * vec2(2, 2));
    vec4 j = tap(uv, t * vec2(-1, -1)), k = tap(uv, t * vec2(1, -1));
    vec4 l = tap(uv, t * vec2(-1, 1)), m = tap(uv, t * vec2(1, 1));
    vec4 g0 = (j + k + l + m) * 0.25;
    vec4 g1 = (a + b + d + e) * 0.25;
    vec4 g2 = (b + c + e + f) * 0.25;
    vec4 g3 = (d + e + g + h) * 0.25;
    vec4 g4 = (e + f + h + i) * 0.25;
    if (pc.a.z > 0.5) {
        float w0 = 0.5 / (1.0 + luma(g0.rgb)), w1 = 0.125 / (1.0 + luma(g1.rgb)), w2 = 0.125 / (1.0 + luma(g2.rgb));
        float w3 = 0.125 / (1.0 + luma(g3.rgb)), w4 = 0.125 / (1.0 + luma(g4.rgb));
        vec3 rgb = (g0.rgb * w0 + g1.rgb * w1 + g2.rgb * w2 + g3.rgb * w3 + g4.rgb * w4) / (w0 + w1 + w2 + w3 + w4);
        float lg = g0.a * 0.5 + (g1.a + g2.a + g3.a + g4.a) * 0.125;
        o_color = vec4(rgb, lg);
    } else {
        o_color = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    }
}
