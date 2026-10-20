layout(set = 1, binding = 0) uniform sampler2D u_tex[3];
layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput u_depth;
layout(push_constant) uniform FxPush {
    vec4 uv[3];
    vec4 atlas;
    float soft;
    float near_z;
    float angular;
    uint layers;
#ifdef SU_DISTORTION
    vec4 distortion;
    vec4 ring;
#endif
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec3 v_pos;
layout(location = 0) out vec4 o_color;

float atlas_wrap(float coordinate, uint mode) {
    if (mode == 1u) {
        return fract(coordinate);
    }
    if (mode == 2u) {
        return 1.0 - abs(mod(coordinate, 2.0) - 1.0);
    }
    return clamp(coordinate, 0.0, 1.0);
}

vec4 sample_layer(sampler2D image, vec2 coordinate, vec2 gradient_x, vec2 gradient_y) {
    if (max(dot(gradient_x, gradient_x), dot(gradient_y, gradient_y)) < 1e-20) {
        return textureLod(image, coordinate, 0.0);
    }
    return textureGrad(image, coordinate, gradient_x, gradient_y);
}

vec4 layer_sample() {
    vec2 uv0 = v_uv * pc.uv[0].xy + pc.uv[0].zw;
    vec2 uv1 = v_uv * pc.uv[1].xy + pc.uv[1].zw;
    vec2 uv2 = v_uv * pc.uv[2].xy + pc.uv[2].zw;
    vec4 sample_color = sample_layer(u_tex[0], uv0, dFdx(uv0), dFdy(uv0));
    if ((pc.layers & 1u) != 0u) {
        sample_color *= sample_layer(u_tex[1], uv1, dFdx(uv1), dFdy(uv1));
    }
    if ((pc.layers & 2u) != 0u) {
        if ((pc.layers & 4u) != 0u) {
            vec2 gradient_x = dFdx(uv2) * pc.atlas.xy;
            vec2 gradient_y = dFdy(uv2) * pc.atlas.xy;
            uv2 = vec2(atlas_wrap(uv2.x, (pc.layers >> 3u) & 3u), atlas_wrap(uv2.y, (pc.layers >> 5u) & 3u));
            uv2 = uv2 * pc.atlas.xy + pc.atlas.zw;
            vec2 inset = min(0.5 / vec2(textureSize(u_tex[2], 0)), pc.atlas.xy * 0.5);
            uv2 = clamp(uv2, pc.atlas.zw + inset, pc.atlas.zw + pc.atlas.xy - inset);
            sample_color *= sample_layer(u_tex[2], uv2, gradient_x, gradient_y);
        } else {
            sample_color *= sample_layer(u_tex[2], uv2, dFdx(uv2), dFdy(uv2));
        }
    }
    return sample_color;
}

float layer_fade() {
    float fade = 1.0;
    if (pc.soft > 0.0) {
        float scene_depth = subpassLoad(u_depth).r;
        if (scene_depth > 0.0) {
            float distance = pc.near_z / scene_depth - pc.near_z / gl_FragCoord.z;
            fade *= clamp(distance * pc.soft, 0.0, 1.0);
        }
    }
    if (pc.angular > 0.0) {
        vec3 normal = cross(dFdx(v_pos), dFdy(v_pos));
        float denominator = sqrt(max(dot(normal, normal) * dot(v_pos, v_pos), 1e-30));
        fade *= mix(1.0, clamp(abs(dot(normal, v_pos)) / denominator, 0.0, 1.0), pc.angular);
    }
    return fade;
}
