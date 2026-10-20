#include "sutekh/render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 128u
#define BYTES (SIZE * SIZE * 4u)
static int failures;
static SuRenderer *renderer;
static SuRenderFrameDesc frame;
static SuFxVertex vertices[192];
static SuFxBatch batches[32];
static uint8_t pixels[BYTES], reference[BYTES], distorted[BYTES];
static SuRenderHandle normal_map;

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); failures++; } } while (0)

static SuVec3f vector(float horizontal, float vertical, float depth) {
    SuVec3f result = {horizontal, vertical, depth};
    return result;
}

static void reset(void) {
    SuFxBuffer buffer = {0};
    const float white[4] = {1, 1, 1, 1};
    memset(&frame, 0, sizeof frame);
    memset(batches, 0, sizeof batches);
    frame.struct_size = sizeof frame;
    frame.camera.rot.w = 1;
    frame.camera.fov_y = 1.0471975512f;
    frame.camera.near_z = 0.05f;
    frame.light.tonemap = SU_TONEMAP_CLAMP;
    frame.light.up = vector(0, 1, 0);
    frame.light.sun_dir = vector(0, 0, -1);
    frame.light.sky_zenith = vector(0.05f, 0.05f, 0.6f);
    frame.light.sky_horizon = vector(0.6f, 0.05f, 0.05f);
    frame.light.sky_nadir = vector(0.05f, 0.6f, 0.05f);
    buffer.v = vertices;
    buffer.cap = 192;
    su_fx_sprite(&buffer, vector(0, 0, -4), 2, 2, 0, white, NULL, su_mat4f_identity());
    batches[0] = su_fx_batch(normal_map, SU_FX_BLEND_ADD, SU_FX_WRAP_CLAMP, SU_FX_WRAP_CLAMP, 0, &buffer);
    batches[0].distortion.mode = SU_DISTORT_NORMAL;
    batches[0].distortion.strength = 16;
    frame.fx_tex = vertices;
    frame.fx_tex_vertex_count = 6;
    frame.fx_batches = batches;
}

static void draw(void) {
    CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
}

static size_t at(uint32_t horizontal, uint32_t vertical) {
    return ((size_t)vertical * SIZE + horizontal) * 4u;
}

static void same_pixel(const uint8_t *first, size_t first_at, const uint8_t *second, size_t second_at, int tolerance) {
    uint32_t channel;
    for (channel = 0; channel < 4u; channel++) {
        CHECK(abs((int)first[first_at + channel] - second[second_at + channel]) <= tolerance);
    }
}

static void mesh_test(uint32_t two_sided) {
    SuRenderFxMaterialDesc material_desc = {0};
    SuRenderHandle material = 0, mesh_handle = 0, model = 0;
    SuAssetVertex mesh_vertices[6] = {0};
    uint32_t indices[6] = {0, 2, 1, 3, 5, 4};
    SuAssetSubmesh sub = {0};
    SuRenderMeshDesc mesh = {0};
    SuRenderModelNode node = {0};
    SuRenderObject object = {0};
    uint32_t index;
    reset();
    frame.fx_batch_count = 1;
    draw();
    memcpy(distorted, pixels, sizeof pixels);
    material_desc.layers.textures[0] = normal_map;
    material_desc.two_sided = two_sided;
    material_desc.distortion = batches[0].distortion;
    CHECK(su_render_fx_material_create(renderer, &material_desc, &material) == SU_OK);
    for (index = 0; index < 6u; index++) {
        memcpy(mesh_vertices[index].pos, vertices[index].pos, sizeof mesh_vertices[index].pos);
        mesh_vertices[index].pos[2] = 0;
        mesh_vertices[index].normal[2] = 1;
        mesh_vertices[index].tangent[0] = mesh_vertices[index].tangent[3] = 1;
        memcpy(mesh_vertices[index].uv0, vertices[index].uv, sizeof mesh_vertices[index].uv0);
    }
    sub.index_count = 6;
    sub.radius = 3;
    mesh.vertices = mesh_vertices;
    mesh.vertex_count = 6;
    mesh.indices = indices;
    mesh.index_count = 6;
    mesh.submeshes = &sub;
    mesh.submesh_count = 1;
    mesh.materials = &material;
    mesh.radius = 3;
    mesh.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_mesh_create(renderer, &mesh, &mesh_handle) == SU_OK);
    node.transform[0] = node.transform[5] = node.transform[10] = node.transform[15] = 1;
    node.lod_count = 1;
    node.lod_mesh[0] = mesh_handle;
    CHECK(su_render_model_create(renderer, &node, 1, &model) == SU_OK);
    object.model = model;
    object.pos = su_wpos(0, 0, -4);
    object.rot.w = 1;
    object.tint[0] = object.tint[1] = object.tint[2] = object.tint[3] = 1;
    frame.fx_batch_count = 0;
    frame.objects = &object;
    frame.object_count = 1;
    for (index = 0; index < 5u; index++) draw();
    same_pixel(pixels, at(64, 48), distorted, at(64, 48), 1);
    same_pixel(pixels, at(40, 80), distorted, at(40, 80), 1);
    CHECK(su_render_release(renderer, model) == SU_OK);
    CHECK(su_render_release(renderer, mesh_handle) == SU_OK);
    CHECK(su_render_release(renderer, material) == SU_OK);
}

