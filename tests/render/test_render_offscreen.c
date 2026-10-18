/* Offscreen Vulkan rendering with readback (sutekh/render.h), checked pixel by pixel against expectations derived
 * from the scene: clear colour in a corner; a red box in front of a large green sphere (depth test: the centre is
 * red, a point beside the box is green); the box's front face fully lit (culling and winding: a culled front face
 * would show the dim inside); a blue ship; a yellow debug line on the row the projection predicts. The image is
 * written as a PPM. A second frame of the same scene must read back byte-identical.
 *
 * Exit 77 (ctest skip) when no Vulkan device meets the renderer's baseline, with the reason printed.
 *
 * Usage: test_render_offscreen [out.ppm] [--no-validation] */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/render.h"

#define W 256u
#define H 256u

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static const uint8_t *px(const uint8_t *img, uint32_t x, uint32_t y) {
    return img + 4u * ((size_t)y * W + x);
}

/* Pixel of a world point. */
static void project(SuMat4f vp, SuVec3f p, uint32_t *x, uint32_t *y) {
    SuVec3f q = su_mat4f_transform_point(vp, p, NULL);
    *x = (uint32_t)((q.x + 1.0f) * 0.5f * (float)W);
    *y = (uint32_t)((q.y + 1.0f) * 0.5f * (float)H);
}

