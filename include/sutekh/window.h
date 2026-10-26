/* Sutekh window (generation 1): a native window for the renderer's swapchain and native input, on Linux.
 *
 * Windows hosts create their own HWND and use su_input_win32 (sutekh/input.h); this header is the Linux
 * counterpart, so a host loop looks the same on both: create a window, create the renderer on it
 * (su_window_render_desc fills SuRenderDesc), and every frame poll the window, forward a size change to
 * su_render_resize, sample the input state, and map it per tick (su_input_mapper_tick).
 *
 * Backend: SDL 3 (zlib), opened at run time (libSDL3.so.0, 3.2.0 or later; SUTEKH_SDL3_LIBRARY overrides the
 * name), so nothing links against SDL and a build or run that never creates a window never loads it. SDL picks
 * Wayland when the session has it, else X11 (also XWayland); SDL_VIDEO_DRIVER=x11 forces X11. Only those two are
 * tried, never a console (KMS/DRM) or dummy driver. The renderer presents to the window's own wl_surface or X11
 * window through VK_KHR_wayland_surface / VK_KHR_xlib_surface (sutekh/render.h, window_system) and owns the
 * instance and the surface. The window is created with SDL_WINDOW_VULKAN, as SDL requires for a window Vulkan
 * presents to; with it SDL loads the Vulkan loader itself (so window creation also fails without one), but it
 * creates no Vulkan objects. Decision record: docs/BACKENDS_AND_DEPENDENCIES.md.
 *
 * Input has the Win32 backend's conventions (sutekh/input.h, portable device events): keys by physical position
 * (scan code set 1, from SDL's USB HID scancodes, independent of the keyboard layout), gamepads in the XInput
 * button bits and ranges (+y up, triggers 0..255) through SDL's gamepad API (which includes the Steam Deck's
 * controls and Steam Input's virtual pads; pads take slots 0..3 in connection order), mouse motion in counts and
 * the wheel in 120ths of a notch. So the same physical input gives the same SuInputState, and the same
 * per-tick records, on Windows and Linux. With relative mouse mode on (the camera case: the pointer is hidden and
 * confined, and motion is the device's unaccelerated motion) motion is device counts like Raw Input; with it off
 * motion is the difference of pointer positions in window coordinates (SDL's logical units: pixels at pixel
 * density 1, fewer than pixels on a scaled display) and SU_INPUT_FLAG_MOUSE_ABSOLUTE is set.
 *
 * Failure is structured: without the SDL3 library, without a Wayland or X11 display, or when SDL refuses the
 * window, su_window_create returns SU_E_UNSUPPORTED_METHOD with a plain-language message, and nothing else is
 * affected (offscreen rendering still works). Headless builds and the core never depend on this library.
 *
 * Library: su_window (static; Linux and other non-Windows builds, only when the pinned SDL3 headers are present:
 * tools/bootstrap_sdl3_headers.py). Threading: SDL's video subsystem is process-global, so one window per process,
 * created, polled and destroyed on the same thread (the main thread on most platforms). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_WINDOW_H
#define SUTEKH_WINDOW_H

#include <stdint.h>

#include "sutekh/input.h"
#include "sutekh/render.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_WINDOW_API
#define SU_WINDOW_API_VERSION 1u

typedef struct SuWindow SuWindow;

#define SU_WINDOW_RESIZABLE 0x1u    /* the user can resize the window */
#define SU_WINDOW_FULLSCREEN 0x2u   /* borderless fullscreen on the current display (a Steam Deck session) */
#define SU_WINDOW_NO_GAMEPADS 0x4u  /* do not start SDL's gamepad subsystem (SU_INPUT_FLAG_NO_XINPUT is then set) */

typedef struct SuWindowDesc {
    uint32_t struct_size;   /* sizeof(SuWindowDesc) */
    uint32_t flags;         /* SU_WINDOW_* */
    const char *title;      /* UTF-8; NULL: "Sutekh" */
    uint32_t width, height; /* requested size in window coordinates (pixels at scale 1), 1..16384 */
} SuWindowDesc;

