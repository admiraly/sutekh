#ifndef SU_RENDER_LIGHTS_H
#define SU_RENDER_LIGHTS_H

#include "sutekh/render.h"

typedef struct SuPointLightGpu {
    float position_radius[4];
    float radiance[4];
} SuPointLightGpu;

int su_point_lights_prepare(const SuRenderPointLight *lights, uint32_t count, SuWorldPos camera,
                            SuPointLightGpu out[SU_RENDER_MAX_POINT_LIGHTS]);
void su_point_lights_mask(const SuPointLightGpu *lights, uint32_t count, const float center[3], float radius,
                          uint32_t mask[4]);

#endif
