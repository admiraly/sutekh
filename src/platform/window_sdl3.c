/* Linux window backend of sutekh/window.h: SDL 3, opened at run time with dlopen (no link-time dependency), for
 * the window, its Wayland or X11 handles, and keyboard, mouse and gamepad events, which are translated into the
 * portable input events of sutekh/input.h. Built against the pinned SDL 3.2 headers
 * (tools/bootstrap_sdl3_headers.py); every entry point used exists in SDL 3.2.0, and its signature is checked at
 * compile time against those headers. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if !defined(_WIN32)

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "platform/su_platform.h"
#include "platform/su_window_sdl3_int.h"
#include "sutekh/window.h"

/* ---------- run-time loading ---------- */

#define SDL_FNS(X)                                                                                           \
    X(int, SDL_GetVersion, (void))                                                                           \
    X(bool, SDL_SetHint, (const char *name, const char *value))                                              \
    X(bool, SDL_Init, (SDL_InitFlags flags))                                                                 \
    X(bool, SDL_InitSubSystem, (SDL_InitFlags flags))                                                        \
    X(void, SDL_Quit, (void))                                                                                \
    X(const char *, SDL_GetError, (void))                                                                    \
    X(const char *, SDL_GetCurrentVideoDriver, (void))                                                       \
    X(SDL_Window *, SDL_CreateWindow, (const char *title, int w, int h, SDL_WindowFlags flags))              \
    X(void, SDL_DestroyWindow, (SDL_Window * window))                                                        \
    X(SDL_PropertiesID, SDL_GetWindowProperties, (SDL_Window * window))                                      \
    X(void *, SDL_GetPointerProperty, (SDL_PropertiesID props, const char *name, void *default_value))       \
    X(Sint64, SDL_GetNumberProperty, (SDL_PropertiesID props, const char *name, Sint64 default_value))       \
    X(SDL_WindowFlags, SDL_GetWindowFlags, (SDL_Window * window))                                            \
    X(bool, SDL_GetWindowSizeInPixels, (SDL_Window * window, int *w, int *h))                                \
    X(float, SDL_GetWindowPixelDensity, (SDL_Window * window))                                               \
    X(bool, SDL_SetWindowTitle, (SDL_Window * window, const char *title))                                    \
    X(bool, SDL_SetWindowSize, (SDL_Window * window, int w, int h))                                          \
    X(bool, SDL_MinimizeWindow, (SDL_Window * window))                                                       \
    X(bool, SDL_RestoreWindow, (SDL_Window * window))                                                        \
    X(bool, SDL_SetWindowRelativeMouseMode, (SDL_Window * window, bool enabled))                             \
    X(bool, SDL_PollEvent, (SDL_Event * event))                                                              \
    X(SDL_Gamepad *, SDL_OpenGamepad, (SDL_JoystickID instance_id))                                          \
    X(void, SDL_CloseGamepad, (SDL_Gamepad * gamepad))

