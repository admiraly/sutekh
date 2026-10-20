#include "sutekh/render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 128u
static int failures;
static SuRenderer *renderer;
static SuRenderFrameDesc frame;
static SuRenderFlame flames[128];
static uint8_t pixels[SIZE * SIZE * 4u];
static SuRenderHandle atlas, gradient, noise;
static uint32_t upload_flags = SU_RENDER_UPLOAD_COPY;

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); failures++; } } while (0)

static SuRenderHandle texture(uint32_t width, uint32_t height, uint32_t depth, const uint8_t *bytes) {
    SuRenderTextureDesc desc;
    SuRenderHandle handle = 0;
    memset(&desc, 0, sizeof desc);
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.format = SU_TEX_RGBA8_UNORM;
    desc.mip_count = 1;
    desc.mip_data = &bytes;
    desc.flags = upload_flags;
    CHECK(su_render_texture_create(renderer, &desc, &handle) == SU_OK);
    return handle;
}

static void reset(void) {
    memset(&frame, 0, sizeof frame);
    frame.struct_size = sizeof frame;
    frame.camera.rot.w = 1.0f;
    frame.camera.fov_y = 1.0471975512f;
    frame.camera.near_z = 0.05f;
    frame.light.no_sky = 1;
    frame.light.tonemap = SU_TONEMAP_CLAMP;
    memset(flames, 0, sizeof flames);
    flames[0].transform = su_mat4f_identity();
    flames[0].transform.m[14] = -3.0f;
    flames[0].colour[0] = 0.005f;
    flames[0].colour[3] = 1.0f;
    flames[0].gain = 1.0f;
    flames[0].shape_atlas = atlas;
    flames[0].gradient = gradient;
    flames[0].noise_volume = noise;
    frame.flames = flames;
    frame.flame_count = 1;
}

static void draw(void) {
    CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
}

static int red(void) {
    return pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u];
}

static int srgb(double value) {
    double encoded = value < 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return (int)floor(fmin(encoded, 1.0) * 255.0 + 0.5);
}

static uint64_t digest(void) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;
    for (index = 0; index < sizeof pixels; index++) {
        hash = (hash ^ pixels[index]) * UINT64_C(1099511628211);
    }
    return hash;
}

