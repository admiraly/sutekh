/* Linux window and swapchain (sutekh/window.h, sutekh/render.h window_system), on a real display.
 *
 * Default: open a window, create the renderer on it (VK_KHR_wayland_surface or VK_KHR_xlib_surface, whichever SDL
 * picked), present frames, request a resize and require the swapchain to follow it, pause presentation with a
 * zero size and resume it, request a minimize (and on X11 a restore; Wayland has no unminimize request), and
 * print the evidence: video driver, device, present mode, frames, recreations, size, timings.
 * --x11: also require the X11 driver (the Xvfb registration).
 * --no-display: with WAYLAND_DISPLAY, DISPLAY, XDG_RUNTIME_DIR and SDL_VIDEO_DRIVER removed, su_window_create must
 * fail with SU_E_UNSUPPORTED_METHOD and a reason, renderer descriptors with bad window fields must be refused, and
 * offscreen rendering must still work (also through an API 11 sized descriptor).
 *
 * Exit 77 (ctest skip), with the reason printed, when there is no display, no SDL3 library, or no Vulkan device
 * that can present to the window (and, for --no-display, when the offscreen part has no Vulkan device).
 *
 * Usage: test_window_swapchain [--x11 | --no-display] [--frames N] */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* unsetenv, nanosleep, clock_gettime */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sutekh/render.h"
