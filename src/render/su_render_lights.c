#include "render/su_render_lights.h"

#include <math.h>
#include <string.h>

int su_point_lights_prepare(const SuRenderPointLight *lights, uint32_t count, SuWorldPos camera,
                            SuPointLightGpu out[SU_RENDER_MAX_POINT_LIGHTS]) {
    uint32_t index;
    if (count > SU_RENDER_MAX_POINT_LIGHTS || (count != 0u && lights == NULL)) return 0;
    memset(out, 0, sizeof(SuPointLightGpu) * SU_RENDER_MAX_POINT_LIGHTS);
    for (index = 0; index < count; index++) {
        const SuRenderPointLight *light = &lights[index];
        SuVec3f relative;
        uint32_t channel;
        if (!isfinite(light->radius) || light->radius <= 0.0f || !isfinite(light->intensity) ||
            light->intensity < 0.0f) return 0;
        relative = su_wpos_delta(light->pos, camera);
        out[index].position_radius[0] = relative.x;
        out[index].position_radius[1] = relative.y;
        out[index].position_radius[2] = relative.z;
        out[index].position_radius[3] = light->radius;
        for (channel = 0; channel < 3u; channel++) {
            float radiance = light->color[channel] * light->intensity;
            if (!isfinite(light->color[channel]) || light->color[channel] < 0.0f || !isfinite(radiance)) return 0;
            out[index].radiance[channel] = radiance;
        }
    }
    return 1;
}

void su_point_lights_mask(const SuPointLightGpu *lights, uint32_t count, const float center[3], float radius,
                          uint32_t mask[4]) {
    uint32_t index;
    memset(mask, 0, 4u * sizeof *mask);
    for (index = 0; index < count; index++) {
        const SuPointLightGpu *light = &lights[index];
        double reach = (double)radius + light->position_radius[3];
        double delta_x = (double)center[0] - light->position_radius[0];
        double delta_y = (double)center[1] - light->position_radius[1];
        double delta_z = (double)center[2] - light->position_radius[2];
        if ((light->radiance[0] > 0.0f || light->radiance[1] > 0.0f || light->radiance[2] > 0.0f) &&
            delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <= reach * reach) {
            mask[index / 32u] |= UINT32_C(1) << (index % 32u);
        }
    }
}
