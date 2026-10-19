// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Layered paint material: albedo, two-channel normal, roughness, material map, a four-channel paint mask selecting up
// to four layers (colour, roughness, metalness, tinted glow; lerp, multiply or tint, in a configurable order), glow, dirt,
// AO. Lighting is PBR-lite's (GGX, Smith, Schlick, Lambert, one sun); ambient is image based when an environment
// cubemap is bound (prefiltered mips, split-sum scale and bias), else the procedural sky exactly like pbr.frag.
// The blend is specified in include/sutekh/render.h ("layered paint material"). Output is linear HDR.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec4 v_uv;
layout(location = 4) in vec4 v_tint;
layout(location = 5) in float v_emissive;

layout(set = 1, binding = 0) uniform sampler2D t_albedo;     // sRGB
layout(set = 1, binding = 1) uniform sampler2D t_normal;     // linear, rg
layout(set = 1, binding = 2) uniform sampler2D t_rough;      // linear
layout(set = 1, binding = 3) uniform sampler2D t_material;   // linear
layout(set = 1, binding = 4) uniform sampler2D t_mask;       // linear, rgba = layers 0..3
layout(set = 1, binding = 5) uniform sampler2D t_glow;       // sRGB
layout(set = 1, binding = 6) uniform sampler2D t_dirt;       // linear
layout(set = 1, binding = 7) uniform sampler2D t_ao;         // linear

struct Layer {
    vec4 color_rough;  // rgb colour, roughness
    vec4 mop;          // metallic, opacity, flags (bit 0 multiply, bit 1 glow tint, bit 2 tint), glow strength
    vec4 glow;         // rgb glow colour
};

layout(set = 1, binding = 8) uniform Paint {
    vec4 a;            // specular, dirt amount, dirt darken, dirt roughen
    uvec4 b;           // layer count, order (byte i = i-th layer), channels rough|metal<<8|spec<<16|dirt<<24, ao channel
    Layer layer[4];
} pt;

layout(set = 2, binding = 0) uniform samplerCube t_env;

layout(push_constant) uniform Material {
    vec4 base_color;
    vec4 emissive;   // rgb: base glow colour (the layers may recolour it, so intensity is kept apart)
    vec4 mr;         // metallic factor, roughness factor, normal scale, occlusion strength
    vec4 misc;       // alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend), base glow intensity, has normal map
    uvec4 uvset;     // x: bit i = slot i reads UV1
} mat;

layout(location = 0) out vec4 o_color;

const float PI = 3.14159265;

vec2 uv_of(uint slot) {
    return ((mat.uvset.x >> slot) & 1u) != 0u ? v_uv.zw : v_uv.xy;
}

// channel c of a texel; c >= 4 means "map not read": 1
float pick(vec4 t, uint c) {
    return c == 0u ? t.r : c == 1u ? t.g : c == 2u ? t.b : c == 3u ? t.a : 1.0;
}

void main() {
    vec4 albedo = texture(t_albedo, uv_of(0u));
    vec4 base = mat.base_color * v_tint * albedo;
    if (mat.misc.y == 1.0 && base.a < mat.misc.x) {
        discard;
    }
    uint ch_rough = pt.b.z & 0xFFu;
    uint ch_metal = (pt.b.z >> 8) & 0xFFu;
    uint ch_spec = (pt.b.z >> 16) & 0xFFu;
    uint ch_dirt = (pt.b.z >> 24) & 0xFFu;
    uint ch_ao = pt.b.w & 0xFFu;

    vec3 a = base.rgb;
    float rough = mat.mr.y * pick(texture(t_rough, uv_of(2u)), ch_rough);
    vec4 mtex = texture(t_material, uv_of(3u));
    float metallic = mat.mr.x * pick(mtex, ch_metal);
    float spec = pt.a.x * pick(mtex, ch_spec);
    vec3 glow_c = mat.emissive.rgb;
    float glow_i = mat.misc.z;

    vec4 mask = texture(t_mask, uv_of(4u));
    for (uint i = 0u; i < pt.b.x; i++) {
        uint k = (pt.b.y >> (8u * i)) & 0xFFu;
        float w = clamp(mask[k], 0.0, 1.0) * clamp(pt.layer[k].mop.y, 0.0, 1.0);
        vec3 c = pt.layer[k].color_rough.rgb;
        float lr = pt.layer[k].color_rough.w;
        float lm = pt.layer[k].mop.x;
        uint fl = uint(pt.layer[k].mop.z + 0.5);
        if ((fl & 4u) != 0u) {
            a = mix(a, albedo.rgb * c, w);
            rough = mix(rough, lr, w);
            metallic = mix(metallic, lm, w);
        } else if ((fl & 1u) != 0u) {
            a = mix(a, a * c, w);
            rough = mix(rough, rough * lr, w);
            metallic = mix(metallic, metallic * lm, w);
        } else {
            a = mix(a, c, w);
            rough = mix(rough, lr, w);
            metallic = mix(metallic, lm, w);
        }
        if ((fl & 2u) != 0u) {
            glow_c = mix(glow_c, pt.layer[k].glow.rgb, w);
            glow_i = mix(glow_i, pt.layer[k].mop.w, w);
        }
    }

    float dirt = clamp(pt.a.y * pick(texture(t_dirt, uv_of(6u)), ch_dirt), 0.0, 1.0);
    a *= 1.0 - dirt * pt.a.z;
    rough = mix(rough, 1.0, dirt * pt.a.w);
    rough = clamp(rough, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    vec3 emit = texture(t_glow, uv_of(5u)).rgb * glow_c * glow_i * v_emissive;

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
            tn.xy = texture(t_normal, uv_of(1u)).xy * 2.0 - 1.0;
            tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
            tn.xy *= mat.mr.z;
            n = normalize(t * tn.x + b * tn.y + n * tn.z);
        }
    }
    float ao = mix(1.0, pick(texture(t_ao, uv_of(7u)), ch_ao), mat.mr.w);

    vec3 v = normalize(-v_pos);
    vec3 l = -fr.sun_dir.xyz;
    vec3 h = normalize(v + l);
    float nl = max(dot(n, l), 0.0);
    float nv = max(dot(n, v), 1e-4);
    float nh = max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    float al = rough * rough;
    float a2 = al * al;
    vec3 f0 = mix(vec3(0.08 * spec), a, metallic);
    vec3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float d = a2 / (PI * dd * dd);
    float vis = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1e-6);
    vec3 diffuse = (1.0 - f) * (1.0 - metallic) * a / PI;
    vec3 direct = (diffuse + d * vis * f) * fr.sun_color.rgb * nl;

    // ambient: split-sum scale and bias (Karis, analytic fit)
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 rr = rough * c0 + c1;
    float a004 = min(rr.x * rr.x, exp2(-9.28 * nv)) * rr.x + rr.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + rr.zw;
    vec3 refl = reflect(-v, n);
    vec3 diff_env;
    vec3 spec_env;
    if (fr.params.z > 0.0) {
        diff_env = textureLod(t_env, n, fr.params.w).rgb * fr.params.z;
        spec_env = textureLod(t_env, refl, rough * fr.params.w).rgb * fr.params.z;
    } else {
        diff_env = sky_base(n);
        spec_env = mix(sky_radiance(refl), sky_base(n), rough * rough);
    }
    vec3 amb = (diff_env * (1.0 - metallic) * a + spec_env * (f0 * ab.x + ab.y)) * fr.sky_zenith.w * ao;

    o_color = vec4(direct + amb + emit, mat.misc.y == 2.0 ? base.a : 1.0);
}
