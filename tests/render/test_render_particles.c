#include "sutekh/render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 128u
#define COUNT 128u
static int failures;
static SuRenderer *renderer;
static SuRenderFrameDesc frame;
static SuParticleEmitter emitter;
static SuRenderHandle white, atlas;
static uint8_t pixels[SIZE * SIZE * 4u], reference_pixels[SIZE * SIZE * 4u];
static SuParticleState states[COUNT];
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); failures++; } } while (0)

static void reset(void) {
    uint32_t key;
    memset(&frame, 0, sizeof frame); frame.struct_size = sizeof frame;
    frame.camera.rot.w = 1; frame.camera.fov_y = 1.0471975512f; frame.camera.near_z = 0.05f;
    frame.light.no_sky = 1; frame.light.tonemap = SU_TONEMAP_CLAMP;
    memset(&emitter, 0, sizeof emitter);
    emitter.capacity = COUNT; emitter.burst = 23; emitter.rate = 60; emitter.lifetime_ticks = 120;
    emitter.time_tick = 35; emitter.seed = 7343;
    emitter.transform_start = su_mat4f_identity(); emitter.transform_start.m[14] = -4;
    emitter.velocity_random.x = 0.4f; emitter.velocity_random.y = 0.4f; emitter.velocity_random.z = 0.4f;
    emitter.extent.x = 0.5f; emitter.extent.y = 0.5f; emitter.extent.z = 0.5f;
    emitter.material.texture = white;
    for (key = 0; key < 4u; key++) {
        emitter.size[key] = 0.12f - (float)key * 0.025f;
        emitter.colour[key][0] = 0.1f + (float)key * 0.1f;
        emitter.colour[key][1] = 0.3f - (float)key * 0.06f;
        emitter.colour[key][2] = 0.1f; emitter.colour[key][3] = 0.25f;
    }
    frame.particle_emitters = &emitter; frame.particle_emitter_count = 1;
}

static void draw(void) {
    CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
}

static void compare_reference(void) {
    SuFxVertex vertices[COUNT * 6u];
    SuFxBuffer buffer = {0};
    SuFxBatch batch = emitter.material;
    uint32_t count = 0, index, component, bad = 0;
    SuMat4f view = su_render_camera_view(&frame.camera);
    float previous_distance = INFINITY;
    uint32_t previous_id = 0;
    draw();
    memcpy(reference_pixels, pixels, sizeof pixels);
    CHECK(su_render_particles_readback(renderer, 0, states, COUNT, &count) == SU_OK);
    CHECK(count == su_particle_count(&emitter, NULL, NULL));
    buffer.v = vertices; buffer.cap = COUNT * 6u;
    for (index = 0; index < count; index++) {
        SuParticleState expected;
        float rect[4], distance, aspect = emitter.aspect != 0.0f ? emitter.aspect : 1.0f;
        uint32_t cell;
        CHECK(su_particle_evaluate(&emitter, states[index].id, &expected) == SU_OK);
        {
            SuVec3f offset = su_wpos_delta(emitter.origin, frame.camera.pos);
            expected.position.x += offset.x; expected.position.y += offset.y; expected.position.z += offset.z;
        }
        CHECK(states[index].alive && states[index].age_ticks == expected.age_ticks);
        CHECK(fabsf(states[index].position.x - expected.position.x) < 0.002f);
        CHECK(fabsf(states[index].position.y - expected.position.y) < 0.002f);
        CHECK(fabsf(states[index].position.z - expected.position.z) < 0.002f);
        CHECK(fabsf(states[index].velocity.x - expected.velocity.x) < 0.002f);
        CHECK(fabsf(states[index].velocity.y - expected.velocity.y) < 0.002f);
        CHECK(fabsf(states[index].velocity.z - expected.velocity.z) < 0.002f);
        CHECK(fabsf(states[index].size - expected.size) < 1e-5f);
        for (component = 0; component < 4u; component++) CHECK(fabsf(states[index].colour[component] - expected.colour[component]) < 1e-5f);
        distance = expected.position.x * expected.position.x + expected.position.y * expected.position.y + expected.position.z * expected.position.z;
        if ((emitter.material.flags & SU_FX_BATCH_SORT) != 0u) {
            CHECK(distance <= previous_distance + 0.001f);
            if (distance == previous_distance) CHECK(expected.id > previous_id);
            previous_distance = distance; previous_id = expected.id;
        }
        cell = emitter.atlas_ticks != 0u ? expected.age_ticks / emitter.atlas_ticks : 0u;
        su_fx_atlas_rect(emitter.atlas_cols, emitter.atlas_rows, cell, rect);
        if ((emitter.flags & SU_PARTICLE_AXIS) != 0u)
            su_fx_sprite_axis(&buffer, expected.position, emitter.axis, expected.size * aspect, expected.size, expected.colour, rect);
        else su_fx_sprite(&buffer, expected.position, expected.size, expected.size * aspect, 0, expected.colour, rect, view);
    }
    batch.first_vertex = 0; batch.vertex_count = buffer.count; batch.flags = 0;
    frame.particle_emitter_count = 0;
    frame.fx_tex = vertices; frame.fx_tex_vertex_count = buffer.count; frame.fx_batches = &batch; frame.fx_batch_count = 1;
    draw();
    for (index = 0; index < sizeof pixels; index++) if (abs((int)pixels[index] - (int)reference_pixels[index]) > 3) bad++;
    if (bad > 32u) fprintf(stderr, "pixel differences=%u shape=%u flags=%u blend=%u\n", bad, emitter.shape, emitter.flags, emitter.material.blend);
    CHECK(bad <= 32u);
    frame.particle_emitter_count = 1; frame.fx_tex = NULL; frame.fx_tex_vertex_count = 0;
    frame.fx_batches = NULL; frame.fx_batch_count = 0;
}

