#include "sutekh/render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 64u
#define HEIGHT 64u
static int failures;
static uint8_t pixels[WIDTH * HEIGHT * 4u], reference[WIDTH * HEIGHT * 4u];
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

static SuHazeVolume haze(void) {
    SuHazeVolume volume;
    memset(&volume, 0, sizeof volume);
    volume.pos = su_wpos(0.0, 0.0, -4.0);
    volume.transform = su_mat4f_identity();
    volume.transform.m[0] = volume.transform.m[5] = volume.transform.m[10] = 2.0f;
    volume.color[0] = 0.3f;
    volume.color[1] = 0.15f;
    volume.color[2] = 0.1f;
    volume.density = 0.4f;
    return volume;
}

static void frame_init(SuRenderFrameDesc *frame) {
    memset(frame, 0, sizeof *frame);
    frame->struct_size = sizeof *frame;
    frame->camera.rot.w = 1.0f;
    frame->camera.fov_y = 1.0471976f;
    frame->camera.near_z = 0.1f;
    frame->light.no_sky = 1;
    frame->light.tonemap = SU_TONEMAP_CLAMP;
    frame->light.clear_color[0] = 0.04f;
    frame->light.clear_color[1] = 0.03f;
    frame->light.clear_color[2] = 0.02f;
    frame->light.clear_color[3] = 1.0f;
    frame->light.sun_dir.z = -1.0f;
    frame->light.sun_color.x = 0.5f;
    frame->light.sun_color.y = 0.2f;
    frame->light.sun_color.z = 0.1f;
    frame->light.sun_intensity = 2.0f;
}

static int srgb(float linear) {
    float value = fminf(1.0f, fmaxf(0.0f, linear));
    value = value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
    return (int)floorf(value * 255.0f + 0.5f);
}

static void check_image(SuRenderer *renderer, SuRenderFrameDesc *frame, float clip) {
    uint32_t x, y, bad = 0;
    CHECK(su_render_frame_ex(renderer, frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < WIDTH; x++) {
            SuVec3f ray = {((float)x + 0.5f) / (float)WIDTH * 2.0f - 1.0f,
                          -(((float)y + 0.5f) / (float)HEIGHT * 2.0f - 1.0f), -1.0f};
            float length;
            float rgb[3];
            uint32_t index, channel;
            ray.x *= tanf(frame->camera.fov_y * 0.5f);
            ray.y *= tanf(frame->camera.fov_y * 0.5f);
            length = sqrtf(ray.x * ray.x + ray.y * ray.y + 1.0f);
            ray.x /= length;
            ray.y /= length;
            ray.z /= length;
            ray = su_quatf_rotate(frame->camera.rot, ray);
            for (channel = 0; channel < 3u; channel++) rgb[channel] = clip > 0.0f ? 0.0f : frame->light.clear_color[channel];
            for (index = 0; index < frame->haze_count; index++) {
                const SuHazeVolume *volume = &frame->haze[index];
                float optical = su_haze_optical_depth(volume, frame->camera.pos, ray, clip > 0.0f ? clip / -ray.z : 1e6f);
                float alpha = 1.0f - expf(-optical);
                float sun[3] = {1.0f, 0.4f, 0.2f};
                for (channel = 0; channel < 3u; channel++) {
                    rgb[channel] = volume->color[channel] * (1.0f + volume->sun * sun[channel]) * alpha +
                                   rgb[channel] * (volume->blend == SU_HAZE_OVER ? 1.0f - alpha : 1.0f);
                }
            }
            for (channel = 0; channel < 3u; channel++) {
                int expected = srgb(rgb[channel]);
                int actual = pixels[((size_t)y * WIDTH + x) * 4u + channel];
                if (abs(expected - actual) > 3) {
                    if (bad == 0u) fprintf(stderr, "pixel %u,%u channel %u: %d expected %d (clip %g)\n", x, y, channel, actual, expected, clip);
                    bad++;
                }
            }
        }
    }
    CHECK(bad == 0u);
}

static int compare_double(const void *left, const void *right) {
    double first = *(const double *)left, second = *(const double *)right;
    return first < second ? -1 : first > second;
}

