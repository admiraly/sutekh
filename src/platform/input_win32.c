/* Windows device layer of sutekh/input.h: keyboard and mouse through Raw Input, gamepads through XInput loaded at
 * run time. Raw Input registration is per process and per device class, so one context per process. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include <stdlib.h>
#include <string.h>

#include "platform/su_platform.h"
#include "sutekh/input.h"

#define PAD_RETRY_NS UINT64_C(1000000000)
#define SINK_CLASS L"SutekhInputSink"

typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);

struct SuInputWin32 {
    HWND hwnd;
    int own_window;
    uint32_t flags;
    SuInputState st;
    HMODULE xinput;
    XInputGetStateFn get_state;
    unsigned long (*override_get_state)(unsigned long, void *);
    uint64_t pad_retry_ns[SU_INPUT_MAX_PADS];
    int have_abs;
    LONG abs_x;
    LONG abs_y;
};

static uint64_t now_ns(void) {
    uint64_t t = 0;
    (void)su_platform_monotonic_ns(&t);
    return t;
}

static LRESULT CALLBACK sink_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SuInputWin32 *c = (SuInputWin32 *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (c != NULL) {
        (void)su_input_win32_message(c, hwnd, msg, (uintptr_t)wp, (intptr_t)lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void load_xinput(SuInputWin32 *c) {
    static const wchar_t *const names[] = {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"};
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0] && c->get_state == NULL; i++) {
        c->xinput = LoadLibraryW(names[i]);
        if (c->xinput != NULL) {
            c->get_state = (XInputGetStateFn)(void (*)(void))GetProcAddress(c->xinput, "XInputGetState");
            if (c->get_state == NULL) {
                FreeLibrary(c->xinput);
                c->xinput = NULL;
            }
        }
    }
}

uint32_t su_input_win32_create(void *hwnd, uint32_t flags, SuInputWin32 **out) {
    SuInputWin32 *c;
    RAWINPUTDEVICE rid[2];
    if (out == NULL || (flags & ~SU_INPUT_WIN32_BACKGROUND) != 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    c->flags = flags;
    if (hwnd == NULL) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = sink_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = SINK_CLASS;
        (void)RegisterClassW(&wc); /* fails harmlessly when already registered */
        c->hwnd = CreateWindowExW(0, SINK_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
        if (c->hwnd == NULL) {
            free(c);
            return SU_E_IO;
        }
        SetWindowLongPtrW(c->hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->own_window = 1;
        c->flags |= SU_INPUT_WIN32_BACKGROUND;
        c->st.focused = 1u;
    } else {
        c->hwnd = (HWND)hwnd;
        c->st.focused = GetForegroundWindow() == c->hwnd ? 1u : 0u;
    }
    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06; /* keyboard */
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02; /* mouse */
    rid[0].dwFlags = rid[1].dwFlags = (c->flags & SU_INPUT_WIN32_BACKGROUND) != 0u ? RIDEV_INPUTSINK : 0u;
    rid[0].hwndTarget = rid[1].hwndTarget = c->hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof rid[0])) {
        if (c->own_window) {
            DestroyWindow(c->hwnd);
        }
        free(c);
        return SU_E_IO;
    }
    load_xinput(c);
    if (c->get_state == NULL) {
        c->st.flags |= SU_INPUT_FLAG_NO_XINPUT;
    }
    *out = c;
    return SU_OK;
}

void su_input_win32_destroy(SuInputWin32 *c) {
    RAWINPUTDEVICE rid[2];
    if (c == NULL) {
        return;
    }
    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;
    rid[0].dwFlags = rid[1].dwFlags = RIDEV_REMOVE;
    (void)RegisterRawInputDevices(rid, 2, sizeof rid[0]);
    if (c->own_window) {
        DestroyWindow(c->hwnd);
    }
    if (c->xinput != NULL) {
        FreeLibrary(c->xinput);
    }
    free(c);
}