static void pixels_test(void) {
    SuRenderStats stats;
    SuRenderInstance wall;
    uint64_t empty;
    uint64_t baseline;
    int reference;
    uint32_t steps;
    reset();
    frame.flame_count = 0;
    draw();
    empty = digest();
    frame.flame_count = 1;
    draw();
    baseline = digest();
    reference = red();
    CHECK(abs(reference - srgb(0.18)) <= 2);
    CHECK(pixels[0] == 0u);
    CHECK(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u + 1u] == 0u);
    draw();
    CHECK(digest() == baseline);
    for (steps = 2; steps <= 12; steps++) {
        flames[0].steps = (float)steps;
        draw();
        CHECK(abs(red() - srgb(0.005 * steps * steps)) <= 2);
    }
    flames[0].steps = 1;
    draw();
    CHECK(abs(red() - srgb(0.02)) <= 2);
    flames[0].steps = 100;
    draw();
    CHECK(abs(red() - srgb(0.72)) <= 2);
    reset();
    flames[1] = flames[0];
    frame.flame_count = 2;
    draw();
    CHECK(abs(red() - srgb(0.36)) <= 2);
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.flames_drawn == 2u && stats.flame_draws == 1u && stats.flames_skipped == 0u);
    flames[1].transform.m[14] = -4.5f;
    flames[1].colour[0] = 0;
    flames[1].colour[2] = 0.005f;
    draw();
    CHECK(abs(red() - reference) <= 1);
    CHECK(abs(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u + 2u] - reference) <= 1);
    {
        SuRenderFlame temporary = flames[0];
        uint64_t ordered = digest();
        flames[0] = flames[1];
        flames[1] = temporary;
        draw();
        CHECK(ordered == digest());
        flames[0] = temporary;
    }
    flames[1].noise_volume = 0;
    draw();
    CHECK(abs(red() - reference) <= 1);
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.flames_drawn == 1u && stats.flames_skipped == 1u);
    reset();
    flames[0].gain = 0.0f;
    draw();
    CHECK(digest() == empty);
    flames[0].gain = 0.01f;
    draw();
    CHECK(digest() == empty);
    reset();
    memset(&wall, 0, sizeof wall);
    wall.rot[3] = 1;
    wall.scale = 1;
    wall.color[3] = 1;
    wall.pos[2] = -2.0f;
    frame.debug_instances = &wall;
    frame.debug_instance_count = 1;
    draw();
    CHECK(red() == 0);
    wall.pos[2] = -4.0f;
    draw();
    CHECK(abs(red() - srgb(0.09)) <= 2);
    flames[0].soft_distance = 2.0f;
    draw();
    CHECK(abs(red() - srgb(0.01125)) <= 2);
    frame.debug_instance_count = 0;
    draw();
    CHECK(abs(red() - reference) <= 1);
    reset();
    flames[0].transform.m[14] = 2;
    draw();
    CHECK(digest() == empty);
    flames[0].transform.m[14] = 0.5f;
    draw();
    CHECK(red() > 0);
    flames[0].transform.m[14] = -0.02f;
    draw();
    CHECK(red() > 0);
    reset();
    flames[0].transform.m[0] = -0.5f;
    flames[0].transform.m[5] = 0.7f;
    flames[0].transform.m[8] = 0.2f;
    draw();
    CHECK(red() > 0);
    reset();
    frame.camera.rot = su_quatf_axis_angle((SuVec3f){0,1,0}, 0.7f);
    draw();
    CHECK(red() == 0);
    flames[0].transform.m[0] = cosf(0.7f);
    flames[0].transform.m[2] = -sinf(0.7f);
    flames[0].transform.m[8] = sinf(0.7f);
    flames[0].transform.m[10] = cosf(0.7f);
    flames[0].transform.m[12] = -3.0f * sinf(0.7f);
    flames[0].transform.m[14] = -3.0f * cosf(0.7f);
    draw();
    CHECK(abs(red() - reference) <= 1);
    reset();
    flames[0].transform.m[0] = 0;
    flames[0].transform.m[2] = -1;
    flames[0].transform.m[8] = 1;
    flames[0].transform.m[10] = 0;
    flames[0].transform.m[12] = 0.5f;
    flames[0].transform.m[14] = -3.5f;
    draw();
    CHECK(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u - 8u) * 4u] > 0u);
    flames[0].gain = 0.5f;
    draw();
    CHECK(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u - 8u) * 4u] == 0u);
    reset();
    flames[0].transform.m[0] = 0;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    reset();
    flames[0].time = NAN;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    reset();
    flames[0].transform.m[3] = 1;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    reset();
    frame.flames = NULL;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    frame.flame_count = 4097;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
}

static int compare_double(const void *left, const void *right) {
    double first = *(const double *)left;
    double second = *(const double *)right;
    return first < second ? -1 : first > second;
}