static void pixels_test(void) {
    uint32_t shape, mode, tick;
    uint8_t standalone[sizeof pixels];
    SuRenderStats stats;
    SuRenderInstance wall = {0};
    SuParticleEmitter pair[2];
    for (shape = 0; shape <= SU_PARTICLE_CONE; shape++) for (mode = 0; mode < 4u; mode++) {
        reset(); emitter.shape = shape;
        emitter.drag = 0.7f; emitter.turbulence = 0.3f; emitter.gravity.y = -0.2f;
        emitter.attractor_count = 2;
        emitter.attractors[0].position.z = -3; emitter.attractors[0].strength = 0.2f; emitter.attractors[0].radius = 0.5f;
        emitter.attractors[1].position.x = 1; emitter.attractors[1].strength = -0.1f; emitter.attractors[1].radius = 0.2f;
        emitter.material.blend = mode & 1u; emitter.material.flags = mode >= 2u ? SU_FX_BATCH_SORT : 0;
        emitter.flags = mode >= 2u ? SU_PARTICLE_LOCAL : 0;
        emitter.transform_end = emitter.transform_start; emitter.transform_end.m[12] = 0.3f; emitter.transform_ticks = 60;
        if (mode == 2u) {
            emitter.transform_end.m[0] = emitter.transform_end.m[5] = 0;
            emitter.transform_end.m[1] = 1; emitter.transform_end.m[4] = -1;
        }
        compare_reference();
    }
    reset(); emitter.flags = SU_PARTICLE_AXIS; emitter.axis.y = 1; emitter.aspect = 2;
    emitter.material.texture = atlas; emitter.atlas_cols = 2; emitter.atlas_rows = 2; emitter.atlas_ticks = 10;
    compare_reference();
    emitter.material.layers.textures[1] = white; emitter.material.layers.textures[2] = white;
    emitter.material.layers.uv[0].speed[0] = 0.1f; frame.time_seconds = 0.3f;
    compare_reference();
    reset(); emitter.burst = 3; emitter.rate = 0; emitter.lifetime_ticks = 4096; emitter.time_tick = 4095;
    emitter.gravity.y = -0.0001f; emitter.drag = 2; emitter.turbulence = 0.0001f;
    compare_reference();
    reset(); emitter.origin.x = frame.camera.pos.x = (int64_t)1000000 * INT64_C(4294967296);
    emitter.origin.y = INT64_C(4294967296); frame.camera.pos.y = INT64_C(2147483648);
    emitter.shape = SU_PARTICLE_SPHERE;
    compare_reference();
    reset(); emitter.burst = 9; emitter.rate = 0; emitter.time_tick = 0;
    emitter.velocity_random.x = emitter.velocity_random.y = emitter.velocity_random.z = 0;
    emitter.material.flags = SU_FX_BATCH_SORT;
    compare_reference();
    reset(); emitter.burst = 1; emitter.rate = 0; emitter.time_tick = 0;
    for (mode = 0; mode < 4u; mode++) {
        emitter.size[mode] = 0.4f;
        emitter.colour[mode][0] = 0.5f; emitter.colour[mode][1] = emitter.colour[mode][2] = 0;
        emitter.colour[mode][3] = 1;
    }
    emitter.transform_start.m[14] = -3.5f;
    wall.pos[2] = -14; wall.scale = 20; wall.rot[3] = 1; wall.color[3] = 1; wall.mesh = SU_MESH_BOX;
    frame.debug_instances = &wall; frame.debug_instance_count = 1;
    draw(); CHECK(abs((int)pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u] - 188) <= 2);
    emitter.material.soft_distance = 2;
    draw(); CHECK(abs((int)pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u] - 99) <= 2);
    compare_reference();
    emitter.transform_start.m[14] = -4.5f;
    draw(); CHECK(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u] == 0);
    frame.debug_instances = NULL; frame.debug_instance_count = 0;
    emitter.transform_start.m[14] = -3.5f;
    pair[0] = emitter; pair[1] = emitter;
    pair[0].material.soft_distance = pair[1].material.soft_distance = 0;
    pair[1].material.blend = SU_FX_BLEND_PREMUL;
    for (mode = 0; mode < 4u; mode++) {
        pair[1].colour[mode][0] = 0; pair[1].colour[mode][1] = 0.5f;
        pair[1].colour[mode][3] = 0.5f;
    }
    frame.particle_emitters = pair; frame.particle_emitter_count = 2;
    draw(); CHECK(su_render_stats(renderer, &stats) == SU_OK && stats.particle_draws == 2);
    CHECK(abs((int)pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u] - 137) <= 2);
    CHECK(abs((int)pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u + 1u] - 137) <= 2);
    pair[0].capacity = pair[1].capacity = SU_PARTICLE_MAX;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    reset(); emitter.shape = SU_PARTICLE_BOX; emitter.turbulence = 0.7f; emitter.material.flags = SU_FX_BATCH_SORT;
    emitter.time_tick = 95; draw(); memcpy(standalone, pixels, sizeof pixels);
    for (tick = 0; tick < 95u; tick += 7u) { emitter.time_tick = tick; draw(); }
    emitter.time_tick = 95; draw(); CHECK(memcmp(standalone, pixels, sizeof pixels) == 0);
    emitter.time_tick = 0; draw(); emitter.time_tick = 95; draw(); CHECK(memcmp(standalone, pixels, sizeof pixels) == 0);
    emitter.capacity = 3; draw(); CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.particles_alive == 3 && stats.particles_dropped == 115 && stats.particle_draws == 1);
    emitter.time_tick = 300; emitter.rate = 0; draw();
    CHECK(su_render_stats(renderer, &stats) == SU_OK && stats.particles_alive == 0 && stats.particle_draws == 0);
    frame.particle_emitter_count = 0; draw(); memcpy(standalone, pixels, sizeof pixels);
    frame.particle_emitter_count = 1; emitter.capacity = 0; draw(); CHECK(memcmp(standalone, pixels, sizeof pixels) == 0);
    emitter.capacity = SU_PARTICLE_MAX + 1u; CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    emitter.capacity = 3; emitter.drag = NAN; CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    emitter.drag = 0; frame.particle_emitter_count = SU_PARTICLE_EMITTER_MAX + 1u;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    frame.particle_emitter_count = 1; frame.particle_emitters = NULL;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
}