static void pixel_tests(void) {
    SuRenderInstance wall = {0};
    SuRenderStats stats;
    uint32_t channel;
    reset();
    draw();
    memcpy(reference, pixels, sizeof pixels);
    frame.fx_batch_count = 1;
    batches[0].distortion.strength = 0;
    draw();
    CHECK(memcmp(reference, pixels, sizeof pixels) == 0);
    batches[0].distortion.strength = 16;
    draw();
    memcpy(distorted, pixels, sizeof pixels);
    CHECK(memcmp(reference, pixels, sizeof pixels) != 0);
    same_pixel(pixels, at(64, 48), reference, at(64, 64), 1);
    same_pixel(pixels, at(0, 0), reference, at(0, 0), 0);
    batches[0].distortion.strength = 255;
    draw();
    same_pixel(pixels, at(64, 48), reference, at(65, 127), 1);
    batches[0].distortion.strength = 16;
    batches[0].distortion.chromatic_split = 0.5f;
    draw();
    CHECK(abs((int)pixels[at(64, 48)] - reference[at(64, 72)]) <= 1);
    CHECK(abs((int)pixels[at(64, 48) + 2u] - reference[at(64, 56) + 2u]) <= 1);
    batches[0].distortion.chromatic_split = 0;
    batches[0].distortion.strength = -16;
    draw();
    same_pixel(pixels, at(64, 64), reference, at(64, 48), 1);
    batches[0].distortion.strength = 16;
    batches[0].distortion.falloff = 2;
    draw();
    CHECK(memcmp(distorted, pixels, sizeof pixels) != 0);
    batches[0].distortion.falloff = 0;
    for (channel = 0; channel < 6u; channel++) vertices[channel].color[3] = 0;
    draw();
    CHECK(memcmp(reference, pixels, sizeof pixels) == 0);
    reset();
    frame.fx_batch_count = 1;
    batches[0].texture = 0;
    batches[0].distortion.mode = SU_DISTORT_VORTEX;
    draw();
    CHECK(memcmp(reference, pixels, sizeof pixels) != 0);
    same_pixel(pixels, at(4, 4), reference, at(4, 4), 0);
    batches[0].distortion.mode = SU_DISTORT_RING;
    draw();
    same_pixel(pixels, at(64, 64), reference, at(64, 64), 0);
    CHECK(abs((int)pixels[at(64, 103)] - reference[at(64, 103)]) > 3);
    batches[0].distortion.ring_radius = 0.4f;
    batches[0].distortion.ring_width = 0.1f;
    draw();
    same_pixel(pixels, at(64, 103), reference, at(64, 103), 0);
    reset();
    wall.pos[2] = -2;
    wall.scale = 0.4f;
    wall.rot[3] = wall.color[3] = 1;
    wall.color[0] = 1;
    frame.debug_instances = &wall;
    frame.debug_instance_count = 1;
    draw();
    memcpy(reference, pixels, sizeof pixels);
    frame.fx_batch_count = 1;
    draw();
    same_pixel(pixels, at(64, 64), reference, at(64, 64), 0);
    same_pixel(pixels, at(64, 44), reference, at(64, 44), 0);
    batches[0].distortion.chromatic_split = 1;
    draw();
    CHECK(pixels[at(64, 32)] == reference[at(64, 32)]);
    reset();
    frame.fx_batch_count = 1;
    frame.post.flags = SU_POST_BLOOM;
    frame.post.bloom_threshold = 0.05f;
    frame.post.bloom_intensity = 0.4f;
    draw();
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.distortion_draws == 1u && stats.bloom_levels_active > 0u);
    reset();
    {
        SuFxBuffer buffer = {0};
        const float red[4] = {1, 0, 0, 1};
        buffer.v = vertices;
        buffer.count = 6;
        buffer.cap = 192;
        su_fx_sprite(&buffer, vector(0, 0, -6), 0.5f, 0.5f, 0, red, NULL, su_mat4f_identity());
        batches[1] = batches[0];
        batches[1].first_vertex = 6;
        batches[1].distortion.mode = SU_DISTORT_NONE;
        batches[1].blend = SU_FX_BLEND_PREMUL;
        frame.fx_tex_vertex_count = 12;
        frame.fx_batch_count = 2;
        draw();
        CHECK(pixels[at(64, 64)] >= 185u);
        CHECK(pixels[at(64, 64) + 1u] <= 2u);
    }
    mesh_test(0);
    mesh_test(1);
}