static void inputs_test(void) {
    uint8_t atlas_bytes[8 * 16 * 4];
    uint8_t gradient_bytes[8 * 8 * 4];
    uint8_t volume_bytes[16 * 16 * 16 * 4];
    SuRenderHandle patterned_atlas;
    SuRenderHandle patterned_gradient;
    SuRenderHandle patterned_noise;
    SuRenderHandle borrowed_noise;
    uint32_t index;
    uint32_t seed = 7;
    uint64_t reference;
    SuRenderFlame saved;
    SuRenderStats stats;
    const uint8_t *mips[2] = {volume_bytes, volume_bytes};
    SuRenderTextureDesc desc;
    SuRenderHandle bad = 0;
    for (index = 0; index < sizeof atlas_bytes; index += 4u) {
        uint32_t row = index / (8u * 4u);
        uint32_t column = (index / 4u) % 8u;
        atlas_bytes[index] = row < 8u ? 255u : 0u;
        atlas_bytes[index + 1u] = row < 8u ? 0u : 255u;
        atlas_bytes[index + 2u] = 0;
        atlas_bytes[index + 3u] = (uint8_t)(column * 32u);
    }
    for (index = 0; index < sizeof gradient_bytes; index += 4u) {
        uint32_t row = index / (8u * 4u);
        uint32_t column = (index / 4u) % 8u;
        gradient_bytes[index] = (uint8_t)(255u - row * 32u);
        gradient_bytes[index + 1u] = (uint8_t)(row * 32u);
        gradient_bytes[index + 2u] = (uint8_t)(column * 32u);
        gradient_bytes[index + 3u] = (uint8_t)(128u + column * 16u);
    }
    for (index = 0; index < sizeof volume_bytes; index++) {
        seed = seed * 1664525u + 1013904223u;
        volume_bytes[index] = (uint8_t)(seed >> 24);
    }
    patterned_atlas = texture(8, 16, 0, atlas_bytes);
    patterned_gradient = texture(8, 8, 0, gradient_bytes);
    patterned_noise = texture(16, 16, 16, volume_bytes);
    upload_flags = 0;
    borrowed_noise = texture(16, 16, 16, volume_bytes);
    upload_flags = SU_RENDER_UPLOAD_COPY;
    reset();
    flames[0].colour[1] = flames[0].colour[2] = flames[0].colour[0];
    flames[0].shape_atlas = patterned_atlas;
    flames[0].atlas_rows = 2;
    draw();
    CHECK(red() > 0);
    CHECK(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u + 1u] == 0u);
    flames[0].atlas_index = 1;
    draw();
    CHECK(red() == 0);
    CHECK(pixels[(SIZE * (SIZE / 2u) + SIZE / 2u) * 4u + 1u] > 0u);
    flames[0].gradient = patterned_gradient;
    flames[0].noise_volume = patterned_noise;
    flames[0].amplitude_start = 0.25f;
    flames[0].amplitude_end = 0.6f;
    flames[0].frequency = 2.5f;
    flames[0].noise_velocity[0] = 0.7f;
    flames[0].noise_velocity[1] = -0.2f;
    flames[0].noise_velocity[2] = 0.4f;
    saved = flames[0];
    draw();
    reference = digest();
    flames[0].noise_volume = borrowed_noise;
    draw();
    CHECK(reference == digest());
    flames[0].noise_volume = patterned_noise;
    flames[0].time = 1.2f;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].phase = 0.4f;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].frequency = 0.8f;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].amplitude_start = 0;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].amplitude_end = 0;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].warp = -0.5f;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].scale = 0.4f;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].gain = 0.5f;
    draw();
    CHECK(reference != digest());
    flames[0] = saved;
    flames[0].colour[3] = 0;
    draw();
    CHECK(red() == 0);
    flames[0] = saved;
    flames[0].shape_atlas = patterned_noise;
    draw();
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.flames_skipped == 1u);
    CHECK(su_render_release(renderer, patterned_noise) == SU_OK);
    flames[0] = saved;
    draw();
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.flames_skipped == 1u);
    memset(&desc, 0, sizeof desc);
    desc.width = desc.height = desc.depth = 16;
    desc.format = SU_TEX_RGBA8_UNORM;
    desc.mip_count = 2;
    desc.mip_data = mips;
    CHECK(su_render_texture_create(renderer, &desc, &bad) == SU_E_INVALID_ARGUMENT);
    desc.mip_count = 1;
    desc.layers = 6;
    CHECK(su_render_texture_create(renderer, &desc, &bad) == SU_E_INVALID_ARGUMENT);
    desc.layers = 0;
    desc.format = SU_TEX_RGBA8_SRGB;
    CHECK(su_render_texture_create(renderer, &desc, &bad) == SU_E_INVALID_ARGUMENT);
    desc.format = SU_TEX_RGBA8_UNORM;
    desc.depth = 257;
    CHECK(su_render_texture_create(renderer, &desc, &bad) == SU_E_INVALID_ARGUMENT);
}