#define X(ret, name, args) typedef ret(SDLCALL *PFN_##name) args;
SDL_FNS(X)
#undef X
/* Each pointer type must be the type of the header's declaration (unevaluated: no reference to the symbol). */
#define X(ret, name, args) \
    _Static_assert(_Generic((&name), PFN_##name: 1, default: 0), #name " differs from the SDL3 headers");
SDL_FNS(X)
#undef X

typedef struct SdlFns {
    void *lib;
#define X(ret, name, args) PFN_##name name;
    SDL_FNS(X)
#undef X
} SdlFns;

/* The library stays loaded once opened: SDL may leave helper threads (HID, D-Bus) behind after SDL_Quit, and
 * unloading code they could still run is not safe. */
static SdlFns g_sdl;
static int g_window_live;

static int load_sdl(char *msg) {
    const char *name = getenv("SUTEKH_SDL3_LIBRARY");
    int v;
    if (g_sdl.lib != NULL) {
        return 1;
    }
    if (name == NULL || name[0] == '\0') {
        name = "libSDL3.so.0";
    }
    g_sdl.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (g_sdl.lib == NULL) {
        snprintf(msg, 256, "the SDL3 library (%s) could not be loaded: install SDL 3.2 or later, or set "
                 "SUTEKH_SDL3_LIBRARY", name);
        return 0;
    }
#define X(ret, fname, args)                                       \
    *(void **)&g_sdl.fname = dlsym(g_sdl.lib, #fname);            \
    if (g_sdl.fname == NULL) {                                    \
        snprintf(msg, 256, "%s has no entry point %s", name, #fname); \
        dlclose(g_sdl.lib);                                       \
        memset(&g_sdl, 0, sizeof g_sdl);                          \
        return 0;                                                 \
    }
    SDL_FNS(X)
#undef X
    v = g_sdl.SDL_GetVersion();
    if (v < SDL_VERSIONNUM(3, 2, 0)) {
        snprintf(msg, 256, "%s is SDL %d.%d.%d; 3.2.0 or later is required", name, SDL_VERSIONNUM_MAJOR(v),
                 SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
        dlclose(g_sdl.lib);
        memset(&g_sdl, 0, sizeof g_sdl);
        return 0;
    }
    return 1;
}

/* ---------- event translation (no library calls; the test seam) ---------- */

static uint32_t pad_button_bit(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SU_PAD_A;
    case SDL_GAMEPAD_BUTTON_EAST: return SU_PAD_B;
    case SDL_GAMEPAD_BUTTON_WEST: return SU_PAD_X;
    case SDL_GAMEPAD_BUTTON_NORTH: return SU_PAD_Y;
    case SDL_GAMEPAD_BUTTON_BACK: return SU_PAD_BACK;
    case SDL_GAMEPAD_BUTTON_START: return SU_PAD_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return SU_PAD_LTHUMB;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return SU_PAD_RTHUMB;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return SU_PAD_LSHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return SU_PAD_RSHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return SU_PAD_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return SU_PAD_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return SU_PAD_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return SU_PAD_DPAD_RIGHT;
    default: return 0u; /* guide, paddles, touchpad, misc: XInput reports none of them */
    }
}

static uint32_t mouse_button_bit(Uint8 b) {
    switch (b) {
    case SDL_BUTTON_LEFT: return SU_INPUT_MOUSE_LEFT;
    case SDL_BUTTON_MIDDLE: return SU_INPUT_MOUSE_MIDDLE;
    case SDL_BUTTON_RIGHT: return SU_INPUT_MOUSE_RIGHT;
    case SDL_BUTTON_X1: return SU_INPUT_MOUSE_X1;
    case SDL_BUTTON_X2: return SU_INPUT_MOUSE_X2;
    default: return 0u;
    }
}

static uint32_t find_pad(const SuSdlTranslate *t, SDL_JoystickID id) {
    uint32_t i;
    for (i = 0; i < SU_INPUT_MAX_PADS; i++) {
        if (id != 0u && t->pad_id[i] == id) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Whole counts of v plus the carried remainder, truncated toward zero (small motions in either direction are
 * treated alike); the new remainder is in (-1, 1) and carries to the next event. */
static int32_t take_whole(double *rem, double v) {
    double acc = *rem + (isfinite(v) ? v : 0.0);
    double whole = trunc(acc);
    if (whole > 1e9) {
        whole = 1e9;
    } else if (whole < -1e9) {
        whole = -1e9;
    }
    *rem = acc - whole;
    if (!(*rem > -1.0 && *rem < 1.0)) {
        *rem = 0.0;
    }
    return (int32_t)whole;
}

static void ev_init(SuInputEvent *o, uint32_t type, uint64_t now_ns) {
    memset(o, 0, sizeof *o);
    o->type = type;
    o->time_ns = now_ns;
}

uint32_t su_window_sdl3_translate(SuSdlTranslate *t, const SDL_Event *e, uint64_t now_ns, SuInputEvent *out,
                                  uint32_t *slot) {
    uint32_t i;
    if (slot != NULL) {
        *slot = UINT32_MAX;
    }
    if (t == NULL || e == NULL || out == NULL) {
        return 0u;
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        ev_init(out, SU_INPUT_EV_KEY, now_ns);
        out->code = (uint32_t)e->key.scancode; /* SDL scancodes are USB HID keyboard usages */
        out->value = e->key.down ? 1 : 0;
        return 1u;
    case SDL_EVENT_MOUSE_MOTION: {
        int32_t dx = take_whole(&t->rem_x, (double)e->motion.xrel);
        int32_t dy = take_whole(&t->rem_y, (double)e->motion.yrel);
        if (dx == 0 && dy == 0) {
            return 0u;
        }
        ev_init(out, SU_INPUT_EV_MOUSE_MOTION, now_ns);
        out->x = dx;
        out->y = dy;
        return 1u;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        uint32_t bit = mouse_button_bit(e->button.button);
        if (bit == 0u) {
            return 0u;
        }
        ev_init(out, SU_INPUT_EV_MOUSE_BUTTON, now_ns);
        out->code = bit;
        out->value = e->button.down ? 1 : 0;
        return 1u;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        double y = (double)e->wheel.y * 120.0;
        int32_t w;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            y = -y; /* "natural" scrolling: report the physical direction, as Raw Input does */
        }
        w = take_whole(&t->rem_wheel, y);
        if (w == 0) {
            return 0u;
        }
        ev_init(out, SU_INPUT_EV_WHEEL, now_ns);
        out->value = w;
        return 1u;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ev_init(out, SU_INPUT_EV_FOCUS, now_ns);
        out->value = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED ? 1 : 0;
        return 1u;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (find_pad(t, e->gdevice.which) != UINT32_MAX || e->gdevice.which == 0u) {
            return 0u;
        }
        for (i = 0; i < SU_INPUT_MAX_PADS; i++) {
            if (t->pad_id[i] == 0u) {
                t->pad_id[i] = e->gdevice.which;
                ev_init(out, SU_INPUT_EV_PAD_CONNECT, now_ns);
                out->device = i;
                out->value = 1;
                if (slot != NULL) {
                    *slot = i;
                }
                return 1u;
            }
        }
        return 0u; /* more than SU_INPUT_MAX_PADS controllers: the extra one is not used */
    case SDL_EVENT_GAMEPAD_REMOVED:
        i = find_pad(t, e->gdevice.which);
        if (i == UINT32_MAX) {
            return 0u;
        }
        t->pad_id[i] = 0u;
        ev_init(out, SU_INPUT_EV_PAD_CONNECT, now_ns);
        out->device = i;
        out->value = 0;
        if (slot != NULL) {
            *slot = i;
        }
        return 1u;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        uint32_t bit = pad_button_bit(e->gbutton.button);
        i = find_pad(t, e->gbutton.which);
        if (i == UINT32_MAX || bit == 0u) {
            return 0u;
        }
        ev_init(out, SU_INPUT_EV_PAD_BUTTON, now_ns);
        out->device = i;
        out->code = bit;
        out->value = e->gbutton.down ? 1 : 0;
        return 1u;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int32_t v = e->gaxis.value;
        i = find_pad(t, e->gaxis.which);
        if (i == UINT32_MAX) {
            return 0u;
        }
        ev_init(out, SU_INPUT_EV_PAD_AXIS, now_ns);
        out->device = i;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: out->code = SU_INPUT_PAD_AXIS_LX; break;
        case SDL_GAMEPAD_AXIS_LEFTY: out->code = SU_INPUT_PAD_AXIS_LY; v = -v; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: out->code = SU_INPUT_PAD_AXIS_RX; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: out->code = SU_INPUT_PAD_AXIS_RY; v = -v; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: out->code = SU_INPUT_PAD_AXIS_LT; v = v < 0 ? 0 : v >> 7; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: out->code = SU_INPUT_PAD_AXIS_RT; v = v < 0 ? 0 : v >> 7; break;
        default: return 0u;
        }
        out->value = v > 32767 ? 32767 : v; /* -(-32768): +y up keeps the XInput range */
        return 1u;
    }
    default:
        return 0u;
    }
}

/* ---------- the window ---------- */

struct SuWindow {
    SDL_Window *win;
    uint32_t window_system;
    void *display;
    void *surface;
    uint64_t xid;
    int gamepads;
    int relative;
    int close;
    uint32_t last_w, last_h;  /* reported by the previous poll */
    SuSdlTranslate tr;
    SDL_Gamepad *pads[SU_INPUT_MAX_PADS];
    SuInputState st;
    char driver[16];
};

static uint64_t now_ns(void) {
    uint64_t t = 0;
    (void)su_platform_monotonic_ns(&t);
    return t;
}

static uint32_t fail(SuWindow *w, char *msg, const char *fmt, const char *detail) {
    if (msg != NULL) {
        snprintf(msg, 256, fmt, detail != NULL ? detail : "");
    }
    if (w != NULL) {
        if (w->win != NULL) {
            g_sdl.SDL_DestroyWindow(w->win);
        }
        free(w);
    }
    if (g_sdl.lib != NULL) {
        g_sdl.SDL_Quit();
    }
    return SU_E_UNSUPPORTED_METHOD;
}

uint32_t su_window_create(const SuWindowDesc *d, SuWindow **out, char message[256]) {
    char local[256];
    char *msg = message != NULL ? message : local;
    SuWindow *w;
    SDL_WindowFlags wf = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_PropertiesID props;
    const char *drv;
    msg[0] = '\0';
    if (out == NULL || d == NULL || d->struct_size != sizeof *d ||
        (d->flags & ~(SU_WINDOW_RESIZABLE | SU_WINDOW_FULLSCREEN | SU_WINDOW_NO_GAMEPADS)) != 0u || d->width == 0u ||
        d->height == 0u || d->width > 16384u || d->height > 16384u) {
        snprintf(msg, 256, "invalid window descriptor");
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (g_window_live) {
        snprintf(msg, 256, "a window already exists in this process (SDL video is process-global)");
        return SU_E_INVALID_ARGUMENT;
    }
    if (!load_sdl(msg)) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    /* Wayland, else X11 (XWayland included); never a console or dummy driver. SDL_VIDEO_DRIVER still wins. */
    (void)g_sdl.SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
    (void)g_sdl.SDL_SetHint(SDL_HINT_APP_NAME, d->title != NULL ? d->title : "Sutekh");
    if (!g_sdl.SDL_Init(SDL_INIT_VIDEO)) {
        const char *wl = getenv("WAYLAND_DISPLAY");
        const char *x = getenv("DISPLAY");
        char detail[200];
        snprintf(detail, sizeof detail, "%s (WAYLAND_DISPLAY %s, DISPLAY %s)", g_sdl.SDL_GetError(),
                 wl != NULL ? wl : "unset", x != NULL ? x : "unset");
        return fail(NULL, msg, "no Wayland or X11 display could be opened: %s", detail);
    }
    w = calloc(1, sizeof *w);
    if (w == NULL) {
        g_sdl.SDL_Quit();
        snprintf(msg, 256, "allocation failed");
        return SU_E_RESOURCE_LIMIT;
    }
    if ((d->flags & SU_WINDOW_NO_GAMEPADS) == 0u) {
        w->gamepads = g_sdl.SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : 0;
    }
    if (!w->gamepads) {
        w->st.flags |= SU_INPUT_FLAG_NO_XINPUT;
    }
    if ((d->flags & SU_WINDOW_RESIZABLE) != 0u) {
        wf |= SDL_WINDOW_RESIZABLE;
    }
    if ((d->flags & SU_WINDOW_FULLSCREEN) != 0u) {
        wf |= SDL_WINDOW_FULLSCREEN;
    }
    w->win = g_sdl.SDL_CreateWindow(d->title != NULL ? d->title : "Sutekh", (int)d->width, (int)d->height, wf);
    if (w->win == NULL) {
        return fail(w, msg, "SDL could not create the window: %s", g_sdl.SDL_GetError());
    }
    drv = g_sdl.SDL_GetCurrentVideoDriver();
    snprintf(w->driver, sizeof w->driver, "%s", drv != NULL ? drv : "");
    props = g_sdl.SDL_GetWindowProperties(w->win);
    if (strcmp(w->driver, "wayland") == 0) {
        w->window_system = SU_RENDER_WINDOW_WAYLAND;
        w->display = g_sdl.SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        w->surface = g_sdl.SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
        if (w->display == NULL || w->surface == NULL) {
            return fail(w, msg, "SDL reported no wl_display / wl_surface for the window%s", NULL);
        }
    } else if (strcmp(w->driver, "x11") == 0) {
        w->window_system = SU_RENDER_WINDOW_XLIB;
        w->display = g_sdl.SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        w->xid = (uint64_t)g_sdl.SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (w->display == NULL || w->xid == 0u) {
            return fail(w, msg, "SDL reported no X11 Display / Window for the window%s", NULL);
        }
    } else {
        return fail(w, msg, "SDL chose the video driver \"%s\"; only wayland and x11 can present", w->driver);
    }
    w->st.focused = (g_sdl.SDL_GetWindowFlags(w->win) & SDL_WINDOW_INPUT_FOCUS) != 0u ? 1u : 0u;
    w->st.flags |= SU_INPUT_FLAG_MOUSE_ABSOLUTE; /* relative mode off */
    {
        int pw = 0;
        int ph = 0;
        (void)g_sdl.SDL_GetWindowSizeInPixels(w->win, &pw, &ph);
        w->last_w = pw > 0 ? (uint32_t)pw : d->width;
        w->last_h = ph > 0 ? (uint32_t)ph : d->height;
    }
    g_window_live = 1;
    *out = w;
    return SU_OK;
}

void su_window_destroy(SuWindow *w) {
    uint32_t i;
    if (w == NULL) {
        return;
    }
    for (i = 0; i < SU_INPUT_MAX_PADS; i++) {
        if (w->pads[i] != NULL) {
            g_sdl.SDL_CloseGamepad(w->pads[i]);
        }
    }
    if (w->relative) {
        (void)g_sdl.SDL_SetWindowRelativeMouseMode(w->win, false);
    }
    g_sdl.SDL_DestroyWindow(w->win);
    g_sdl.SDL_Quit();
    free(w);
    g_window_live = 0;
}

uint32_t su_window_render_desc(const SuWindow *w, SuRenderDesc *d) {
    if (w == NULL || d == NULL || d->struct_size != sizeof *d) {
        return SU_E_INVALID_ARGUMENT;
    }
    d->hwnd = NULL;
    d->window_system = w->window_system;
    d->window_display = w->display;
    d->window_surface = w->window_system == SU_RENDER_WINDOW_WAYLAND ? w->surface : NULL;
    d->window_id = w->window_system == SU_RENDER_WINDOW_XLIB ? w->xid : 0u;
    d->width = w->last_w != 0u ? w->last_w : 1u;
    d->height = w->last_h != 0u ? w->last_h : 1u;
    return SU_OK;
}

static void on_pad_slot(SuWindow *w, const SDL_Event *e, uint32_t slot, SuInputEvent *ev) {
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        w->pads[slot] = g_sdl.SDL_OpenGamepad(e->gdevice.which);
        if (w->pads[slot] == NULL) {
            /* SDL could not open it: give the slot back and report nothing */
            w->tr.pad_id[slot] = 0u;
            ev->type = 0u;
        }
    } else if (w->pads[slot] != NULL) {
        g_sdl.SDL_CloseGamepad(w->pads[slot]);
        w->pads[slot] = NULL;
    }
}

uint32_t su_window_poll(SuWindow *w, SuWindowEvents *out) {
    SDL_Event e;
    SDL_WindowFlags f;
    uint32_t n = 0;
    uint32_t i;
    int pw = 0;
    int ph = 0;
    uint32_t cw;
    uint32_t ch;
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    while (g_sdl.SDL_PollEvent(&e)) {
        SuInputEvent ev[SU_SDL_MAX_EVENTS_PER_SDL_EVENT];
        uint32_t slot;
        uint32_t k;
        uint32_t m;
        n++;
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            w->close = 1;
            continue;
        }
        m = su_window_sdl3_translate(&w->tr, &e, now_ns(), ev, &slot);
        if (slot != UINT32_MAX && m > 0u) {
            on_pad_slot(w, &e, slot, &ev[0]);
        }
        for (k = 0; k < m; k++) {
            if (ev[k].type != 0u) {
                (void)su_input_state_apply(&w->st, &ev[k]);
            }
        }
    }
    f = g_sdl.SDL_GetWindowFlags(w->win);
    (void)g_sdl.SDL_GetWindowSizeInPixels(w->win, &pw, &ph);
    cw = pw > 0 ? (uint32_t)pw : 0u;
    ch = ph > 0 ? (uint32_t)ph : 0u;
    if ((f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) != 0u) {
        cw = 0u;
        ch = 0u;
    }
    if (out != NULL) {
        memset(out, 0, sizeof *out);
        out->close_requested = w->close ? 1u : 0u;
        out->resized = cw != w->last_w || ch != w->last_h;
        out->width = cw;
        out->height = ch;
        out->presentable = cw != 0u && ch != 0u;
        out->focused = w->st.focused;
        for (i = 0; i < SU_INPUT_MAX_PADS; i++) {
            out->pads_connected |= w->st.pads[i].connected ? 1u << i : 0u;
        }
        out->events = n;
    }
    w->last_w = cw;
    w->last_h = ch;
    return SU_OK;
}

uint32_t su_window_sample_input(SuWindow *w, SuInputState *out) {
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_input_state_take(&w->st, now_ns(), out);
    return SU_OK;
}

uint32_t su_window_set_relative_mouse(SuWindow *w, int on) {
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (!g_sdl.SDL_SetWindowRelativeMouseMode(w->win, on != 0)) {
        return SU_E_UNSUPPORTED_METHOD;
    }
    w->relative = on != 0;
    if (w->relative) {
        w->st.flags &= ~SU_INPUT_FLAG_MOUSE_ABSOLUTE;
    } else {
        w->st.flags |= SU_INPUT_FLAG_MOUSE_ABSOLUTE;
    }
    return SU_OK;
}

void su_window_set_title(SuWindow *w, const char *title) {
    if (w != NULL && title != NULL) {
        (void)g_sdl.SDL_SetWindowTitle(w->win, title);
    }
}

uint32_t su_window_set_size(SuWindow *w, uint32_t width, uint32_t height) {
    if (w == NULL || width == 0u || height == 0u || width > 16384u || height > 16384u) {
        return SU_E_INVALID_ARGUMENT;
    }
    return g_sdl.SDL_SetWindowSize(w->win, (int)width, (int)height) ? SU_OK : SU_E_UNSUPPORTED_METHOD;
}

uint32_t su_window_minimize(SuWindow *w) {
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return g_sdl.SDL_MinimizeWindow(w->win) ? SU_OK : SU_E_UNSUPPORTED_METHOD;
}

uint32_t su_window_restore(SuWindow *w) {
    if (w == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return g_sdl.SDL_RestoreWindow(w->win) ? SU_OK : SU_E_UNSUPPORTED_METHOD;
}

uint32_t su_window_info(const SuWindow *w, SuWindowInfo *out) {
    int v;
    if (w == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    v = g_sdl.SDL_GetVersion();
    snprintf(out->backend, sizeof out->backend, "SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
             SDL_VERSIONNUM_MICRO(v));
    snprintf(out->video_driver, sizeof out->video_driver, "%s", w->driver);
    out->window_system = w->window_system;
    out->gamepads = w->gamepads ? 1u : 0u;
    out->relative_mouse = w->relative ? 1u : 0u;
    out->pixel_density = g_sdl.SDL_GetWindowPixelDensity(w->win);
    return SU_OK;
}

#endif /* !_WIN32 */
