#version 450
layout(set = 0, binding = 0) uniform sampler2D u_scene;
layout(set = 0, binding = 1) uniform sampler2D u_field;
layout(set = 0, binding = 2) uniform sampler2D u_depth;
layout(set = 1, binding = 0) uniform samplerCube u_cube;
layout(push_constant) uniform Resolve { vec4 params; } pc;
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

vec3 refract_channel(vec2 origin, vec2 offset, float effect_depth) {
    vec2 size = vec2(textureSize(u_scene, 0));
    vec2 coordinate = clamp(origin + offset / size, 0.5 / size, 1.0 - 0.5 / size);
    if (texture(u_depth, coordinate).r > effect_depth) coordinate = origin;
    return texture(u_scene, coordinate).rgb;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 field = texelFetch(u_field, pixel, 0);
    vec4 scene = texelFetch(u_scene, pixel, 0);
    if (field.w <= 0.0) {
        o_color = scene;
        return;
    }
    vec2 uv = v_ndc * 0.5 + 0.5;
    vec3 color = refract_channel(uv, field.xy, field.w);
    if (field.z > 0.0) {
        color.r = refract_channel(uv, field.xy * (1.0 + field.z), field.w).r;
        color.b = refract_channel(uv, field.xy * (1.0 - field.z), field.w).b;
    }
    if (pc.params.x > 0.0) {
        color += textureLod(u_cube, normalize(vec3(field.xy, 16.0)), 0.0).rgb * pc.params.x;
    }
    o_color = vec4(color, scene.a);
}