static void cube_tests(void) {
    float faces[6 * 8 * 8 * 4];
    SuRenderHandle cube = 0;
    uint32_t index;
    reset();
    frame.fx_batch_count = 1;
    draw();
    memcpy(distorted, pixels, sizeof pixels);
    for (index = 0; index < sizeof faces / sizeof faces[0]; index++) {
        faces[index] = index % 4u == 2u ? 0.25f : 0;
    }
    CHECK(su_render_env_create(renderer, 8, faces, &cube) == SU_OK);
    frame.distortion_cube = cube;
    frame.distortion_cube_intensity = 0.5f;
    for (index = 0; index < 5u; index++) draw();
    CHECK(pixels[at(64, 48) + 2u] > distorted[at(64, 48) + 2u] + 10u);
    same_pixel(pixels, at(0, 0), distorted, at(0, 0), 0);
    frame.distortion_cube_intensity = 0;
    draw();
    CHECK(memcmp(pixels, distorted, sizeof pixels) == 0);
    CHECK(su_render_release(renderer, cube) == SU_OK);
    frame.distortion_cube_intensity = 1;
    draw();
    CHECK(memcmp(pixels, distorted, sizeof pixels) == 0);
    frame.distortion_cube = normal_map;
    draw();
    CHECK(memcmp(pixels, distorted, sizeof pixels) == 0);
    batches[0].flags = SU_FX_BATCH_SORT;
    draw();
    CHECK(memcmp(pixels, distorted, sizeof pixels) == 0);
    batches[0].flags = 0;
    batches[1] = batches[0];
    batches[1].distortion.strength = -16;
    frame.fx_batch_count = 2;
    draw();
    memcpy(reference, pixels, sizeof pixels);
    batches[0] = batches[1];
    frame.fx_batch_count = 1;
    draw();
    CHECK(memcmp(pixels, reference, sizeof pixels) == 0);
    frame.fx_batch_count = 0;
    draw();
    memcpy(reference, pixels, sizeof pixels);
    frame.fx_batch_count = 1;
    batches[0].soft_distance = 1;
    draw();
    CHECK(memcmp(pixels, reference, sizeof pixels) != 0);
}

static void input_tests(void) {
    SuRenderFxMaterialDesc desc = {0};
    SuRenderHandle handle = 0;
    SuRenderStats stats;
    uint32_t index;
    const float invalid[] = {NAN, INFINITY, -INFINITY, 4097.0f};
    reset();
    frame.fx_batch_count = 1;
    for (index = 0; index < sizeof invalid / sizeof invalid[0]; index++) {
        desc.distortion.strength = invalid[index];
        CHECK(su_render_fx_material_create(renderer, &desc, &handle) == SU_E_INVALID_ARGUMENT);
        batches[0].distortion.strength = invalid[index];
        draw();
        CHECK(su_render_stats(renderer, &stats) == SU_OK);
        CHECK(stats.fx_batches_skipped == 1u && stats.distortion_draws == 0u);
    }
    memset(&desc, 0, sizeof desc);
    desc.distortion.mode = SU_DISTORT_RING + 1u;
    CHECK(su_render_fx_material_create(renderer, &desc, &handle) == SU_E_INVALID_ARGUMENT);
    desc.distortion.mode = SU_DISTORT_NORMAL;
    desc.distortion.falloff = -1;
    CHECK(su_render_fx_material_create(renderer, &desc, &handle) == SU_E_INVALID_ARGUMENT);
    desc.distortion.falloff = 0;
    desc.distortion.chromatic_split = 1.1f;
    CHECK(su_render_fx_material_create(renderer, &desc, &handle) == SU_E_INVALID_ARGUMENT);
    desc.distortion.chromatic_split = 0;
    desc.distortion.ring_radius = 1.1f;
    CHECK(su_render_fx_material_create(renderer, &desc, &handle) == SU_E_INVALID_ARGUMENT);
    desc.distortion.ring_radius = 0;
    desc.distortion.ring_width = NAN;
    CHECK(su_render_fx_material_create(renderer, &desc, &handle) == SU_E_INVALID_ARGUMENT);
    reset();
    frame.distortion_cube_intensity = NAN;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    frame.distortion_cube_intensity = -1;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    reset();
    frame.fx_batch_count = 1;
    batches[0].texture = UINT64_MAX;
    draw();
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.fx_batches_skipped == 1u && stats.distortion_draws == 0u);
}

