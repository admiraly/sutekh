#include "sutekh/render.h"

#include <math.h>
#include <string.h>

static float random_float(uint32_t seed, uint32_t id, uint32_t counter) {
    uint32_t value = seed ^ (id * 0x9e3779b9u) ^ (counter * 0x85ebca6bu);
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)(value >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

static int vector_valid(SuVec3f value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
           fabsf(value.x) <= 10000.0f && fabsf(value.y) <= 10000.0f && fabsf(value.z) <= 10000.0f;
}

uint32_t su_particle_validate(const SuParticleEmitter *emitter) {
    uint32_t component, key;
    int start_zero = 1, end_zero = 1;
    if (emitter == NULL || emitter->capacity > SU_PARTICLE_MAX || emitter->time_tick > 65535u ||
        emitter->lifetime_ticks == 0u || emitter->lifetime_ticks > 4096u || emitter->rate > 60000u ||
        emitter->burst > SU_PARTICLE_MAX || emitter->shape > SU_PARTICLE_CONE || (emitter->flags & ~3u) != 0u ||
        emitter->transform_ticks > 65535u || !vector_valid(emitter->extent) || !vector_valid(emitter->velocity) ||
        !vector_valid(emitter->velocity_random) || !vector_valid(emitter->gravity) ||
        !vector_valid(emitter->axis) || !isfinite(emitter->drag) || emitter->drag < 0.0f || emitter->drag > 60.0f ||
        !isfinite(emitter->turbulence) || fabsf(emitter->turbulence) > 10000.0f ||
        emitter->attractor_count > SU_PARTICLE_ATTRACTOR_MAX ||
        !isfinite(emitter->aspect) || emitter->aspect < 0.0f || emitter->aspect > 10000.0f ||
        emitter->atlas_cols > 1024u || emitter->atlas_rows > 1024u ||
        emitter->material.blend > SU_FX_BLEND_PREMUL || emitter->material.wrap_u > SU_FX_WRAP_MIRROR ||
        emitter->material.wrap_v > SU_FX_WRAP_MIRROR || (emitter->material.flags & ~SU_FX_BATCH_SORT) != 0u ||
        !isfinite(emitter->material.soft_distance) || emitter->material.soft_distance < 0.0f) return SU_E_INVALID_ARGUMENT;
    for (key = 0; key < emitter->attractor_count; key++) {
        const SuParticleAttractor *attractor = &emitter->attractors[key];
        if (!vector_valid(attractor->position) || !isfinite(attractor->strength) || fabsf(attractor->strength) > 10000.0f ||
            !isfinite(attractor->radius) || attractor->radius < 0.0f || attractor->radius > 10000.0f)
            return SU_E_INVALID_ARGUMENT;
    }
    for (component = 0; component < 16u; component++) {
        if (!isfinite(emitter->transform_start.m[component]) || !isfinite(emitter->transform_end.m[component]) ||
            fabsf(emitter->transform_start.m[component]) > 10000.0f ||
            fabsf(emitter->transform_end.m[component]) > 10000.0f) return SU_E_INVALID_ARGUMENT;
        if (emitter->transform_start.m[component] != 0.0f) start_zero = 0;
        if (emitter->transform_end.m[component] != 0.0f) end_zero = 0;
    }
    if ((!start_zero && (emitter->transform_start.m[3] != 0.0f || emitter->transform_start.m[7] != 0.0f ||
                        emitter->transform_start.m[11] != 0.0f || emitter->transform_start.m[15] != 1.0f)) ||
        (!end_zero && (emitter->transform_end.m[3] != 0.0f || emitter->transform_end.m[7] != 0.0f ||
                      emitter->transform_end.m[11] != 0.0f || emitter->transform_end.m[15] != 1.0f))) return SU_E_INVALID_ARGUMENT;
    for (key = 0; key < 4u; key++) {
        if (!isfinite(emitter->size[key]) || emitter->size[key] < 0.0f || emitter->size[key] > 10000.0f)
            return SU_E_INVALID_ARGUMENT;
        for (component = 0; component < 4u; component++)
            if (!isfinite(emitter->colour[key][component]) || emitter->colour[key][component] < 0.0f ||
                emitter->colour[key][component] > 10000.0f) return SU_E_INVALID_ARGUMENT;
    }
    return SU_OK;
}

static uint32_t total_at(const SuParticleEmitter *emitter, uint32_t tick) {
    return emitter->burst + emitter->rate * tick / 60u;
}

uint32_t su_particle_count(const SuParticleEmitter *emitter, uint32_t *first_id, uint32_t *dropped) {
    uint32_t total, expired, active, count;
    if (first_id != NULL) *first_id = 0;
    if (dropped != NULL) *dropped = 0;
    if (su_particle_validate(emitter) != SU_OK) return 0;
    total = total_at(emitter, emitter->time_tick);
    expired = emitter->time_tick >= emitter->lifetime_ticks ?
              total_at(emitter, emitter->time_tick - emitter->lifetime_ticks) : 0u;
    active = total - expired;
    count = active < emitter->capacity ? active : emitter->capacity;
    if (first_id != NULL) *first_id = total - count;
    if (dropped != NULL) *dropped = active - count;
    return count;
}

static SuMat4f transform_at(const SuParticleEmitter *emitter, uint32_t tick) {
    SuMat4f start = emitter->transform_start, end = emitter->transform_end;
    uint32_t component;
    float amount = emitter->transform_ticks != 0u ? fminf((float)tick / (float)emitter->transform_ticks, 1.0f) : 0.0f;
    int start_zero = 1, end_zero = 1;
    for (component = 0; component < 16u; component++) {
        if (start.m[component] != 0.0f) start_zero = 0;
        if (end.m[component] != 0.0f) end_zero = 0;
    }
    if (start_zero) start = su_mat4f_identity();
    if (end_zero) end = start;
    for (component = 0; component < 16u; component++) start.m[component] += (end.m[component] - start.m[component]) * amount;
    return start;
}

static SuVec3f transform_vector(SuMat4f matrix, SuVec3f vector, float point) {
    SuVec3f result = {matrix.m[0] * vector.x + matrix.m[4] * vector.y + matrix.m[8] * vector.z + matrix.m[12] * point,
                      matrix.m[1] * vector.x + matrix.m[5] * vector.y + matrix.m[9] * vector.z + matrix.m[13] * point,
                      matrix.m[2] * vector.x + matrix.m[6] * vector.y + matrix.m[10] * vector.z + matrix.m[14] * point};
    return result;
}

uint32_t su_particle_evaluate(const SuParticleEmitter *emitter, uint32_t id, SuParticleState *out) {
    uint32_t birth, step, component, key;
    float fraction, curve;
    SuVec3f position, velocity;
    SuMat4f matrix;
    if (out == NULL || su_particle_validate(emitter) != SU_OK) return SU_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out); out->id = id;
    if (id >= total_at(emitter, emitter->time_tick)) return SU_OK;
    birth = id < emitter->burst ? 0u : ((id - emitter->burst + 1u) * 60u + emitter->rate - 1u) / emitter->rate;
    out->age_ticks = emitter->time_tick - birth;
    if (out->age_ticks >= emitter->lifetime_ticks) return SU_OK;
    position.x = random_float(emitter->seed, id, 0);
    position.y = random_float(emitter->seed, id, 1);
    position.z = random_float(emitter->seed, id, 2);
    if (emitter->shape == SU_PARTICLE_POINT) memset(&position, 0, sizeof position);
    if (emitter->shape == SU_PARTICLE_SPHERE) {
        float radius = cbrtf((random_float(emitter->seed, id, 3) + 1.0f) * 0.5f);
        float angle = position.x * 3.14159265358979323846f;
        float ring = sqrtf(fmaxf(1.0f - position.z * position.z, 0.0f));
        position.x = cosf(angle) * ring * radius;
        position.y = sinf(angle) * ring * radius;
        position.z *= radius;
    }
    if (emitter->shape == SU_PARTICLE_CONE) {
        float height = cbrtf((position.z + 1.0f) * 0.5f);
        float angle = position.x * 3.14159265358979323846f;
        float radius = sqrtf((position.y + 1.0f) * 0.5f) * height;
        position.x = cosf(angle) * radius; position.y = sinf(angle) * radius; position.z = height;
    }
    position.x *= emitter->extent.x; position.y *= emitter->extent.y; position.z *= emitter->extent.z;
    velocity.x = emitter->velocity.x + emitter->velocity_random.x * random_float(emitter->seed, id, 4);
    velocity.y = emitter->velocity.y + emitter->velocity_random.y * random_float(emitter->seed, id, 5);
    velocity.z = emitter->velocity.z + emitter->velocity_random.z * random_float(emitter->seed, id, 6);
    matrix = transform_at(emitter, (emitter->flags & SU_PARTICLE_LOCAL) != 0u ? emitter->time_tick : birth);
    if ((emitter->flags & SU_PARTICLE_LOCAL) == 0u) {
        position = transform_vector(matrix, position, 1.0f);
        velocity = transform_vector(matrix, velocity, 0.0f);
    }
    for (step = 0; step < out->age_ticks; step++) {
        SuVec3f acceleration = emitter->gravity;
        uint32_t attractor_index;
        float damping = 1.0f / (1.0f + emitter->drag / 60.0f);
        for (attractor_index = 0; attractor_index < emitter->attractor_count; attractor_index++) {
            const SuParticleAttractor *attractor = &emitter->attractors[attractor_index];
            SuVec3f delta = {attractor->position.x - position.x, attractor->position.y - position.y,
                             attractor->position.z - position.z};
            float distance = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            float force = attractor->strength / fmaxf(distance, fmaxf(attractor->radius, 0.001f));
            acceleration.x += delta.x * force; acceleration.y += delta.y * force; acceleration.z += delta.z * force;
        }
        velocity.x = (velocity.x + (acceleration.x + emitter->turbulence *
                      random_float(emitter->seed, id, 7u + step * 3u)) / 60.0f) * damping;
        velocity.y = (velocity.y + (acceleration.y + emitter->turbulence *
                      random_float(emitter->seed, id, 8u + step * 3u)) / 60.0f) * damping;
        velocity.z = (velocity.z + (acceleration.z + emitter->turbulence *
                      random_float(emitter->seed, id, 9u + step * 3u)) / 60.0f) * damping;
        position.x += velocity.x / 60.0f; position.y += velocity.y / 60.0f; position.z += velocity.z / 60.0f;
    }
    if ((emitter->flags & SU_PARTICLE_LOCAL) != 0u) {
        position = transform_vector(matrix, position, 1.0f);
        velocity = transform_vector(matrix, velocity, 0.0f);
    }
    out->position = position; out->velocity = velocity; out->alive = 1;
    curve = (float)out->age_ticks / (float)emitter->lifetime_ticks * 3.0f;
    key = (uint32_t)curve; if (key > 2u) key = 2u;
    fraction = curve - (float)key;
    out->size = emitter->size[key] + (emitter->size[key + 1u] - emitter->size[key]) * fraction;
    for (component = 0; component < 4u; component++) out->colour[component] = emitter->colour[key][component] +
        (emitter->colour[key + 1u][component] - emitter->colour[key][component]) * fraction;
    return SU_OK;
}