static void check_chord(SuRenderer *renderer) {
    SuRenderFrameDesc frame;
    SuHazeVolume volume = haze();
    uint32_t x, y, pass;
    frame_init(&frame);
    frame.haze = &volume;
    frame.haze_count = 1;
    volume.response = SU_HAZE_CHORD;
    volume.density = 0.1f;
    volume.chord_power = 2.0f;
    volume.source_size = 1.5f;
    for (pass = 0; pass < 3u; pass++) {
        uint32_t bad = 0;
        volume.source_brightness = pass == 0u ? 0.0f : 0.4f;
        if (pass == 2u) volume.chord_power = 0.0f;
        CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
        CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
        for (y = 0; y < HEIGHT; y++) {
            for (x = 0; x < WIDTH; x++) {
                SuVec3f ray = {(((float)x + 0.5f) / WIDTH * 2.0f - 1.0f) * tanf(frame.camera.fov_y * 0.5f),
                              -(((float)y + 0.5f) / HEIGHT * 2.0f - 1.0f) * tanf(frame.camera.fov_y * 0.5f), -1.0f};
                float length = sqrtf(ray.x * ray.x + ray.y * ray.y + 1.0f);
                float distance = 2.0f * sqrtf(fmaxf(0.0f, 1.0f - 1.0f / (length * length)));
                SuHazeVolume uniform = volume;
                float chord, alpha, power = pass == 2u ? 1.0f : 2.0f;
                uint32_t channel;
                uniform.density = 1.0f;
                uniform.falloff = 0.0f;
                chord = 0.5f * su_haze_optical_depth(&uniform, frame.camera.pos, ray, 1e6f);
                alpha = chord <= 0.0f ? 0.0f : fminf(1.0f, powf(chord * volume.density, power) +
                        volume.source_brightness * powf(fmaxf(0.0f, 1.0f - distance / 0.75f), power / 3.0f));
                for (channel = 0; channel < 3u; channel++) {
                    int expected = srgb(frame.light.clear_color[channel] + volume.color[channel] * alpha);
                    if (abs(expected - pixels[((size_t)y * WIDTH + x) * 4u + channel]) > 3) bad++;
                }
            }
        }
        CHECK(bad == 0u);
    }
}

static void check_shadow(SuRenderer *renderer) {
    SuMeshVertex vertices[24];
    uint16_t indices16[36];
    SuAssetVertex asset_vertices[24];
    uint32_t indices[36], vertex_count, index_count, index;
    SuAssetSubmesh submesh;
    SuRenderMeshDesc mesh_desc;
    SuRenderHandle mesh = 0, model = 0;
    SuRenderModelNode node;
    SuRenderObject object;
    SuRenderFrameDesc frame;
    SuHazeVolume volume = haze();
    int lit, shadowed;
    CHECK(su_mesh_build(SU_MESH_BOX, vertices, 24, &vertex_count, indices16, 36, &index_count) == SU_OK);
    memset(asset_vertices, 0, sizeof asset_vertices);
    for (index = 0; index < 24u; index++) {
        memcpy(asset_vertices[index].pos, vertices[index].pos, sizeof vertices[index].pos);
        memcpy(asset_vertices[index].normal, vertices[index].normal, sizeof vertices[index].normal);
        asset_vertices[index].tangent[0] = asset_vertices[index].tangent[3] = 1.0f;
    }
    for (index = 0; index < 36u; index++) indices[index] = indices16[index];
    memset(&submesh, 0, sizeof submesh);
    submesh.index_count = 36;
    submesh.radius = 0.866026f;
    memset(&mesh_desc, 0, sizeof mesh_desc);
    mesh_desc.vertices = asset_vertices;
    mesh_desc.vertex_count = vertex_count;
    mesh_desc.indices = indices;
    mesh_desc.index_count = index_count;
    mesh_desc.submeshes = &submesh;
    mesh_desc.submesh_count = 1;
    mesh_desc.radius = submesh.radius;
    mesh_desc.flags = SU_RENDER_UPLOAD_COPY;
    CHECK(su_render_mesh_create(renderer, &mesh_desc, &mesh) == SU_OK);
    memset(&node, 0, sizeof node);
    node.transform[0] = node.transform[5] = node.transform[10] = node.transform[15] = 1.0f;
    node.lod_mesh[0] = mesh;
    node.lod_screen[0] = 1.0f;
    node.lod_count = 1;
    CHECK(su_render_model_create(renderer, &node, 1, &model) == SU_OK);
    memset(&object, 0, sizeof object);
    object.model = model;
    object.pos = su_wpos(2.0, 0.0, -4.0);
    object.rot.w = 1.0f;
    object.scale = 1.0f;
    object.tint[0] = object.tint[1] = object.tint[2] = object.tint[3] = 1.0f;
    object.emissive = 1.0f;
    frame_init(&frame);
    frame.light.sun_dir.x = -1.0f;
    frame.light.sun_dir.z = 0.0f;
    frame.haze = &volume;
    frame.haze_count = 1;
    frame.objects = &object;
    frame.object_count = 1;
    frame.shadow.flags = SU_SHADOW_ON;
    frame.shadow.size = 256;
    frame.shadow.distance = 20.0f;
    volume.sun = 2.0f;
    for (index = 0; index < 3u; index++) CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
    lit = pixels[((HEIGHT / 2u) * WIDTH + WIDTH / 2u) * 4u];
    volume.flags = SU_HAZE_SHADOW;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
    shadowed = pixels[((HEIGHT / 2u) * WIDTH + WIDTH / 2u) * 4u];
    printf("haze shadow: lit=%d shadowed=%d\n", lit, shadowed);
    CHECK(lit > shadowed + 30);
    frame.shadow.cascades = 3;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
    CHECK(su_render_readback(renderer, pixels, sizeof pixels) == SU_OK);
    shadowed = pixels[((HEIGHT / 2u) * WIDTH + WIDTH / 2u) * 4u];
    CHECK(lit > shadowed + 30);
    CHECK(su_render_release(renderer, model) == SU_OK);
    CHECK(su_render_release(renderer, mesh) == SU_OK);
}

