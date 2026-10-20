// SPDX-FileCopyrightText: 2026 admiraly
// SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_layers.glsl"

void main() {
    vec4 sample_color = layer_sample();
    float alpha = sample_color.a * v_color.a * layer_fade();
    o_color = vec4(sample_color.rgb * v_color.rgb * alpha, alpha);
}