static void benchmark(void) {
    uint32_t count;
    uint32_t steps;
    uint32_t sample;
    uint32_t instance;
    SuRenderStats stats;
    for (steps = 2; steps <= 12; steps += steps == 2 ? 4u : 6u) {
        for (count = 0; count <= 128; count += count == 0 ? 32u : 96u) {
            double gpu[120];
            double cpu[120];
            reset();
            for (instance = 0; instance < count; instance++) {
                flames[instance] = flames[0];
                flames[instance].steps = (float)steps;
                flames[instance].transform.m[0] = 0.12f;
                flames[instance].transform.m[5] = 0.12f;
                flames[instance].transform.m[10] = 0.65f;
                flames[instance].transform.m[12] = ((float)(instance % 16u) - 7.5f) * 0.5f;
                flames[instance].transform.m[13] = ((float)(instance / 16u) - 3.5f) * 0.42f;
                flames[instance].transform.m[14] = -6;
                flames[instance].frequency = 2;
                flames[instance].amplitude_start = 0.08f;
                flames[instance].amplitude_end = 0.2f;
                flames[instance].noise_velocity[2] = 1;
                flames[instance].time = (float)instance * 0.15f;
            }
            frame.flame_count = count;
            for (sample = 0; sample < 140u; sample++) {
                CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
                CHECK(su_render_stats(renderer, &stats) == SU_OK);
                if (sample >= 20u) {
                    gpu[sample - 20u] = stats.gpu_ms;
                    cpu[sample - 20u] = stats.cpu_ms;
                }
            }
            qsort(gpu, 120, sizeof gpu[0], compare_double);
            qsort(cpu, 120, sizeof cpu[0], compare_double);
            printf("benchmark instances=%u steps=%u gpu_median_ms=%.6f gpu_p95_ms=%.6f cpu_median_ms=%.6f cpu_p95_ms=%.6f\n",
                   count, steps, gpu[60], gpu[114], cpu[60], cpu[114]);
            CHECK(gpu[0] >= 0.0);
            CHECK(stats.flames_drawn == count && stats.flame_draws == (count > 0u ? 1u : 0u));
        }
    }
}

int main(int argc, char **argv) {
    SuRenderDesc desc;
    SuRenderStats stats;
    SuRenderDeviceInfo device;
    uint8_t white[8 * 8 * 4];
    uint8_t volume[16 * 16 * 16 * 4];
    char message[256];
    int bench = argc > 1 && strcmp(argv[1], "--benchmark") == 0;
    uint32_t index;
    uint32_t device_count = 0;
    SuRenderDeviceInfo devices[8];
    int usable = 0;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.width = bench ? 1280u : SIZE;
    desc.height = bench ? 720u : SIZE;
    desc.device_index = -1;
    desc.flags = bench ? 0u : SU_RENDER_FLAG_VALIDATION;
    if (su_render_enumerate(devices, 8, &device_count, message) != SU_OK) {
        printf("SKIP: %s\n", message);
        return 77;
    }
    for (index = 0; index < device_count && index < 8u; index++) usable |= devices[index].meets_baseline != 0u;
    if (!usable) return 77;
    if (su_render_create(&desc, &renderer, message) != SU_OK) {
        fprintf(stderr, "create: %s\n", message);
        return 1;
    }
    CHECK(su_render_device(renderer, &device) == SU_OK);
    printf("device=%s driver=%u Vulkan=%u timestamps=%u resolution=%ux%u validation_requested=%d\n",
           device.name, device.driver_version, device.api_version, device.timestamps, desc.width, desc.height, !bench);
    memset(white, 255, sizeof white);
    for (index = 0; index < sizeof volume; index++) {
        volume[index] = bench ? (uint8_t)((index * 73u + index / 37u) % 256u) : 128u;
    }
    atlas = texture(8, 8, 0, white);
    gradient = texture(8, 8, 0, white);
    noise = texture(16, 16, 16, volume);
    reset();
    frame.flame_count = 0;
    for (index = 0; index < 5; index++) CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    if (bench) benchmark(); else {
        pixels_test();
        inputs_test();
    }
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.validation_errors == 0 && stats.validation_warnings == 0);
    if (!bench && getenv("SUTEKH_REQUIRE_VALIDATION") != NULL) CHECK(stats.validation_enabled != 0u);
    printf("validation_enabled=%u errors=%u warnings=%u failures=%d\n", stats.validation_enabled,
           stats.validation_errors, stats.validation_warnings, failures);
    su_render_destroy(renderer);
    return failures == 0 ? 0 : 1;
}