static int benchmark(void) {
    SuRenderDesc desc;
    SuRenderer *renderer = NULL;
    SuRenderDeviceInfo device;
    SuRenderFrameDesc frame;
    SuRenderStats stats;
    SuHazeVolume volumes[32];
    uint8_t *image = malloc(1280u * 720u * 4u);
    char message[256];
    uint32_t count, index;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.width = 1280;
    desc.height = 720;
    desc.device_index = -1;
    if (image == NULL) return 1;
    if (su_render_create(&desc, &renderer, message) != SU_OK) { free(image); return 77; }
    CHECK(su_render_device(renderer, &device) == SU_OK);
    printf("device=%s driver=%u timestamps=%u validation=off size=1280x720 samples=120 warmup=20\n",
           device.name, device.driver_version, device.timestamps);
    frame_init(&frame);
    for (index = 0; index < 32u; index++) {
        volumes[index] = haze();
        volumes[index].falloff = 1.0f;
        volumes[index].density = 0.01f;
    }
    frame.haze = volumes;
    for (count = 0; count <= 32u; count = count == 0u ? 1u : count == 1u ? 8u : count * 4u) {
        double gpu[120], cpu[120];
        frame.haze_count = count;
        for (index = 0; index < 140u; index++) {
            CHECK(su_render_frame_ex(renderer, &frame) == SU_OK);
            CHECK(su_render_readback(renderer, image, 1280u * 720u * 4u) == SU_OK);
            CHECK(su_render_stats(renderer, &stats) == SU_OK);
            if (index >= 20u) { gpu[index - 20u] = stats.gpu_ms; cpu[index - 20u] = stats.cpu_ms; }
        }
        qsort(gpu, 120, sizeof *gpu, compare_double);
        qsort(cpu, 120, sizeof *cpu, compare_double);
        printf("volumes=%u gpu_ms[min/median/p95/max]=%.6f/%.6f/%.6f/%.6f cpu_ms[median/p95]=%.6f/%.6f\n",
               count, gpu[0], (gpu[59] + gpu[60]) * 0.5, gpu[113], gpu[119], (cpu[59] + cpu[60]) * 0.5, cpu[113]);
    }
    su_render_destroy(renderer);
    free(image);
    return failures != 0;
}