void su_input_win32_set_focus(SuInputWin32 *c, int focused) {
    if (c == NULL) {
        return;
    }
    if (!focused) {
        memset(c->st.keys, 0, sizeof c->st.keys);
        c->st.mouse_buttons = 0;
        c->st.flags |= SU_INPUT_FLAG_FOCUS_LOST;
        c->have_abs = 0;
    }
    c->st.focused = focused ? 1u : 0u;
}

static void apply_keyboard(SuInputWin32 *c, const RAWKEYBOARD *k, uint64_t t) {
    uint32_t code = k->MakeCode;
    int down = (k->Flags & RI_KEY_BREAK) == 0;
    if ((k->Flags & RI_KEY_E1) != 0 || k->VKey == 0xFF) {
        return; /* Pause/Break and fake keys carry no usable scan code */
    }
    if (code == 0u) {
        /* No scan code: derive it from the virtual key. Windows maps ambiguous keys (arrows vs keypad) to the
         * non-extended code, so the E0 flag of the report still decides. */
        code = MapVirtualKeyW(k->VKey, MAPVK_VK_TO_VSC_EX);
        if ((code & 0xFF00u) == 0xE000u || (k->Flags & RI_KEY_E0) != 0) {
            code = 0x100u | (code & 0xFFu);
        }
    } else if ((k->Flags & RI_KEY_E0) != 0) {
        code |= 0x100u;
    }
    if (code == 0x12Au || code == 0x136u || code == 0u || code >= SU_INPUT_KEY_COUNT) {
        return; /* E0-prefixed fake shifts sent around navigation keys */
    }
    if (code == 0x145u) {
        /* NumLock: Raw Input and MAPVK_VK_TO_VSC_EX can report it E0-prefixed (0xE045); its set 1 position is
         * 0x45 (SU_KEY_NUMLOCK), as on Linux. Pause, the other key on 0x45, is an E1 sequence and returned above. */
        code = SU_KEY_NUMLOCK;
    }
    su_input_set_key(&c->st, code, down);
    c->st.key_ns = t;
}

static void apply_mouse(SuInputWin32 *c, const RAWMOUSE *m, uint64_t t) {
    USHORT bf = m->usButtonFlags;
    static const struct {
        USHORT down;
        USHORT up;
        uint32_t bit;
    } k_buttons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, SU_INPUT_MOUSE_LEFT},
                     {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, SU_INPUT_MOUSE_RIGHT},
                     {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, SU_INPUT_MOUSE_MIDDLE},
                     {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, SU_INPUT_MOUSE_X1},
                     {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, SU_INPUT_MOUSE_X2}};
    size_t i;
    if ((m->usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        /* Remote desktop and some tablets: normalized 0..65535 positions over the (virtual) screen. */
        int virt = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        LONG w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        LONG h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        LONG x = (LONG)((int64_t)m->lLastX * w / 65535);
        LONG y = (LONG)((int64_t)m->lLastY * h / 65535);
        if (c->have_abs) {
            c->st.mouse_dx += x - c->abs_x;
            c->st.mouse_dy += y - c->abs_y;
        }
        c->abs_x = x;
        c->abs_y = y;
        c->have_abs = 1;
        c->st.flags |= SU_INPUT_FLAG_MOUSE_ABSOLUTE;
    } else {
        c->st.mouse_dx += m->lLastX;
        c->st.mouse_dy += m->lLastY;
    }
    for (i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++) {
        if ((bf & k_buttons[i].down) != 0) {
            c->st.mouse_buttons |= k_buttons[i].bit;
        }
        if ((bf & k_buttons[i].up) != 0) {
            c->st.mouse_buttons &= ~k_buttons[i].bit;
        }
    }
    if ((bf & RI_MOUSE_WHEEL) != 0) {
        c->st.wheel += (SHORT)m->usButtonData;
    }
    c->st.mouse_ns = t;
}

