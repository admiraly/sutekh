/* Windows device layer of sutekh/input.h with synthetic Raw Input structures and a fake XInputGetState: key codes
 * (E0 keys, fake shifts, scan code from the virtual key), mouse motion, buttons and wheel, absolute (remote
 * desktop) mouse motion, focus loss, sampling and the once-per-second polling of empty gamepad slots. Real devices
 * are exercised only by the manual examples/input/input_probe tool.
 *
 * Usage: test_input_win32 */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include <stdio.h>
#include <string.h>

#include "sutekh/input.h"

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

static uint32_t key(SuInputWin32 *c, USHORT make, USHORT flags, USHORT vkey) {
    RAWINPUT ri;
    memset(&ri, 0, sizeof ri);
    ri.header.dwType = RIM_TYPEKEYBOARD;
    ri.header.dwSize = sizeof ri;
    ri.data.keyboard.MakeCode = make;
    ri.data.keyboard.Flags = flags;
    ri.data.keyboard.VKey = vkey;
    return su_input_win32_feed_raw(c, &ri, sizeof ri, 1000);
}

static uint32_t mouse(SuInputWin32 *c, USHORT flags, LONG x, LONG y, USHORT buttons, SHORT data) {
    RAWINPUT ri;
    memset(&ri, 0, sizeof ri);
    ri.header.dwType = RIM_TYPEMOUSE;
    ri.header.dwSize = sizeof ri;
    ri.data.mouse.usFlags = flags;
    ri.data.mouse.lLastX = x;
    ri.data.mouse.lLastY = y;
    ri.data.mouse.usButtonFlags = buttons;
    ri.data.mouse.usButtonData = (USHORT)data;
    return su_input_win32_feed_raw(c, &ri, sizeof ri, 2000);
}

static int g_calls[4];
static int g_connected0 = 1;
static DWORD g_packet = 5;

static unsigned long fake_get_state(unsigned long slot, void *out) {
    XINPUT_STATE *xs = out;
    g_calls[slot]++;
    if (slot != 0u || !g_connected0) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    xs->dwPacketNumber = g_packet;
    xs->Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT;
    xs->Gamepad.sThumbLX = -32768;
    xs->Gamepad.sThumbLY = 1234;
    xs->Gamepad.sThumbRX = 32767;
    xs->Gamepad.sThumbRY = -5;
    xs->Gamepad.bLeftTrigger = 0;
    xs->Gamepad.bRightTrigger = 255;
    return ERROR_SUCCESS;
}

