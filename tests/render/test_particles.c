#include "sutekh/render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); failures++; } } while (0)

int main(void) {
    SuParticleEmitter emitter = {0};
    SuParticleState state, direct;
    uint32_t first, dropped, tick;
    emitter.capacity = 100; emitter.burst = 10; emitter.rate = 60; emitter.lifetime_ticks = 120;
    emitter.time_tick = 60; emitter.velocity.x = 3; emitter.gravity.y = -6;
    CHECK(su_particle_validate(&emitter) == SU_OK);
    CHECK(su_particle_count(&emitter, &first, &dropped) == 70 && first == 0 && dropped == 0);
    CHECK(su_particle_evaluate(&emitter, 0, &state) == SU_OK && state.alive == 1 && state.age_ticks == 60);
    CHECK(fabsf(state.position.x - 3.0f) < 1e-5f && fabsf(state.position.y + 3.05f) < 1e-5f);
    emitter.capacity = 12;
    CHECK(su_particle_count(&emitter, &first, &dropped) == 12 && first == 58 && dropped == 58);
    emitter.time_tick = 120;
    CHECK(su_particle_evaluate(&emitter, 0, &state) == SU_OK && !state.alive);
    emitter.capacity = 1000;
    CHECK(su_particle_count(&emitter, &first, &dropped) == 120 && first == 10 && dropped == 0);
    emitter.time_tick = 1; emitter.rate = 1; emitter.burst = 0;
    CHECK(su_particle_count(&emitter, NULL, NULL) == 0);
    emitter.time_tick = 60;
    CHECK(su_particle_count(&emitter, &first, NULL) == 1 && first == 0);
    CHECK(su_particle_evaluate(&emitter, 0, &state) == SU_OK && state.age_ticks == 0);
    emitter.burst = 10; emitter.rate = 0; emitter.time_tick = 60; emitter.turbulence = 2;
    emitter.attractor_count = 1; emitter.attractors[0].strength = 1; emitter.attractors[0].position.x = 2;
    emitter.drag = 0.5f; emitter.seed = 123;
    CHECK(su_particle_evaluate(&emitter, 5, &direct) == SU_OK);
    for (tick = 0; tick <= 60; tick++) {
        emitter.time_tick = tick;
        CHECK(su_particle_evaluate(&emitter, 5, &state) == SU_OK);
    }
    CHECK(memcmp(&state, &direct, sizeof state) == 0);
    emitter.transform_start = su_mat4f_identity(); emitter.transform_end = su_mat4f_identity();
    emitter.transform_end.m[12] = 10; emitter.transform_ticks = 60;
    emitter.velocity.x = 0; emitter.gravity.y = 0; emitter.turbulence = 0; emitter.attractor_count = 0;
    emitter.flags = SU_PARTICLE_LOCAL;
    CHECK(su_particle_evaluate(&emitter, 0, &state) == SU_OK && state.position.x == 10);
    emitter.flags = 0;
    CHECK(su_particle_evaluate(&emitter, 0, &state) == SU_OK && state.position.x == 0);
    emitter.time_tick = 65535; emitter.rate = 60000; emitter.burst = SU_PARTICLE_MAX;
    CHECK(su_particle_validate(&emitter) == SU_OK);
    CHECK(su_particle_count(&emitter, &first, &dropped) == 1000);
    CHECK(su_particle_evaluate(&emitter, first, &state) == SU_OK && state.alive);
    emitter.time_tick++;
    CHECK(su_particle_validate(&emitter) == SU_E_INVALID_ARGUMENT);
    emitter.time_tick = 0; emitter.size[0] = NAN;
    CHECK(su_particle_validate(&emitter) == SU_E_INVALID_ARGUMENT);
    emitter.size[0] = 0; emitter.capacity = SU_PARTICLE_MAX + 1u;
    CHECK(su_particle_validate(&emitter) == SU_E_INVALID_ARGUMENT);
    CHECK(su_particle_evaluate(NULL, 0, &state) == SU_E_INVALID_ARGUMENT);
    emitter.capacity = 1000; emitter.transform_start.m[3] = 1;
    CHECK(su_particle_validate(&emitter) == SU_E_INVALID_ARGUMENT);
    emitter.transform_start.m[3] = 0; emitter.attractor_count = SU_PARTICLE_ATTRACTOR_MAX + 1u;
    CHECK(su_particle_validate(&emitter) == SU_E_INVALID_ARGUMENT);
    printf("particle reference failures=%d\n", failures);
    return failures != 0;
}
