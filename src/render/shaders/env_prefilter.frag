// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// GPU environment prefilter: one face of one mip of the specular chain, the same estimator as su_env_prefilter (GGX
// importance sampling, 96 samples, N = V, source level picked from the sample's solid angle, cosine weights).
#version 450
layout(set = 0, binding = 0) uniform samplerCube u_src; // box-filtered source chain
layout(push_constant) uniform Pf {
    vec4 a; // face, mip size, roughness, solid angle of a base texel
    vec4 b; // last source level
} pc;
layout(location = 0) out vec4 o_color;

const float PI = 3.14159265358979;

vec3 texel_dir(uint face, vec2 st) {
    float sc = st.x;
    float tc = st.y;
    if (face == 0u) return vec3(1.0, -tc, -sc);
    if (face == 1u) return vec3(-1.0, -tc, sc);
    if (face == 2u) return vec3(sc, 1.0, tc);
    if (face == 3u) return vec3(sc, -1.0, -tc);
    if (face == 4u) return vec3(sc, -tc, 1.0);
    return vec3(-sc, -tc, -1.0);
}

void main() {
    uint face = uint(pc.a.x + 0.5);
    vec2 st = gl_FragCoord.xy / pc.a.y * 2.0 - 1.0;
    vec3 n = normalize(texel_dir(face, st));
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n));
    vec3 ty = cross(n, tx);
    float al = pc.a.z * pc.a.z;
    float a2 = al * al;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 96; i++) {
        float u1 = (float(i) + 0.5) / 96.0;
        float u2 = float(bitfieldReverse(uint(i))) * 2.3283064365386963e-10;
        float phi = 2.0 * PI * u2;
        float cos_t = sqrt((1.0 - u1) / (1.0 + (a2 - 1.0) * u1));
        float sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0));
        vec3 h = tx * (sin_t * cos(phi)) + ty * (sin_t * sin(phi)) + n * cos_t;
        vec3 l = h * (2.0 * dot(n, h)) - n;
        float nl = dot(n, l);
        if (nl > 0.0) {
            float dd = cos_t * cos_t * (a2 - 1.0) + 1.0;
            float pdf = a2 / (PI * dd * dd) * 0.25;
            float sa = 1.0 / (96.0 * pdf + 1e-4);
            float lod = max(0.5 * log2(sa / pc.a.w) + 1.0, 0.0);
            float li = min(floor(lod + 0.5), pc.b.x);
            acc += textureLod(u_src, l, li).rgb * nl;
            wsum += nl;
        }
    }
    o_color = vec4(acc / (wsum > 0.0 ? wsum : 1.0), 1.0);
}