#include "sutekh/window.h"

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

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static SuVec3f v3(float x, float y, float z) {
    SuVec3f r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static SuRenderView g_view;
static SuRenderInstance g_inst;

static void scene(void) {
    memset(&g_view, 0, sizeof g_view);
    g_view.view = su_mat4f_look_at(v3(0, 0, 5), v3(0, 0, 0), v3(0, 1, 0));
    g_view.proj = su_mat4f_perspective(1.0471976f, 16.0f / 9.0f, 0.1f, 100.0f);
    g_view.clear[0] = 0.1f;
    g_view.clear[1] = 0.1f;
    g_view.clear[2] = 0.15f;
    g_view.clear[3] = 1.0f;
    g_view.light_dir = v3(0, 0, -1);
    g_view.ambient = 0.25f;
    memset(&g_inst, 0, sizeof g_inst);
    g_inst.mesh = SU_MESH_BOX;
    g_inst.scale = 1.0f;
    g_inst.rot[3] = 1.0f;
    g_inst.color[0] = 1.0f;
    g_inst.color[3] = 1.0f;
}

typedef struct Run {
    SuWindow *w;
    SuRenderer *r;
    SuWindowEvents ev;
    uint64_t ok, paused, failed;
} Run;

/* One frame as a host does it: poll, forward a size change, draw unless paused. */
static uint32_t frame(Run *run) {
    uint32_t st;
    CHECK(su_window_poll(run->w, &run->ev) == SU_OK);
    if (run->ev.resized) {
        su_render_resize(run->r, run->ev.width, run->ev.height);
    }
    g_inst.rot[2] = (float)((run->ok % 64u) / 64.0);
    st = su_render_frame(run->r, &g_view, &g_inst, 1, NULL, 0);
    if (st == SU_OK) {
        run->ok++;
    } else if (st == SU_E_RESOURCE_LIMIT) {
        run->paused++;
        sleep_ms(5); /* nothing presented: do not spin */
    } else {
        run->failed++;
        fprintf(stderr, "frame: %s\n", su_render_last_error(run->r));
    }
    return st;
}

static int no_display(void) {
    SuWindowDesc wd;
    SuWindow *w = NULL;
    SuRenderDesc d;
    SuRenderer *r = NULL;
    char msg[256];
    uint32_t st;
    int dummy;
    uint8_t px[64 * 64 * 4];
    (void)unsetenv("WAYLAND_DISPLAY");
    (void)unsetenv("DISPLAY");
    (void)unsetenv("XDG_RUNTIME_DIR");
    (void)unsetenv("SDL_VIDEO_DRIVER");
    memset(&wd, 0, sizeof wd);
    wd.struct_size = sizeof wd;
    wd.width = 320;
    wd.height = 200;
    msg[0] = '\0';
    st = su_window_create(&wd, &w, msg);
    printf("no display: su_window_create -> %u: %s\n", (unsigned)st, msg);
    CHECK(st == SU_E_UNSUPPORTED_METHOD && w == NULL && msg[0] != '\0');
    wd.width = 0;
    CHECK(su_window_create(&wd, &w, msg) == SU_E_INVALID_ARGUMENT);
    wd.width = 320;
    wd.struct_size = 4;
    CHECK(su_window_create(&wd, &w, msg) == SU_E_INVALID_ARGUMENT);

    /* bad window fields are refused before any Vulkan work */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.width = 64;
    d.height = 64;
    d.device_index = -1;
    d.window_system = SU_RENDER_WINDOW_WAYLAND;
    CHECK(su_render_create(&d, &r, msg) == SU_E_INVALID_ARGUMENT && r == NULL); /* no display/surface */
    d.window_system = SU_RENDER_WINDOW_XLIB;
    d.window_display = &dummy;
    CHECK(su_render_create(&d, &r, msg) == SU_E_INVALID_ARGUMENT); /* no window id */
    d.window_system = 99u;
    CHECK(su_render_create(&d, &r, msg) == SU_E_INVALID_ARGUMENT);
    d.window_system = SU_RENDER_WINDOW_NONE;
    d.window_display = NULL;
    d.hwnd = &dummy;
    CHECK(su_render_create(&d, &r, msg) == SU_E_UNSUPPORTED_METHOD); /* an HWND outside Windows */
    printf("hwnd on Linux: %s\n", msg);
    d.hwnd = &dummy;
    d.window_system = SU_RENDER_WINDOW_XLIB;
    d.window_display = &dummy;
    d.window_id = 1u;
    CHECK(su_render_create(&d, &r, msg) == SU_E_INVALID_ARGUMENT); /* both an HWND and a window system */

    /* offscreen still works, also through an API 11 sized descriptor */
    memset(&d, 0, sizeof d);
    d.struct_size = SU_RENDER_DESC_V2_SIZE;
    d.width = 64;
    d.height = 64;
    d.device_index = -1;
    st = su_render_create(&d, &r, msg);
    if (st == SU_E_UNSUPPORTED_METHOD) {
        printf("SKIP: the window part passed (%ld checks, %ld failures); offscreen needs Vulkan: %s\n", g_checks,
               g_fail, msg);
        return g_fail == 0 ? 77 : 1;
    }
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "offscreen create: %s\n", msg);
        return 1;
    }
    scene();
    CHECK(su_render_frame(r, &g_view, NULL, 0, NULL, 0) == SU_OK);
    CHECK(su_render_readback(r, px, sizeof px) == SU_OK);
    /* clear colour: sRGB(0.1) = 89, sRGB(0.15) = 108 */
    printf("offscreen without a display: pixel %u %u %u\n", px[0], px[1], px[2]);
    CHECK(px[0] >= 87 && px[0] <= 91 && px[2] >= 106 && px[2] <= 110);
    su_render_destroy(r);
    printf("test_window_swapchain --no-display: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int want_x11 = 0;
    uint32_t frames = 120;
    SuRenderDeviceInfo devs[8];
    uint32_t ndev = 0;
    SuWindowDesc wd;
    SuWindowInfo info;
    SuRenderDesc d;
    SuRenderDeviceInfo used;
    SuRenderStats rs;
    Run run;
    char msg[256];
    uint32_t st;
    uint32_t i;
    uint32_t w0;
    uint32_t h0;
    double t0;
    int usable = 0;
    int minimize_seen = 0;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--no-display") == 0) {
            return no_display();
        } else if (strcmp(argv[i], "--x11") == 0) {
            want_x11 = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < (uint32_t)argc) {
            frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    msg[0] = '\0';
    if (su_render_enumerate(devs, 8, &ndev, msg) != SU_OK) {
        printf("SKIP: Vulkan unavailable: %s\n", msg);
        return 77;
    }
    for (i = 0; i < ndev && i < 8u; i++) {
        usable |= devs[i].meets_baseline != 0u;
    }
    if (!usable) {
        printf("SKIP: no Vulkan device meets the baseline\n");
        return 77;
    }
    memset(&wd, 0, sizeof wd);
    wd.struct_size = sizeof wd;
    wd.flags = SU_WINDOW_RESIZABLE | SU_WINDOW_NO_GAMEPADS;
    wd.title = "sutekh test_window_swapchain";
    wd.width = 640;
    wd.height = 360;
    memset(&run, 0, sizeof run);
    st = su_window_create(&wd, &run.w, msg);
    if (st == SU_E_UNSUPPORTED_METHOD) {
        printf("SKIP: no window: %s\n", msg);
        return 77;
    }
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "window: %s\n", msg);
        return 1;
    }
    {
        SuWindow *second = NULL; /* SDL video is process-global: one window per process */
        CHECK(su_window_create(&wd, &second, msg) == SU_E_INVALID_ARGUMENT && second == NULL);
    }
    CHECK(su_window_info(run.w, &info) == SU_OK);
    printf("window: %s, driver %s, window system %u, pixel density %.2f\n", info.backend, info.video_driver,
           (unsigned)info.window_system, (double)info.pixel_density);
    if (want_x11) {
        CHECK(strcmp(info.video_driver, "x11") == 0 && info.window_system == SU_RENDER_WINDOW_XLIB);
    }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.flags = SU_RENDER_FLAG_VALIDATION;
    d.device_index = -1;
    CHECK(su_window_render_desc(run.w, &d) == SU_OK);
    CHECK(d.window_system == info.window_system && d.window_display != NULL && d.width > 0u && d.height > 0u);
    st = su_render_create(&d, &run.r, msg);
    if (st == SU_E_UNSUPPORTED_METHOD) {
        printf("SKIP: no Vulkan device can present to this %s window: %s\n", info.video_driver, msg);
        su_window_destroy(run.w);
        return 77;
    }
    CHECK(st == SU_OK);
    if (st != SU_OK) {
        fprintf(stderr, "renderer: %s\n", msg);
        su_window_destroy(run.w);
        return 1;
    }
    CHECK(su_render_device(run.r, &used) == SU_OK);
    scene();

    /* present */
    t0 = now_ms();
    for (i = 0; i < frames; i++) {
        (void)frame(&run);
    }
    CHECK(su_render_stats(run.r, &rs) == SU_OK);
    printf("presented %llu frames in %.1f ms (%.1f fps), %ux%u, present mode %u, cpu %.3f ms, wait %.3f ms, present "
           "%.3f ms, gpu %.3f ms\n",
           (unsigned long long)run.ok, now_ms() - t0, (double)run.ok * 1000.0 / (now_ms() - t0), rs.width, rs.height,
           rs.present_mode, rs.cpu_ms, rs.wait_ms, rs.present_ms, rs.gpu_ms);
    CHECK(run.ok >= frames * 3u / 4u && run.failed == 0u);
    w0 = rs.width;
    h0 = rs.height;

    /* resize: the swapchain follows the window */
    CHECK(su_window_set_size(run.w, 800, 500) == SU_OK);
    t0 = now_ms();
    while (now_ms() - t0 < 3000.0) {
        (void)frame(&run);
        CHECK(su_render_stats(run.r, &rs) == SU_OK);
        if (rs.width != w0 || rs.height != h0) {
            break;
        }
    }
    for (i = 0; i < 10u; i++) {
        (void)frame(&run);
    }
    CHECK(su_render_stats(run.r, &rs) == SU_OK);
    printf("resize request 800x500 (window coordinates): swapchain %ux%u -> %ux%u, recreations %u\n", w0, h0,
           rs.width, rs.height, rs.swapchain_recreations);
    CHECK(rs.width != w0 || rs.height != h0);
    CHECK(rs.swapchain_recreations >= 1u);
    CHECK(rs.width == run.ev.width && rs.height == run.ev.height);

    /* a zero size pauses presentation without touching the swapchain; a real size resumes it */
    {
        uint64_t before = rs.frame;
        su_render_resize(run.r, 0, 0);
        CHECK(su_render_frame(run.r, &g_view, &g_inst, 1, NULL, 0) == SU_E_RESOURCE_LIMIT);
        CHECK(su_render_frame(run.r, &g_view, &g_inst, 1, NULL, 0) == SU_E_RESOURCE_LIMIT);
        CHECK(su_render_stats(run.r, &rs) == SU_OK && rs.frame == before);
        su_render_resize(run.r, run.ev.width, run.ev.height);
        CHECK(su_render_frame(run.r, &g_view, &g_inst, 1, NULL, 0) == SU_OK);
        CHECK(su_render_stats(run.r, &rs) == SU_OK && rs.frame == before + 1u);
    }

    /* minimize and restore (last: Wayland has no request to unminimize, so there the window stays minimized until
     * the user restores it). Minimize is a request: X11 reports it; a Wayland compositor may report it as a
     * suspended (occluded) window, or not at all. */
    (void)su_window_minimize(run.w);
    t0 = now_ms();
    while (now_ms() - t0 < 1500.0) {
        (void)frame(&run);
        if (!run.ev.presentable) {
            minimize_seen = 1;
            break;
        }
    }
    if (minimize_seen) {
        uint64_t before;
        CHECK(su_render_stats(run.r, &rs) == SU_OK);
        before = rs.frame;
        for (i = 0; i < 5u; i++) {
            CHECK(frame(&run) == SU_E_RESOURCE_LIMIT);
        }
        CHECK(su_render_stats(run.r, &rs) == SU_OK && rs.frame == before);
        printf("minimize: reported (drawable 0x0), frames refused while minimized\n");
    } else {
        printf("minimize: not reported by the %s window system within 1.5 s\n", info.video_driver);
    }
    if (info.window_system == SU_RENDER_WINDOW_XLIB) {
        (void)su_window_restore(run.w);
        t0 = now_ms();
        while (now_ms() - t0 < 3000.0) {
            if (frame(&run) == SU_OK && run.ev.presentable) {
                break;
            }
        }
        CHECK(run.ev.presentable);
        for (i = 0; i < 10u; i++) {
            (void)frame(&run);
        }
        printf("restore: %s\n", run.ev.presentable ? "presenting again" : "still not presentable");
    } else {
        printf("restore: not attempted (Wayland cannot unminimize a window from the client)\n");
    }
    CHECK(su_render_stats(run.r, &rs) == SU_OK);
    printf("evidence: driver %s, device %s, present mode %u, frames %llu (ok %llu, paused %llu, failed %llu), "
           "swapchain recreations %u, final %ux%u, validation %s (%u errors, %u warnings)\n",
           info.video_driver, used.name, rs.present_mode, (unsigned long long)rs.frame, (unsigned long long)run.ok,
           (unsigned long long)run.paused, (unsigned long long)run.failed, rs.swapchain_recreations, rs.width,
           rs.height, rs.validation_enabled ? "on" : "not available", rs.validation_errors, rs.validation_warnings);
    CHECK(run.failed == 0u && rs.validation_errors == 0u);
    CHECK(getenv("SUTEKH_REQUIRE_VALIDATION") == NULL || rs.validation_enabled != 0u);
    su_render_destroy(run.r);
    su_window_destroy(run.w);
    printf("test_window_swapchain: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
