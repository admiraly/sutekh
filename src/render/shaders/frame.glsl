// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Per-frame constants shared by the scene shaders (set 0, binding 0). Space is camera-relative: the camera sits at
// the origin and view_proj has no translation.
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
} fr;

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