uint32_t su_input_win32_feed_raw(SuInputWin32 *c, const void *rawinput, size_t len, uint64_t t) {
    const RAWINPUT *ri = rawinput;
    if (c == NULL || ri == NULL || len < sizeof(RAWINPUTHEADER)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (ri->header.dwType == RIM_TYPEKEYBOARD &&
        len >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)) {
        apply_keyboard(c, &ri->data.keyboard, t);
        return SU_OK;
    }
    if (ri->header.dwType == RIM_TYPEMOUSE && len >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) {
        apply_mouse(c, &ri->data.mouse, t);
        return SU_OK;
    }
    return SU_E_INVALID_ARGUMENT;
}

int su_input_win32_message(SuInputWin32 *c, void *hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    (void)hwnd;
    if (c == NULL) {
        return 0;
    }
    switch (msg) {
    case WM_INPUT: {
        union {
            RAWINPUT ri;
            unsigned char bytes[256];
        } buf;
        UINT size = sizeof buf;
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            (void)su_input_win32_feed_raw(c, &buf, size, now_ns());
        }
        return 1;
    }
    case WM_SETFOCUS:
        su_input_win32_set_focus(c, 1);
        return 0;
    case WM_KILLFOCUS:
        su_input_win32_set_focus(c, 0);
        return 0;
    case WM_ACTIVATEAPP:
        su_input_win32_set_focus(c, wparam != 0u);
        return 0;
    default:
        return 0;
    }
}

void su_input_win32_pump(SuInputWin32 *c) {
    MSG m;
    if (c == NULL || !c->own_window) {
        return;
    }
    while (PeekMessageW(&m, c->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void su_input_win32_set_xinput(SuInputWin32 *c, unsigned long (*get_state)(unsigned long, void *)) {
    if (c != NULL) {
        c->override_get_state = get_state;
        memset(c->pad_retry_ns, 0, sizeof c->pad_retry_ns);
    }
}

static void poll_pads(SuInputWin32 *c, uint64_t t) {
    DWORD i;
    if (c->override_get_state == NULL && c->get_state == NULL) {
        return;
    }
    for (i = 0; i < SU_INPUT_MAX_PADS; i++) {
        SuInputPad *p = &c->st.pads[i];
        XINPUT_STATE xs;
        DWORD r;
        if (!p->connected && t < c->pad_retry_ns[i]) {
            continue;
        }
        memset(&xs, 0, sizeof xs);
        r = c->override_get_state != NULL ? (DWORD)c->override_get_state(i, &xs) : c->get_state(i, &xs);
        if (r != ERROR_SUCCESS) {
            if (p->connected) {
                memset(p, 0, sizeof *p);
                p->changed_ns = t;
            }
            c->pad_retry_ns[i] = t + PAD_RETRY_NS;
            continue;
        }
        if (!p->connected || p->packet != xs.dwPacketNumber) {
            p->changed_ns = t;
        }
        p->connected = 1u;
        p->packet = xs.dwPacketNumber;
        p->buttons = xs.Gamepad.wButtons;
        p->lx = xs.Gamepad.sThumbLX;
        p->ly = xs.Gamepad.sThumbLY;
        p->rx = xs.Gamepad.sThumbRX;
        p->ry = xs.Gamepad.sThumbRY;
        p->lt = xs.Gamepad.bLeftTrigger;
        p->rt = xs.Gamepad.bRightTrigger;
    }
}

uint32_t su_input_win32_sample(SuInputWin32 *c, SuInputState *out) {
    uint64_t t;
    if (c == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    su_input_win32_pump(c);
    t = now_ns();
    poll_pads(c, t);
    c->st.sample_ns = t;
    *out = c->st;
    c->st.mouse_dx = 0;
    c->st.mouse_dy = 0;
    c->st.wheel = 0;
    c->st.flags &= ~SU_INPUT_FLAG_FOCUS_LOST;
    return SU_OK;
}

#endif /* _WIN32 */
