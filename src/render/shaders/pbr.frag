// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// PBR-lite: metallic-roughness (glTF), GGX specular with a height-correlated Smith visibility and Schlick Fresnel,
// Lambert diffuse, one directional sun, image-based-lighting-lite from the procedural sky (sky colour along the
// normal for diffuse; along the reflection, blurred toward the normal's by roughness, with an analytic environment
// BRDF for specular), tangent-space normal maps, occlusion, emissive. Output is linear HDR.
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

layout(set = 1, binding = 0) uniform sampler2D t_base;
layout(set = 1, binding = 1) uniform sampler2D t_mr;
layout(set = 1, binding = 2) uniform sampler2D t_normal;
layout(set = 1, binding = 3) uniform sampler2D t_emissive;
layout(set = 1, binding = 4) uniform sampler2D t_occlusion;

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
    return ((mat.uvset.x >> slot) & 1u) != 0u ? v_uv.zw : v_uv.xy;
}

void main() {
    vec4 base = mat.base_color * v_tint * texture(t_base, uv_of(0u));
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
            /* x and y from the map; z rebuilt (two-channel BC5 maps carry no z; for RGB maps it is the same) */
            vec3 tn;
            tn.xy = texture(t_normal, uv_of(2u)).xy * 2.0 - 1.0;
            tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
            tn.xy *= mat.mr.z;
            n = normalize(t * tn.x + b * tn.y + n * tn.z);
        }
    }
    vec4 mrs = texture(t_mr, uv_of(1u));
    float metallic = clamp(mat.mr.x * mrs.b, 0.0, 1.0);
    float rough = clamp(mat.mr.y * mrs.g, 0.04, 1.0);
    float ao = mix(1.0, texture(t_occlusion, uv_of(4u)).r, mat.mr.w);

    vec3 v = normalize(-v_pos);
    vec3 l = -fr.sun_dir.xyz;
    vec3 h = normalize(v + l);
    float nl = max(dot(n, l), 0.0);
    float nv = max(dot(n, v), 1e-4);
    float nh = max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    float a = rough * rough;
    float a2 = a * a;
    vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
    vec3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float d = a2 / (PI * dd * dd);
    float vis = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1e-6);
    vec3 diffuse = (1.0 - f) * (1.0 - metallic) * base.rgb / PI;
    vec3 direct = (diffuse + d * vis * f) * fr.sun_color.rgb * nl;
    if (nl > 0.0) {
        direct *= sun_shadow(v_pos, normalize(v_normal) * (gl_FrontFacing ? 1.0 : -1.0));
    }

    if (any(notEqual(v_light_mask, uvec4(0u)))) {
        direct += point_lighting(v_light_mask, v_pos, n, v, base.rgb, f0, 1.0 - metallic, rough);
    }

    // ambient: analytic environment BRDF (Karis, mobile approximation)
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 rr = rough * c0 + c1;
    float a004 = min(rr.x * rr.x, exp2(-9.28 * nv)) * rr.x + rr.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + rr.zw;
    vec3 refl = reflect(-v, n);
    vec3 spec_env = mix(sky_radiance(refl), sky_base(n), rough * rough);
    vec3 amb = (sky_base(n) * (1.0 - metallic) * base.rgb + spec_env * (f0 * ab.x + ab.y)) * fr.sky_zenith.w * ao;

    o_color = vec4(direct + amb + emit, mat.misc.y == 2.0 ? base.a : 1.0);
}
