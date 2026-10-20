vec3 point_lighting(uvec4 mask, vec3 position, vec3 normal, vec3 view_dir, vec3 albedo,
                    vec3 f0, float diffuse_weight, float roughness) {
    vec3 result = vec3(0.0);
    float nv = max(dot(normal, view_dir), 1e-4);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    for (uint word = 0u; word < 4u; word++) {
        uint bits = mask[word];
        while (bits != 0u) {
            uint index = word * 32u + uint(findLSB(bits));
            bits &= bits - 1u;
            PointLight light = fr.point_lights[index];
            vec3 delta = light.position_radius.xyz - position;
            float distance2 = dot(delta, delta);
            float distance_to_light = sqrt(distance2);
            if (distance_to_light >= light.position_radius.w || distance2 <= 1e-12) continue;
            vec3 direction = delta / distance_to_light;
            float nl = max(dot(normal, direction), 0.0);
            if (nl <= 0.0) continue;
            vec3 half_sum = view_dir + direction;
            vec3 half_dir = half_sum * inversesqrt(max(dot(half_sum, half_sum), 1e-12));
            float nh = max(dot(normal, half_dir), 0.0);
            float vh = max(dot(view_dir, half_dir), 0.0);
            vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
            float denominator = nh * nh * (alpha2 - 1.0) + 1.0;
            float distribution = alpha2 / (PI * denominator * denominator);
            float visibility = 0.5 / max(nl * sqrt(nv * nv * (1.0 - alpha2) + alpha2) +
                                         nv * sqrt(nl * nl * (1.0 - alpha2) + alpha2), 1e-6);
            vec3 diffuse = (1.0 - fresnel) * diffuse_weight * albedo / PI;
            float window = distance_to_light / light.position_radius.w;
            window *= window;
            window = max(1.0 - window * window, 0.0);
            float attenuation = window * window / max(distance2, 1.0);
            result += (diffuse + distribution * visibility * fresnel) * light.radiance.rgb * (nl * attenuation);
        }
    }
    return result;
}
