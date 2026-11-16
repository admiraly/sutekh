// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Stylized / toon shading: UV transform (scale, rotation, offset), optional per-vertex color,
// stepped/Half-Lambert cel diffuse with shadow tint, stylized Blinn-Phong specular band, Fresnel rim.
#version 450
#extension GL_GOOGLE_include_directive : require
#define SU_SHADOW_RECEIVER
#include "frame.glsl"

layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec4 v_uv;
layout(location = 4) in vec4 v_tint;
layout(location = 5) in float v_emissive;
layout(location = 6) flat in uvec4 v_light_mask;
layout(location = 7) in vec4 v_color;

layout(set = 1, binding = 0) uniform sampler2D t_base;
layout(set = 1, binding = 1) uniform sampler2D t_mr;
layout(set = 1, binding = 2) uniform sampler2D t_normal;
layout(set = 1, binding = 3) uniform sampler2D t_emissive;
layout(set = 1, binding = 4) uniform sampler2D t_occlusion;

layout(set = 1, binding = 5) uniform Toon {
    vec4 uv_xform;      // xy: scale, zw: offset
    vec4 uv_rot;        // x: cos(rot), y: sin(rot), z: shade_threshold, w: shade_softness
    vec4 shadow_color;  // rgb: shadow tint
    vec4 spec_color;    // rgb: specular color, w: specular_threshold
    vec4 spec_params;   // x: specular_softness, y: specular_intensity, z: glossiness, w: rim_power
    vec4 rim_color;     // rgb: rim color, w: rim_intensity
    uvec4 params;       // x: diffuse_bands, y: flags (bit 0 half-lambert, bit 1 vertex color, bit 2 rim lit mask)
} toon;

layout(push_constant) uniform Material {
    vec4 base_color;
    vec4 emissive;   // rgb: factor * strength
    vec4 mr;         // metallic, roughness, normal scale, occlusion strength
    vec4 misc;       // alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend), unlit, has normal map
    uvec4 uvset;     // x: bit i = slot i reads UV1
} mat;

layout(location = 0) out vec4 o_color;

const float PI = 3.14159265;
#include "point_lights.glsl"

vec2 uv_of(uint slot) {
    vec2 raw = ((mat.uvset.x >> slot) & 1u) != 0u ? v_uv.zw : v_uv.xy;
    vec2 scaled = raw * toon.uv_xform.xy;
    float c = toon.uv_rot.x;
    float s = toon.uv_rot.y;
    return vec2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y) + toon.uv_xform.zw;
}

void main() {
    vec4 vcol = (toon.params.y & 2u) != 0u ? v_color : vec4(1.0);
    vec4 base = mat.base_color * v_tint * vcol * texture(t_base, uv_of(0u));
    if (mat.misc.y == 1.0 && base.a < mat.misc.x) {
        discard;
    }
    vec3 emit = mat.emissive.rgb * texture(t_emissive, uv_of(3u)).rgb * v_emissive;
    if (mat.misc.z != 0.0) {
        o_color = vec4(base.rgb + emit, mat.misc.y == 2.0 ? base.a : 1.0);
        return;
    }
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) {
        n = -n;
    }
    if (mat.misc.w != 0.0) {
        vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
        float tl = dot(t, t);
        if (tl > 1e-12) {
            t *= inversesqrt(tl);
            vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
            vec3 tn;
            tn.xy = texture(t_normal, uv_of(2u)).xy * 2.0 - 1.0;
            tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
            tn.xy *= mat.mr.z;
            n = normalize(t * tn.x + b * tn.y + n * tn.z);
        }
    }

    float ao = mix(1.0, texture(t_occlusion, uv_of(4u)).r, mat.mr.w);
    vec3 v = normalize(-v_pos);
    vec3 l = -fr.sun_dir.xyz;
    vec3 h = normalize(v + l);
    float nl_raw = dot(n, l);
    float nl = (toon.params.y & 1u) != 0u ? (nl_raw * 0.5 + 0.5) : nl_raw;
    float shadow_mask = nl_raw > 0.0 ? sun_shadow(v_pos, normalize(v_normal) * (gl_FrontFacing ? 1.0 : -1.0)) : 1.0;

    float thr = toon.uv_rot.z;
    float soft = max(toon.uv_rot.w, 1e-4);
    uint nbands = max(toon.params.x, 1u);
    float bands = float(nbands);
    float cel = 0.0;
    if (nbands == 1u) {
        cel = smoothstep(thr - soft, thr + soft, nl);
    } else {
        for (uint bi = 0u; bi < nbands; bi++) {
            float edge = mix(thr, 1.0, float(bi) / bands);
            cel += smoothstep(edge - soft, edge + soft, nl);
        }
        cel /= bands;
    }
    cel *= shadow_mask;

    vec3 lit_tint = mix(toon.shadow_color.rgb, vec3(1.0), cel);
    vec3 diffuse = base.rgb * lit_tint * fr.sun_color.rgb;

    float nh = max(dot(n, h), 0.0);
    float spec_raw = pow(nh, toon.spec_params.z);
    float spec_thr = toon.spec_color.w;
    float spec_soft = max(toon.spec_params.x, 1e-4);
    float spec_band = smoothstep(spec_thr - spec_soft, spec_thr + spec_soft, spec_raw);
    vec3 spec = toon.spec_color.rgb * (toon.spec_params.y * spec_band * cel) * fr.sun_color.rgb;

    float nv = max(dot(n, v), 0.0);
    float rim_raw = pow(1.0 - nv, toon.spec_params.w);
    float rim_mask = (toon.params.y & 4u) != 0u ? cel : 1.0;
    vec3 rim = toon.rim_color.rgb * (toon.rim_color.w * rim_raw * rim_mask);

    vec3 direct = diffuse + spec;
    if (any(notEqual(v_light_mask, uvec4(0u)))) {
        direct += point_lighting(v_light_mask, v_pos, n, v, base.rgb, vec3(0.04), 1.0, 0.5);
    }
    vec3 amb = sky_base(n) * base.rgb * fr.sky_zenith.w * ao;

    o_color = vec4(direct + amb + rim + emit, mat.misc.y == 2.0 ? base.a : 1.0);
}
