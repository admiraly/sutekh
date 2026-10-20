#include "render/su_render_lights.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
_Static_assert(sizeof(SuPointLightGpu) == 32u, "point light uniform layout");
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

int main(void) {
    SuRenderPointLight lights[SU_RENDER_MAX_POINT_LIGHTS];
    SuPointLightGpu gpu[SU_RENDER_MAX_POINT_LIGHTS];
    SuWorldPos camera = su_wpos(1000000000.25, -1000000000.5, 1000000000.75);
    float center[3] = {0, 0, 0};
    uint32_t mask[4];
    uint32_t index;
    memset(lights, 0, sizeof lights);
    CHECK(su_point_lights_prepare(NULL, 0, camera, gpu));
    su_point_lights_mask(gpu, 0, center, 1, mask);
    CHECK((mask[0] | mask[1] | mask[2] | mask[3]) == 0u);
    CHECK(!su_point_lights_prepare(NULL, 1, camera, gpu));
    CHECK(!su_point_lights_prepare(lights, SU_RENDER_MAX_POINT_LIGHTS + 1u, camera, gpu));
    for (index = 0; index < SU_RENDER_MAX_POINT_LIGHTS; index++) {
        lights[index].pos = su_wpos_add(camera, (SuVec3f){0.125f, 0.25f, 0.5f});
        lights[index].radius = 2;
        lights[index].color[0] = 0.5f;
        lights[index].intensity = 4;
    }
    CHECK(su_point_lights_prepare(lights, SU_RENDER_MAX_POINT_LIGHTS, camera, gpu));
    CHECK(gpu[127].position_radius[0] == 0.125f && gpu[127].position_radius[1] == 0.25f &&
          gpu[127].position_radius[2] == 0.5f && gpu[127].radiance[0] == 2.0f);
    su_point_lights_mask(gpu, SU_RENDER_MAX_POINT_LIGHTS, center, 1, mask);
    CHECK(mask[0] == UINT32_MAX && mask[1] == UINT32_MAX && mask[2] == UINT32_MAX && mask[3] == UINT32_MAX);
    center[0] = 10;
    su_point_lights_mask(gpu, SU_RENDER_MAX_POINT_LIGHTS, center, 1, mask);
    CHECK((mask[0] | mask[1] | mask[2] | mask[3]) == 0u);
    center[0] = 3;
    center[1] = 0.25f;
    center[2] = 0.5f;
    su_point_lights_mask(gpu, SU_RENDER_MAX_POINT_LIGHTS, center, 0.875f, mask);
    CHECK(mask[3] == UINT32_MAX);
    su_point_lights_mask(gpu, SU_RENDER_MAX_POINT_LIGHTS, center, 0.874f, mask);
    CHECK(mask[3] == 0u);
    lights[0].radius = 0;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].radius = -1;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].radius = NAN;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].radius = INFINITY;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].radius = 2;
    lights[0].intensity = -1;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].intensity = INFINITY;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].intensity = NAN;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].intensity = FLT_MAX;
    lights[0].color[2] = FLT_MAX;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].intensity = 1;
    lights[0].color[2] = -1;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].color[2] = NAN;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].color[2] = INFINITY;
    CHECK(!su_point_lights_prepare(lights, 1, camera, gpu));
    lights[0].color[2] = 0;
    lights[0].intensity = 0;
    CHECK(su_point_lights_prepare(lights, 1, camera, gpu));
    su_point_lights_mask(gpu, 1, center, FLT_MAX, mask);
    CHECK(mask[0] == 0u);
    lights[0].intensity = 1;
    lights[0].radius = FLT_MAX;
    CHECK(su_point_lights_prepare(lights, 1, camera, gpu));
    su_point_lights_mask(gpu, 1, center, FLT_MAX, mask);
    CHECK(mask[0] == 1u);
    printf("point light math: %u failures\n", failures);
    return failures != 0u;
}
