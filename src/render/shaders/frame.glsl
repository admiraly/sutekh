// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Per-frame constants shared by the scene shaders (set 0, binding 0). Space is camera-relative: the camera sits at
// the origin and view_proj has no translation.
struct PointLight {
    vec4 position_radius;
    vec4 radiance;
};

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 sun_dir;      // xyz: direction the light travels; w: cos of the sun disc radius (0: no disc)
    vec4 sun_color;    // rgb: colour * intensity
    vec4 sky_zenith;   // rgb; w: ambient multiplier
    vec4 sky_horizon;
    vec4 sky_nadir;
    vec4 up;           // xyz; w: exposure
    vec4 params;       // x: tonemap mode, y: time in seconds
    mat4 shadow_vp[4]; // per cascade: camera-relative position -> shadow map clip space (x, y -1..1, depth 0..1)
    vec4 shadow_params; // x: strength (0: no shadow), z: 1 / map size, w: cascade count
    vec4 shadow_split;  // far view depth of each cascade
    vec4 shadow_bias;   // depth bias of each cascade
    vec4 shadow_noff;   // normal offset of each cascade (units)
    vec4 shadow_fwd;    // the camera's forward axis
    PointLight point_lights[128];
} fr;

#ifdef SU_SHADOW_RECEIVER
// The sun's shadow map, one layer per cascade (compare sampler: 1 where the reference depth is not farther).
layout(set = 0, binding = 1) uniform sampler2DArrayShadow t_shadow;

// Fraction of the sun reaching p (camera-relative) with geometric normal n: the cascade p's view depth falls in, then
// a 3 x 3 percentage-closer filter.
float sun_shadow(vec3 p, vec3 n) {
    if (fr.shadow_params.x <= 0.0) {
        return 1.0;
    }
    int count = int(fr.shadow_params.w + 0.5);
    int c = 0;
    if (count > 1) {
        float d = dot(p, fr.shadow_fwd.xyz);
        while (c < count && d > fr.shadow_split[c]) {
            c++;
        }
        if (c == count) {
            return 1.0;
        }
    }
    vec4 s = fr.shadow_vp[c] * vec4(p + n * fr.shadow_noff[c], 1.0);
    if (abs(s.x) > 1.0 || abs(s.y) > 1.0 || s.z > 1.0) {
        return 1.0;
    }
    vec2 uv = s.xy * 0.5 + 0.5;
    float ref = s.z - fr.shadow_bias[c];
    float lit = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            lit += texture(t_shadow, vec4(uv + vec2(float(x), float(y)) * fr.shadow_params.z, float(c), ref));
        }
    }
    return 1.0 - fr.shadow_params.x * (1.0 - lit / 9.0);
}
#endif

vec3 sky_base(vec3 d) {
    float h = dot(d, fr.up.xyz);
    return h >= 0.0 ? mix(fr.sky_horizon.rgb, fr.sky_zenith.rgb, sqrt(h))
                    : mix(fr.sky_horizon.rgb, fr.sky_nadir.rgb, sqrt(-h));
}

// The sky seen along d, with the sun's glow (used for reflections and the background).
vec3 sky_radiance(vec3 d) {
    float s = max(dot(d, -fr.sun_dir.xyz), 0.0);
    return sky_base(d) + fr.sun_color.rgb * (0.02 * pow(s, 8.0) + 0.25 * pow(s, 256.0));
}