int main(int argc, char **argv) {
    SuRenderDesc desc;
    SuRenderer *renderer = NULL;
    SuRenderFrameDesc frame;
    SuRenderStats stats;
    SuHazeVolume volumes[32];
    SuRenderInstance occluder;
    SuRenderDeviceInfo devices[8];
    uint32_t count = 0, index, shape, blend;
    char message[256];
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) return benchmark();
    if (su_render_enumerate(devices, 8, &count, message) != SU_OK) return 77;
    for (index = 0; index < count && index < 8u; index++) if (devices[index].meets_baseline) break;
    if (index == count || index == 8u) return 77;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.width = WIDTH;
    desc.height = HEIGHT;
    desc.device_index = -1;
    desc.flags = SU_RENDER_FLAG_VALIDATION;
    CHECK(su_render_create(&desc, &renderer, message) == SU_OK);
    if (renderer == NULL) { fprintf(stderr, "%s\n", message); return 1; }
    frame_init(&frame);
    check_image(renderer, &frame, 0.0f);
    memcpy(reference, pixels, sizeof pixels);
    frame.haze = volumes;
    frame.haze_count = 1;
    volumes[0] = haze();
    volumes[0].density = 0.0f;
    check_image(renderer, &frame, 0.0f);
    CHECK(memcmp(reference, pixels, sizeof pixels) == 0);
    frame.camera.rot = su_quatf_axis_angle((SuVec3f){0.0f, 1.0f, 0.0f}, 0.2f);
    check_image(renderer, &frame, 0.0f);
    for (shape = 0; shape <= SU_HAZE_BOX; shape++) {
        for (blend = 0; blend <= SU_HAZE_OVER; blend++) {
            volumes[0] = haze();
            volumes[0].shape = shape;
            volumes[0].blend = blend;
            check_image(renderer, &frame, 0.0f);
            volumes[0].falloff = 1.0f;
            check_image(renderer, &frame, 0.0f);
        }
    }
    volumes[0].transform = su_mat4f_from_quat(su_quatf_axis_angle((SuVec3f){0.2f, 1.0f, 0.3f}, 0.7f));
    for (index = 0; index < 3u; index++) {
        volumes[0].transform.m[index] *= 2.5f;
        volumes[0].transform.m[4u + index] *= 1.1f;
        volumes[0].transform.m[8u + index] *= 1.7f;
    }
    volumes[0].transform.m[4] += 0.3f;
    volumes[0].transform.m[12] = 0.2f;
    check_image(renderer, &frame, 0.0f);
    volumes[0].shape = SU_HAZE_ELLIPSOID;
    check_image(renderer, &frame, 0.0f);
    memcpy(reference, pixels, sizeof pixels);
    frame.camera.pos = su_wpos(50000.125, -25000.25, 70000.5);
    volumes[0].pos = su_wpos(50000.125, -25000.25, 69996.5);
    check_image(renderer, &frame, 0.0f);
    CHECK(memcmp(reference, pixels, sizeof pixels) == 0);
    frame_init(&frame);
    frame.haze = volumes;
    frame.haze_count = 1;
    volumes[0] = haze();
    volumes[0].pos = su_wpos(0.0, 0.0, -50000.0);
    volumes[0].transform.m[0] = 10000.0f;
    volumes[0].transform.m[5] = 7000.0f;
    volumes[0].transform.m[10] = 8000.0f;
    volumes[0].density = 0.00004f;
    volumes[0].falloff = 1.0f;
    check_image(renderer, &frame, 0.0f);
    volumes[0] = haze();
    volumes[0].pos = frame.camera.pos;
    check_image(renderer, &frame, 0.0f);
    volumes[0] = haze();
    volumes[0].sun = 0.5f;
    volumes[0].flags = SU_HAZE_SHADOW;
    frame.shadow.flags = SU_SHADOW_ON;
    frame.shadow.size = 256;
    frame.shadow.distance = 20.0f;
    check_image(renderer, &frame, 0.0f);
    frame.shadow.flags = 0;
    memset(&occluder, 0, sizeof occluder);
    occluder.scale = 100.0f;
    occluder.pos[2] = -54.0f;
    occluder.rot[3] = 1.0f;
    occluder.color[3] = 1.0f;
    frame.debug_instances = &occluder;
    frame.debug_instance_count = 1;
    check_image(renderer, &frame, 4.0f);
    volumes[0].falloff = 1.0f;
    check_image(renderer, &frame, 4.0f);
    occluder.pos[2] = -51.0f;
    check_image(renderer, &frame, 1.0f);
    frame.debug_instance_count = 0;
    for (index = 0; index < 32u; index++) { volumes[index] = haze(); volumes[index].density = 0.01f; }
    frame.haze_count = 32;
    check_image(renderer, &frame, 0.0f);
    for (index = 0; index < 32u; index++) volumes[index].blend = SU_HAZE_OVER;
    check_image(renderer, &frame, 0.0f);
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.haze_volumes_drawn == 32u);
    frame.haze_count = 1;
    volumes[0].pos = su_wpos(1000.0, 0.0, -4.0);
    check_image(renderer, &frame, 0.0f);
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(stats.haze_volumes_drawn == 0u);
    frame.haze = NULL;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    frame.haze = volumes;
    frame.haze_count = SU_HAZE_MAX_VOLUMES + 1u;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    frame.haze_count = 1;
    volumes[0].transform.m[0] = 0.0f;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    volumes[0] = haze();
    volumes[0].density = NAN;
    CHECK(su_render_frame_ex(renderer, &frame) == SU_E_INVALID_ARGUMENT);
    check_chord(renderer);
    check_shadow(renderer);
    CHECK(su_render_stats(renderer, &stats) == SU_OK);
    CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || stats.validation_enabled != 0u);
    CHECK(stats.validation_errors == 0u && stats.validation_warnings == 0u);
    printf("render_haze: validation=%u errors=%u warnings=%u failures=%d\n", stats.validation_enabled,
           stats.validation_errors, stats.validation_warnings, failures);
    su_render_destroy(renderer);
    return failures != 0;
}