/* Opens the window (shown at once). SU_E_INVALID_ARGUMENT for a bad descriptor or when a window already exists in
 * this process; SU_E_UNSUPPORTED_METHOD when the SDL3 library cannot be loaded (or is older than 3.2.0), when
 * neither a Wayland nor an X11 display can be opened, or when SDL refuses the window. message (optional, 256 bytes)
 * says why. A missing gamepad subsystem is not an error (the window works; pads never connect). */
SU_WINDOW_API uint32_t su_window_create(const SuWindowDesc *desc, SuWindow **out, char message[256]);
/* Closes the gamepads and the window and stops the SDL subsystems su_window_create started (video, gamepad) with
 * SDL_QuitSubSystem, not SDL_Quit, so other SDL use in the process is unaffected. The renderer presenting to it
 * must be destroyed first. NULL is ignored. */
SU_WINDOW_API void su_window_destroy(SuWindow *w);

/* Fills the window fields of a renderer descriptor (struct_size must be sizeof(SuRenderDesc)): window_system,
 * window_display, window_surface, window_id, and width/height with the current size in pixels. hwnd stays NULL.
 * SU_E_INVALID_ARGUMENT otherwise. */
SU_WINDOW_API uint32_t su_window_render_desc(const SuWindow *w, SuRenderDesc *d);

/* What happened since the previous poll. */
typedef struct SuWindowEvents {
    uint32_t close_requested;  /* the user closed the window, or the process got SIGINT/SIGTERM (stays set) */
    uint32_t resized;          /* width/height differ from the previous poll's: call su_render_resize */
    uint32_t width, height;    /* drawable size in pixels; 0 x 0 while minimized, hidden or occluded, as Win32's
                                  WM_SIZE reports a minimized window, so su_render_resize pauses presentation */
    uint32_t presentable;      /* 1 when width and height are non-zero */
    uint32_t focused;          /* 1 while the window has keyboard focus */
    uint32_t pads_connected;   /* bit i: pad slot i has a controller */
    uint32_t events;           /* SDL events processed by this poll */
} SuWindowEvents;

/* Processes every pending window-system event: window state, and keyboard, mouse and gamepad events into the
 * window's input state. Call once per frame, before su_window_sample_input. out may be NULL. */
SU_WINDOW_API uint32_t su_window_poll(SuWindow *w, SuWindowEvents *out);
/* The input state, with sample_ns = now (su_platform monotonic clock); then clears the relative quantities and
 * the per-sample flags, as su_input_win32_sample does. */
SU_WINDOW_API uint32_t su_window_sample_input(SuWindow *w, SuInputState *out);
/* Relative mouse mode (hidden, confined pointer; unaccelerated device motion): on for a mouse-driven camera.
 * SU_E_UNSUPPORTED_METHOD when the window system refuses it (the mode is then unchanged). */
SU_WINDOW_API uint32_t su_window_set_relative_mouse(SuWindow *w, int on);

SU_WINDOW_API void su_window_set_title(SuWindow *w, const char *title);
/* Requests a size (window coordinates), minimizes or restores. Requests: the window system may refuse or delay
 * them (a Wayland compositor may ignore a minimize, and never reports one), and su_window_poll reports what
 * actually happened. */
SU_WINDOW_API uint32_t su_window_set_size(SuWindow *w, uint32_t width, uint32_t height);
SU_WINDOW_API uint32_t su_window_minimize(SuWindow *w);
SU_WINDOW_API uint32_t su_window_restore(SuWindow *w);

typedef struct SuWindowInfo {
    char backend[32];          /* "SDL 3.4.16": the runtime library's version */
    char video_driver[16];     /* "wayland" or "x11" */
    uint32_t window_system;    /* SU_RENDER_WINDOW_WAYLAND or SU_RENDER_WINDOW_XLIB */
    uint32_t gamepads;         /* 1 when the gamepad subsystem is running */
    uint32_t relative_mouse;   /* 1 while relative mouse mode is on */
    float pixel_density;       /* drawable pixels per window coordinate */
} SuWindowInfo;
SU_WINDOW_API uint32_t su_window_info(const SuWindow *w, SuWindowInfo *out);

#ifdef __cplusplus
}
#endif

#endif
