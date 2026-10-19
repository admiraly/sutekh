// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Textured premultiplied-alpha effects: the texture is premultiplied; blend ONE, ONE_MINUS_SRC_ALPHA.
//   result = t.rgb * c.rgb * c.a + dst * (1 - t.a * c.a)
#version 450
layout(set = 1, binding = 0) uniform sampler2D u_tex;
// The scene depth (reversed Z, infinite far: depth = near / view depth; 0 = background), read in the effects subpass.
layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput u_depth;
layout(push_constant) uniform FxPush {
    float soft;   // 1 / soft distance; 0 = hard intersections
    float near_z;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// Soft particles: fade as the sprite nears the surface behind it.
float soft_fade() {
    if (pc.soft <= 0.0) {
        return 1.0;
    }
    float ds = subpassLoad(u_depth).r;
    if (ds <= 0.0) {
        return 1.0;
    }
    float d = pc.near_z / ds - pc.near_z / gl_FragCoord.z;
    return clamp(d * pc.soft, 0.0, 1.0);
}

void main() {
    vec4 t = texture(u_tex, v_uv);
    float a = v_color.a * soft_fade();
    o_color = vec4(t.rgb * v_color.rgb * a, t.a * a);
}