static int compare_double(const void *left, const void *right) {
    double first = *(const double *)left, second = *(const double *)right;
    return first < second ? -1 : (first > second ? 1 : 0);
}

static void large_sort_test(void) {
    static const uint32_t counts[] = {63, 64, 65, 127, 128, 129, 1025, SU_PARTICLE_MAX};
    SuParticleState *output = (SuParticleState *)malloc(sizeof *output * SU_PARTICLE_MAX);
    uint8_t *seen = (uint8_t *)malloc(SU_PARTICLE_MAX);
    uint32_t test, key;
    CHECK(output != NULL && seen != NULL);
    if (output == NULL || seen == NULL) { free(output); free(seen); return; }
    for (test = 0; test < sizeof counts / sizeof counts[0]; test++) {
        uint32_t count = 0, index;
        float previous = INFINITY;
        reset(); emitter.capacity = emitter.burst = counts[test]; emitter.rate = 0; emitter.time_tick = 0;
        emitter.shape = SU_PARTICLE_BOX; emitter.material.flags = SU_FX_BATCH_SORT;
        for (key = 0; key < 4u; key++) emitter.size[key] = 0.0002f;
        draw();
        CHECK(su_render_particles_readback(renderer, 0, output, SU_PARTICLE_MAX, &count) == SU_OK);
        CHECK(count == counts[test]);
        memset(seen, 0, SU_PARTICLE_MAX);
        for (index = 0; index < count; index++) {
            SuVec3f position = output[index].position;
            float distance = position.x * position.x + position.y * position.y + position.z * position.z;
            CHECK(distance <= previous + 0.0001f); previous = distance;
            CHECK(output[index].id < count);
            if (output[index].id < count) { CHECK(seen[output[index].id] == 0); seen[output[index].id] = 1; }
        }
    }
    free(output); free(seen);
}

