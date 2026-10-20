#version 450
#extension GL_GOOGLE_include_directive : require
#define SU_SHADOW_RECEIVER
#include "frame.glsl"

layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInput u_depth;
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform HazePush {
    vec4 row[3];
    vec4 color_density;
    vec4 params;
    vec4 response;
} pc;

float profile(vec3 point) {
    vec3 squared = point * point;
    float density = pc.params.x < 0.5 ? max(0.0, 1.0 - dot(point, point))
                                     : max(0.0, 1.0 - squared.x) * max(0.0, 1.0 - squared.y) * max(0.0, 1.0 - squared.z);
    return mix(1.0, density, pc.params.y);
}

void main() {
    vec4 near_point = fr.inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 direction = normalize(near_point.xyz / near_point.w);
    float depth = subpassLoad(u_depth).r;
    float end = 3.0e30;
    if (depth > 0.0) {
        vec4 surface = fr.inv_view_proj * vec4(v_ndc, depth, 1.0);
        end = length(surface.xyz / surface.w);
    }
    vec3 origin = vec3(pc.row[0].w, pc.row[1].w, pc.row[2].w);
    vec3 ray = vec3(dot(pc.row[0].xyz, direction), dot(pc.row[1].xyz, direction), dot(pc.row[2].xyz, direction));
    float start = 0.0;
    if (pc.params.x < 0.5) {
        float speed = dot(ray, ray);
        float middle = -dot(origin, ray) / speed;
        vec3 closest = origin + middle * ray;
        float radius = 1.0 - dot(closest, closest);
        if (radius <= 0.0) discard;
        float half_chord = sqrt(radius / speed);
        start = max(start, middle - half_chord);
        end = min(end, middle + half_chord);
    } else {
        for (int axis = 0; axis < 3; axis++) {
            if (abs(ray[axis]) < 1.0e-30) {
                if (abs(origin[axis]) > 1.0) discard;
            } else {
                vec2 hit = (vec2(-1.0, 1.0) - origin[axis]) / ray[axis];
                start = max(start, min(hit.x, hit.y));
                end = min(end, max(hit.x, hit.y));
            }
        }
    }
    if (end <= start) discard;
    float middle = 0.5 * (start + end);
    float half_chord = 0.5 * (end - start);
    vec3 center = origin + ray * middle;
    vec3 delta = ray * half_chord;
    float integral;
    if (pc.params.x < 0.5) {
        integral = 2.0 * mix(1.0, max(0.0, 1.0 - dot(center, center) - dot(delta, delta) / 3.0), pc.params.y);
    } else {
        integral = 0.3478548451 * (profile(center - delta * 0.8611363116) + profile(center + delta * 0.8611363116))
                 + 0.6521451549 * (profile(center - delta * 0.3399810436) + profile(center + delta * 0.3399810436));
    }
    float alpha = 1.0 - exp(-pc.color_density.w * half_chord * integral);
    if (pc.response.w > 0.5) {
        float closest_t = clamp(-dot(origin, ray) / dot(ray, ray), start, end);
        float distance = length(origin + ray * closest_t);
        float lobe = pc.response.z > 0.0 ? pow(clamp(1.0 - distance / (0.5 * pc.response.z), 0.0, 1.0),
                                               pc.response.x / 3.0) : 0.0;
        float chord = (end - start) * length(ray) * pc.color_density.w;
        alpha = clamp(pow(chord, pc.response.x) + pc.response.y * lobe, 0.0, 1.0);
    }
    float shadow = pc.params.w > 0.5 ? sun_shadow(direction * middle, vec3(0.0)) : 1.0;
    vec3 source = pc.color_density.rgb * (vec3(1.0) + pc.params.z * fr.sun_color.rgb * shadow);
    o_color = vec4(source * alpha, alpha);
}