static int compare_double(const void *first, const void *second) {
    double left = *(const double *)first;
    double right = *(const double *)second;
    return left < right ? -1 : left > right;
}

static void benchmark(uint32_t width, uint32_t height) {
    uint32_t count;
    uint32_t sample;
    SuRenderStats stats;
    for (count = 0; count <= 32u; count += count == 0u ? 1u : 31u) {
        double gpu[180], cpu[180];
        uint32_t index;
        reset();
        for (index = 0; index < 32u; index++) {
            SuFxBuffer buffer = {0};
            const float white[4] = {1, 1, 1, 1};
            buffer.v = vertices;
            buffer.count = index * 6u;
            buffer.cap = 192;
            su_fx_sprite(&buffer, vector(0, 0, -4), 3.2f, 1.8f, 0, white, NULL, su_mat4f_identity());
            batches[index] = batches[0];
            batches[index].first_vertex = index * 6u;
            batches[index].distortion.mode = SU_DISTORT_VORTEX;
            batches[index].distortion.falloff = 1;
            batches[index].distortion.chromatic_split = 0.1f;
        }
        frame.fx_tex_vertex_count = count * 6u;
        frame.fx_batch_count = count;
        for (sample = 0; sample < 210u; sample++) {
            CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
            CHECK(su_render_stats(renderer, &stats) == SU_OK);
            if (sample >= 30u) {
                gpu[sample - 30u] = stats.gpu_ms;
                cpu[sample - 30u] = stats.cpu_ms;
            }
        }
        qsort(gpu, 180, sizeof gpu[0], compare_double);
        qsort(cpu, 180, sizeof cpu[0], compare_double);
        printf("%ux%u effects=%u gpu_ms_p50=%.6f p95=%.6f cpu_ms_p50=%.6f p95=%.6f\n",
               width, height, count, gpu[90], gpu[171], cpu[90], cpu[171]);
        CHECK(gpu[0] >= 0.0 && stats.distortion_draws == count);
    }
}

int main(int argc, char **argv) {
    SuRenderDesc desc = {0};
    SuRenderTextureDesc texture = {0};
    SuRenderStats stats;
    SuRenderDeviceInfo device;
    char message[256];
    const uint8_t normal[4] = {128, 255, 128, 255};
    const uint8_t *bytes = normal;
    int bench = argc > 1 && strcmp(argv[1], "--benchmark") == 0;
    uint32_t status;
    uint32_t index;
    desc.struct_size = sizeof desc;
    desc.device_index = -1;
    desc.width = bench ? (argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1280u) : SIZE;
    desc.height = bench ? (argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 720u) : SIZE;
    desc.flags = bench ? 0u : SU_RENDER_FLAG_VALIDATION;
    status = su_render_create(&desc, &renderer, message);
    if (status == SU_E_UNSUPPORTED_METHOD) { printf("SKIP: %s\n", message); return 77; }
    if (status != SU_OK) { fprintf(stderr, "create: %s\n", message); return 1; }
    CHECK(su_render_device(renderer, &device) == SU_OK);
    printf("device=%s driver=%u api=%u timestamps=%u resolution=%ux%u\n", device.name,
           device.driver_version, device.api_version, device.timestamps, desc.width, desc.height);
    texture.width = texture.height = texture.mip_count = 1;
    texture.format = SU_TEX_RGBA8_UNORM;
    texture.mip_data = &bytes;
    texture.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_texture_create(renderer, &texture, &normal_map) == SU_OK);
    reset();
    for (index = 0; index < 5u; index++) CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    if (bench) benchmark(desc.width, desc.height); else { pixel_tests(); cube_tests(); input_tests(); }
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.validation_errors == 0 && stats.validation_warnings == 0);
    if (!bench && getenv("SUTEKH_REQUIRE_VALIDATION") != NULL) CHECK(stats.validation_enabled != 0u);
    printf("validation_enabled=%u errors=%u warnings=%u failures=%d\n", stats.validation_enabled,
           stats.validation_errors, stats.validation_warnings, failures);
    su_render_destroy(renderer);
    return failures == 0 ? 0 : 1;
}
