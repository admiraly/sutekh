// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
// Additive effects. v < 0 marks a billboard (v from -1 to -2 across it): a round soft spot. Otherwise a ribbon or
// beam: soft across (u), fading along (v, 0 at the head and 1 at the tail).
#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    float k;
    if (v_uv.y < 0.0) {
        vec2 q = vec2(v_uv.x * 2.0 - 1.0, (-v_uv.y - 1.0) * 2.0 - 1.0);
        float r = length(q);
        k = max(1.0 - r, 0.0);
        k = k * k * (0.4 + 0.6 * k);
    } else {
        float across = 1.0 - abs(v_uv.x * 2.0 - 1.0);
        k = across * across * (1.0 - clamp(v_uv.y, 0.0, 1.0));
    }
    o_color = vec4(v_color.rgb * v_color.a * k, 0.0);
}