int main(void) {
    SuInputWin32 *c = NULL;
    SuInputState s;
    RAWINPUT ri;
    uint32_t st = su_input_win32_create(NULL, 0, &c);
    CHECK(st == SU_OK && c != NULL);
    if (c == NULL) {
        printf("test_input_win32: %ld checks, %ld failures (no context: %u)\n", g_checks, g_fail, (unsigned)st);
        return 1;
    }
    /* Keyboard. */
    CHECK(key(c, 0x11, RI_KEY_MAKE, 'W') == SU_OK);
    CHECK(su_input_win32_sample(c, &s) == SU_OK && su_input_key_down(&s, SU_KEY_W) && s.key_ns == 1000u);
    CHECK(s.focused == 1u && s.sample_ns > 0u);
    CHECK(key(c, 0x11, RI_KEY_BREAK, 'W') == SU_OK);
    CHECK(key(c, 0x48, RI_KEY_E0, VK_UP) == SU_OK);       /* arrow up: E0 48 */
    CHECK(key(c, 0x48, 0, VK_NUMPAD8) == SU_OK);          /* keypad 8: 48 */
    CHECK(key(c, 0x2A, RI_KEY_E0, VK_SHIFT) == SU_OK);    /* fake shift: ignored */
    CHECK(key(c, 0x1D, RI_KEY_E1, VK_PAUSE) == SU_OK);    /* pause: ignored */
    CHECK(key(c, 0, RI_KEY_E0, VK_RIGHT) == SU_OK);       /* no scan code: from the virtual key (4D) + E0 */
    CHECK(key(c, 0, 0, VK_RCONTROL) == SU_OK);            /* no scan code: the virtual key maps to E0 1D */
    CHECK(su_input_win32_sample(c, &s) == SU_OK);
    CHECK(!su_input_key_down(&s, SU_KEY_W) && su_input_key_down(&s, SU_KEY_UP) && su_input_key_down(&s, SU_KEY_KP_8));
    CHECK(!su_input_key_down(&s, SU_KEY_LSHIFT) && !su_input_key_down(&s, 0x12A) && !su_input_key_down(&s, SU_KEY_LCTRL));
    CHECK(su_input_key_down(&s, SU_KEY_RIGHT) && !su_input_key_down(&s, SU_KEY_KP_6));
    CHECK(su_input_key_down(&s, SU_KEY_RCTRL));
    /* Mouse: relative motion accumulates until the sample, then resets. */
    CHECK(mouse(c, MOUSE_MOVE_RELATIVE, 5, -3, 0, 0) == SU_OK);
    CHECK(mouse(c, MOUSE_MOVE_RELATIVE, 5, -3, RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL, -120) == SU_OK);
    CHECK(mouse(c, MOUSE_MOVE_RELATIVE, 0, 0, RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_BUTTON_5_DOWN, 0) == SU_OK);
    CHECK(su_input_win32_sample(c, &s) == SU_OK);
    CHECK(s.mouse_dx == 10 && s.mouse_dy == -6 && s.wheel == -120 && s.mouse_ns == 2000u);
    CHECK(s.mouse_buttons == (SU_INPUT_MOUSE_LEFT | SU_INPUT_MOUSE_RIGHT | SU_INPUT_MOUSE_X2));
    CHECK((s.flags & SU_INPUT_FLAG_MOUSE_ABSOLUTE) == 0u);
    CHECK(mouse(c, MOUSE_MOVE_RELATIVE, 0, 0, RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_BUTTON_5_UP, 0) == SU_OK);
    CHECK(su_input_win32_sample(c, &s) == SU_OK && s.mouse_dx == 0 && s.mouse_dy == 0 && s.wheel == 0);
    CHECK(s.mouse_buttons == SU_INPUT_MOUSE_RIGHT);
    /* Absolute motion (remote desktop): the first report sets the origin, later ones give pixel differences. */
    {
        LONG w = GetSystemMetrics(SM_CXSCREEN);
        LONG h = GetSystemMetrics(SM_CYSCREEN);
        CHECK(mouse(c, MOUSE_MOVE_ABSOLUTE, 0, 0, 0, 0) == SU_OK);
        CHECK(mouse(c, MOUSE_MOVE_ABSOLUTE, 65535, 32768, 0, 0) == SU_OK);
        CHECK(su_input_win32_sample(c, &s) == SU_OK);
        CHECK(s.mouse_dx == w && s.mouse_dy == (LONG)((int64_t)32768 * h / 65535));
        CHECK((s.flags & SU_INPUT_FLAG_MOUSE_ABSOLUTE) != 0u);
    }
    /* Focus loss releases everything and is reported once. */
    CHECK(su_input_win32_message(c, NULL, WM_KILLFOCUS, 0, 0) == 0);
    CHECK(su_input_win32_sample(c, &s) == SU_OK);
    CHECK(s.focused == 0u && (s.flags & SU_INPUT_FLAG_FOCUS_LOST) != 0u && s.mouse_buttons == 0u);
    CHECK(!su_input_key_down(&s, SU_KEY_UP) && !su_input_key_down(&s, SU_KEY_RIGHT));
    CHECK(su_input_win32_message(c, NULL, WM_ACTIVATEAPP, 1, 0) == 0);
    CHECK(su_input_win32_sample(c, &s) == SU_OK && s.focused == 1u && (s.flags & SU_INPUT_FLAG_FOCUS_LOST) == 0u);
    /* Malformed structures. */
    memset(&ri, 0, sizeof ri);
    ri.header.dwType = RIM_TYPEHID;
    CHECK(su_input_win32_feed_raw(c, &ri, sizeof ri, 0) == SU_E_INVALID_ARGUMENT);
    ri.header.dwType = RIM_TYPEMOUSE;
    CHECK(su_input_win32_feed_raw(c, &ri, sizeof(RAWINPUTHEADER) + 4u, 0) == SU_E_INVALID_ARGUMENT);
    CHECK(su_input_win32_feed_raw(c, NULL, 0, 0) == SU_E_INVALID_ARGUMENT);
    /* Gamepads through the fake XInput: slot 0 connected, slots 1..3 polled at most once per second. */
    su_input_win32_set_xinput(c, fake_get_state);
    CHECK(su_input_win32_sample(c, &s) == SU_OK);
    CHECK(s.pads[0].connected == 1u && s.pads[0].buttons == (SU_PAD_A | SU_PAD_DPAD_LEFT));
    CHECK(s.pads[0].lx == -32768 && s.pads[0].ly == 1234 && s.pads[0].rx == 32767 && s.pads[0].ry == -5);
    CHECK(s.pads[0].lt == 0u && s.pads[0].rt == 255u && s.pads[0].packet == 5u && s.pads[0].changed_ns > 0u);
    CHECK(s.pads[1].connected == 0u && (s.flags & SU_INPUT_FLAG_NO_XINPUT) == (s.flags & SU_INPUT_FLAG_NO_XINPUT));
    {
        uint64_t changed = s.pads[0].changed_ns;
        CHECK(su_input_win32_sample(c, &s) == SU_OK && s.pads[0].changed_ns == changed);
        g_packet = 6;
        Sleep(2);
        CHECK(su_input_win32_sample(c, &s) == SU_OK && s.pads[0].changed_ns > changed);
    }
    CHECK(g_calls[0] == 3 && g_calls[1] == 1 && g_calls[2] == 1 && g_calls[3] == 1);
    g_connected0 = 0;
    CHECK(su_input_win32_sample(c, &s) == SU_OK && s.pads[0].connected == 0u && s.pads[0].buttons == 0u);
    CHECK(su_input_win32_sample(c, &s) == SU_OK && g_calls[0] == 4);
    su_input_win32_set_xinput(c, NULL);
    su_input_win32_destroy(c);
    su_input_win32_destroy(NULL);
    printf("test_input_win32: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