static void benchmark(void) {
    uint32_t count, sample, sorted;
    for (sorted = 0; sorted < 2u; sorted++) for (count = 0; count <= 100000u; count += count == 0u ? 10000u : 90000u) {
        double gpu[60], cpu[60];
        SuRenderStats stats = {0};
        reset(); emitter.capacity = count; emitter.burst = count; emitter.rate = 0;
        emitter.shape = SU_PARTICLE_BOX; emitter.extent.x = 4; emitter.extent.y = 2; emitter.extent.z = 0.5f;
        emitter.time_tick = 60; emitter.lifetime_ticks = 600;
        emitter.drag = 0.2f; emitter.gravity.y = -0.1f; emitter.turbulence = 0.1f;
        emitter.material.flags = sorted != 0u ? SU_FX_BATCH_SORT : 0u;
        emitter.size[0] = emitter.size[1] = emitter.size[2] = emitter.size[3] = 0.006f;
        for (sample = 0; sample < 80u; sample++) {
            CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
            CHECK(su_render_stats(renderer, &stats) == SU_OK);
            if (sample >= 20u) { gpu[sample - 20u] = stats.gpu_ms; cpu[sample - 20u] = stats.cpu_ms; }
        }
        qsort(gpu, 60, sizeof gpu[0], compare_double); qsort(cpu, 60, sizeof cpu[0], compare_double);
        printf("particles=%u sorted=%u age_ticks=60 gpu_median_ms=%.6f gpu_p95_ms=%.6f cpu_median_ms=%.6f cpu_p95_ms=%.6f\n",
               count, sorted, gpu[30], gpu[57], cpu[30], cpu[57]);
        CHECK(stats.particles_alive == count && stats.validation_errors == 0 && stats.validation_warnings == 0);
    }
}

int main(int argc, char **argv) {
    SuRenderDesc desc = {0};
    SuRenderStats stats;
    SuRenderDeviceInfo device, devices[8];
    SuRenderTextureDesc texture = {0};
    uint8_t bytes[8 * 8 * 4];
    const uint8_t *data = bytes;
    uint32_t index, count = 0;
    int usable = 0, bench = argc > 1 && strcmp(argv[1], "--benchmark") == 0;
    char message[256];
    desc.struct_size = sizeof desc; desc.width = bench ? 1280u : SIZE; desc.height = bench ? 720u : SIZE;
    desc.device_index = -1; desc.flags = bench ? 0u : SU_RENDER_FLAG_VALIDATION;
    if (su_render_enumerate(devices, 8, &count, message) != SU_OK) { printf("SKIP: %s\n", message); return 77; }
    for (index = 0; index < count && index < 8u; index++) usable |= devices[index].meets_baseline != 0u;
    if (!usable) return 77;
    if (su_render_create(&desc, &renderer, message) != SU_OK) { fprintf(stderr, "create: %s\n", message); return 1; }
    CHECK(su_render_device(renderer, &device) == SU_OK);
    if (!device.gpu_particles) { printf("SKIP: selected graphics queue has no compute support\n"); su_render_destroy(renderer); return 77; }
    printf("device=%s driver=%u Vulkan=%u timestamps=%u resolution=%ux%u\n", device.name, device.driver_version,
           device.api_version, device.timestamps, desc.width, desc.height);
    memset(bytes, 255, sizeof bytes);
    texture.width = texture.height = 8; texture.format = SU_TEX_RGBA8_UNORM; texture.mip_count = 1;
    texture.mip_data = &data; texture.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_texture_create(renderer, &texture, &white) == SU_OK);
    for (index = 0; index < 64u; index++) {
        bytes[index * 4u] = index % 8u < 4u ? 255 : 50;
        bytes[index * 4u + 1u] = index / 8u < 4u ? 255 : 50;
    }
    CHECK(su_render_texture_create(renderer, &texture, &atlas) == SU_OK);
    reset(); frame.particle_emitter_count = 0;
    for (index = 0; index < 5u; index++) CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    if (bench) benchmark(); else { pixels_test(); large_sort_test(); }
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.validation_errors == 0 && stats.validation_warnings == 0);
    if (!bench && getenv("SUTEKH_REQUIRE_VALIDATION") != NULL) CHECK(stats.validation_enabled != 0u);
    printf("validation_enabled=%u errors=%u warnings=%u failures=%d\n", stats.validation_enabled,
           stats.validation_errors, stats.validation_warnings, failures);
    su_render_destroy(renderer);
    return failures != 0;
}