static const char *type_name(uint32_t t) {
    static const char *const names[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    return t <= SU_RENDER_DEVICE_CPU ? names[t] : "?";
}

static void instance(SuRenderInstance *in, uint32_t mesh, SuVec3f p, float scale, float r, float g, float b) {
    memset(in, 0, sizeof *in);
    in->mesh = mesh;
    in->pos[0] = p.x;
    in->pos[1] = p.y;
    in->pos[2] = p.z;
    in->scale = scale;
    in->rot[3] = 1.0f;
    in->color[0] = r;
    in->color[1] = g;
    in->color[2] = b;
    in->color[3] = 1.0f;
}

int main(int argc, char **argv) {
    const char *out_path = argc > 1 && argv[1][0] != '-' ? argv[1] : "render_offscreen.ppm";
    int validation = 1;
    SuRenderDeviceInfo devs[8];
    uint32_t ndev = 0;
    char msg[256];
    uint32_t i;
    int usable = 0;
    SuRenderDesc d;
    SuRenderer *r = NULL;
    SuRenderView view;
    SuRenderInstance inst[3];
    SuRenderLineVertex lines[2];
    SuRenderStats st;
    SuRenderDeviceInfo used;
    SuMat4f vp;
    uint8_t *img = malloc(W * H * 4u);
    uint8_t *img2 = malloc(W * H * 4u);
    uint32_t x;
    uint32_t y;
    uint32_t st_code;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--no-validation") == 0) {
            validation = 0;
        }
    }
    if (img == NULL || img2 == NULL) {
        return 2;
    }
    msg[0] = '\0';
    st_code = su_render_enumerate(devs, 8, &ndev, msg);
    if (st_code != SU_OK) {
        printf("SKIP: Vulkan unavailable: %s\n", msg);
        return 77;
    }
    for (i = 0; i < ndev && i < 8u; i++) {
        printf("device %u: %s (%s, Vulkan %u.%u.%u, vendor 0x%04x, %llu MiB device-local, timestamps %s)%s%s\n",
               (unsigned)i, devs[i].name, type_name(devs[i].type), devs[i].api_version >> 22,
               (devs[i].api_version >> 12) & 0x3FFu, devs[i].api_version & 0xFFFu, (unsigned)devs[i].vendor_id,
               (unsigned long long)(devs[i].device_local_bytes >> 20), devs[i].timestamps ? "yes" : "no",
               devs[i].meets_baseline ? "" : " not usable: ", devs[i].meets_baseline ? "" : devs[i].why_not);
        usable |= devs[i].meets_baseline != 0u;
    }
    if (!usable) {
        printf("SKIP: no Vulkan device meets the baseline (%u found)%s%s\n", (unsigned)ndev, msg[0] ? ": " : "", msg);
        return 77;
    }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.flags = validation ? SU_RENDER_FLAG_VALIDATION : 0u;
    d.width = W;
    d.height = H;
    d.device_index = -1;
    st_code = su_render_create(&d, &r, msg);
    CHECK(st_code == SU_OK);
    if (st_code != SU_OK) {
        fprintf(stderr, "create: %s\n", msg);
        return 1;
    }
    CHECK(su_render_device(r, &used) == SU_OK);
    printf("rendering on: %s (%s)\n", used.name, type_name(used.type));

    memset(&view, 0, sizeof view);
    view.view = su_mat4f_look_at(v3(0, 0, 5), v3(0, 0, 0), v3(0, 1, 0));
    view.proj = su_mat4f_perspective(1.0471976f, 1.0f, 0.1f, 100.0f);
    view.clear[0] = 0.1f;
    view.clear[1] = 0.1f;
    view.clear[2] = 0.15f;
    view.clear[3] = 1.0f;
    view.light_dir = v3(0, 0, -1);
    view.ambient = 0.25f;
    vp = su_mat4f_mul(view.proj, view.view);
    /* Sphere first in the array: the renderer groups by mesh (box, sphere, ship), so the far sphere is drawn after
     * the near box and only the depth test keeps the box visible. */
    instance(&inst[0], SU_MESH_SPHERE, v3(0, 0, -3), 4.0f, 0.0f, 1.0f, 0.0f);
    instance(&inst[1], SU_MESH_BOX, v3(0, 0, 0), 1.0f, 1.0f, 0.0f, 0.0f);
    instance(&inst[2], SU_MESH_SHIP, v3(1.6f, 1.4f, 0), 1.2f, 0.2f, 0.4f, 1.0f);
    memset(lines, 0, sizeof lines);
    lines[0].pos[0] = -3.0f;
    lines[0].pos[1] = -1.5f;
    lines[1].pos[0] = 3.0f;
    lines[1].pos[1] = -1.5f;
    lines[0].rgba = lines[1].rgba = 0xFF00FFFFu; /* yellow, 0xAABBGGRR */

    CHECK(su_render_frame(r, &view, inst, 3, lines, 2) == SU_OK);
    CHECK(su_render_readback(r, img, W * H * 4u) == SU_OK);
    CHECK(su_render_frame(r, &view, inst, 3, lines, 2) == SU_OK);
    CHECK(su_render_readback(r, img2, W * H * 4u) == SU_OK);
    CHECK(memcmp(img, img2, W * H * 4u) == 0);
    /* Frames back to back, both frame slots in flight, then one readback (the pattern that needs the barrier
     * between consecutive copies into the shared readback buffer; synchronization validation checks it). */
    for (i = 0; i < 4u; i++) {
        CHECK(su_render_frame(r, &view, inst, 3, lines, 2) == SU_OK);
    }
    CHECK(su_render_readback(r, img2, W * H * 4u) == SU_OK);
    CHECK(memcmp(img, img2, W * H * 4u) == 0);
    CHECK(su_render_stats(r, &st) == SU_OK);
    printf("stats: frames %llu, draws %u, instances %u, line vertices %u, cpu %.3f ms, gpu %.3f ms, validation %s "
           "(%u errors, %u warnings)\n",
           (unsigned long long)st.frame, st.draws, st.instances, st.line_vertices, st.cpu_ms, st.gpu_ms,
           st.validation_enabled ? "on" : "not available", st.validation_errors, st.validation_warnings);
    CHECK(st.frame == 6u && st.draws == 4u && st.instances == 3u && st.line_vertices == 2u && st.dropped == 0u);
    CHECK(st.validation_errors == 0u);
    CHECK(used.timestamps ? st.gpu_ms >= 0.0 : st.gpu_ms < 0.0);
    CHECK(su_render_write_ppm(out_path, img, W, H) == SU_OK);
    printf("wrote %s\n", out_path);

    /* Clear colour: sRGB(0.1) = 89, sRGB(0.15) = 108. */
    {
        const uint8_t *c = px(img, 2, 2);
        printf("corner %u %u %u\n", c[0], c[1], c[2]);
        CHECK(abs(c[0] - 89) <= 2 && abs(c[1] - 89) <= 2 && abs(c[2] - 108) <= 2);
    }
    /* Box front face, lit head-on: full red. */
    {
        const uint8_t *c = px(img, W / 2u, H / 2u);
        printf("centre %u %u %u\n", c[0], c[1], c[2]);
        CHECK(c[0] >= 250 && c[1] <= 5 && c[2] <= 5);
    }
    /* Beside the box (24.6 px half size) but inside the sphere (about 55 px radius): green. */
    {
        const uint8_t *c = px(img, W / 2u + 40u, H / 2u);
        printf("sphere %u %u %u\n", c[0], c[1], c[2]);
        CHECK(c[1] >= 100 && c[0] <= 20 && c[2] <= 20);
    }
    /* Ship: the projection of a point inside the hull lands on it: blue dominates. */
    project(vp, v3(1.6f, 1.4f + 0.016f * 1.2f, 0.16f * 1.2f), &x, &y);
    {
        const uint8_t *c = px(img, x, y);
        printf("ship at %u,%u: %u %u %u\n", (unsigned)x, (unsigned)y, c[0], c[1], c[2]);
        CHECK(c[2] > c[0] + 40 && c[2] > c[1]);
    }
    /* Line: yellow within one row of the predicted one, left of the sphere. */
    project(vp, v3(-2.5f, -1.5f, 0.0f), &x, &y);
    {
        int found = 0;
        uint32_t yy;
        for (yy = y > 1u ? y - 1u : 0u; yy <= y + 1u && yy < H; yy++) {
            const uint8_t *c = px(img, x, yy);
            if (c[0] >= 200 && c[1] >= 200 && c[2] <= 80) {
                found = 1;
            }
        }
        printf("line near %u,%u: %s\n", (unsigned)x, (unsigned)y, found ? "yellow" : "missing");
        CHECK(found);
    }
    su_render_destroy(r);
    free(img);
    free(img2);
    printf("test_render_offscreen: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
