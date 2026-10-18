/* Manual smoke tool for the Windows input layer (sutekh/input.h): real devices through Raw Input and XInput.
 *
 *   input_probe [--seconds N] [--map examples/input/default_map.json]
 *       Samples at 120 Hz for N seconds (default 5) from a hidden background window, maps every sample with the
 *       map, and prints a line whenever the device state or the mapped record changes. Run from the repository
 *       root.
 *   input_probe --inject
 *       End-to-end check through the Windows input stack without a person: injects right-Ctrl down/up and a
 *       mouse move of +40 then -40 pixels with SendInput (no clicks, no wheel, no character keys: the session
 *       may be in use), then reports what Raw Input delivered. Exit 0 when both arrived. Injected input is not a
 *       physical device; this checks registration, message delivery and translation only. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/input.h"

static int load(const char *path, SuInputMap *map) {
    FILE *f = fopen(path, "rb");
    static uint8_t buf[65536];
    size_t n;
    char msg[160];
    uint32_t st;
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    st = su_input_map_from_json(buf, n, map, msg);
    if (st != SU_OK) {
        fprintf(stderr, "%s: %s\n", path, msg);
        return 0;
    }
    return 1;
}

static void send_key(WORD scan, int up) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | (up ? KEYEVENTF_KEYUP : 0u);
    (void)SendInput(1, &in, sizeof in);
}

static void send_move(LONG dx) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    (void)SendInput(1, &in, sizeof in);
}

static int inject(SuInputWin32 *c) {
    SuInputState s;
    int saw_key = 0;
    int32_t dx_total = 0;
    int32_t dx_max = 0;
    int i;
    (void)su_input_win32_sample(c, &s); /* drain */
    send_key(0x1D, 0); /* E0 1D: right Ctrl */
    for (i = 0; i < 20 && !saw_key; i++) {
        Sleep(10);
        (void)su_input_win32_sample(c, &s);
        saw_key = su_input_key_down(&s, SU_KEY_RCTRL);
    }
    send_key(0x1D, 1);
    send_move(40);
    for (i = 0; i < 20; i++) {
        Sleep(10);
        (void)su_input_win32_sample(c, &s);
        dx_total += s.mouse_dx;
        dx_max = dx_total > dx_max ? dx_total : dx_max;
        if (dx_max != 0) {
            break;
        }
    }
    send_move(-40);
    Sleep(30);
    (void)su_input_win32_sample(c, &s);
    printf("inject: right Ctrl %s; mouse dx after +40: %d (absolute mouse reports: %s); right Ctrl released: %s\n",
           saw_key ? "seen" : "NOT seen", (int)dx_max, (s.flags & SU_INPUT_FLAG_MOUSE_ABSOLUTE) ? "yes" : "no",
           su_input_key_down(&s, SU_KEY_RCTRL) ? "no" : "yes");
    return saw_key && dx_max != 0 && !su_input_key_down(&s, SU_KEY_RCTRL);
}

int main(int argc, char **argv) {
    const char *map_path = "examples/input/default_map.json";
    double seconds = 5.0;
    int do_inject = 0;
    SuInputWin32 *c = NULL;
    SuInputMap map;
    SuInputMapper mp;
    SuInputState prev;
    SuInputRecord prev_rec;
    uint32_t st;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--inject") == 0) {
            do_inject = 1;
        } else {
            fprintf(stderr, "usage: input_probe [--seconds N] [--map file] [--inject]\n");
            return 2;
        }
    }
    st = su_input_win32_create(NULL, 0, &c);
    if (st != SU_OK) {
        fprintf(stderr, "input context: status %u\n", (unsigned)st);
        return 1;
    }
    if (do_inject) {
        int ok = inject(c);
        su_input_win32_destroy(c);
        return ok ? 0 : 1;
    }
    if (!load(map_path, &map) || su_input_mapper_init(&mp, &map) != SU_OK) {
        su_input_win32_destroy(c);
        return 1;
    }
    memset(&prev, 0, sizeof prev);
    memset(&prev_rec, 0, sizeof prev_rec);
    {
        SuInputState s;
        (void)su_input_win32_sample(c, &s);
        printf("xinput: %s; pads connected:", (s.flags & SU_INPUT_FLAG_NO_XINPUT) ? "not available" : "loaded");
        for (i = 0; i < (int)SU_INPUT_MAX_PADS; i++) {
            printf(" %d", (int)s.pads[i].connected);
        }
        printf("\nsampling %.1f s at 120 Hz; press keys, move the mouse, use a gamepad\n", seconds);
    }
    for (i = 0; i < (int)(seconds * 120.0); i++) {
        SuInputState s;
        SuInputRecord r;
        uint32_t a;
        Sleep(8);
        (void)su_input_win32_sample(c, &s);
        (void)su_input_mapper_tick(&mp, &s, (uint64_t)i, &r);
        if (memcmp(s.keys, prev.keys, sizeof s.keys) != 0 || s.mouse_dx != 0 || s.mouse_dy != 0 || s.wheel != 0 ||
            s.mouse_buttons != prev.mouse_buttons || s.pads[0].packet != prev.pads[0].packet ||
            memcmp(r.axes, prev_rec.axes, sizeof r.axes) != 0 || r.buttons != prev_rec.buttons) {
            uint32_t k;
            printf("t=%8.3f keys:", (double)s.sample_ns / 1e9);
            for (k = 0; k < SU_INPUT_KEY_COUNT; k++) {
                if (su_input_key_down(&s, k)) {
                    const char *n = su_input_key_name(k);
                    if (n != NULL) {
                        printf(" %s", n);
                    } else {
                        printf(" 0x%03X", (unsigned)k);
                    }
                }
            }
            printf(" mouse %+d %+d wheel %+d buttons 0x%X%s | pad0 %s btn 0x%04X L(%d,%d) R(%d,%d) T(%u,%u) | record buttons 0x%X axes",
                   (int)s.mouse_dx, (int)s.mouse_dy, (int)s.wheel, (unsigned)s.mouse_buttons,
                   (s.flags & SU_INPUT_FLAG_MOUSE_ABSOLUTE) ? " (absolute)" : "", s.pads[0].connected ? "on" : "off",
                   (unsigned)s.pads[0].buttons, s.pads[0].lx, s.pads[0].ly, s.pads[0].rx, s.pads[0].ry,
                   (unsigned)s.pads[0].lt, (unsigned)s.pads[0].rt, (unsigned)r.buttons);
            for (a = 0; a < r.axis_count; a++) {
                printf(" %s=%.3f", map.axis_names[a], (double)r.axes[a] / SU_INPUT_ONE);
            }
            printf("\n");
        }
        prev = s;
        prev_rec = r;
    }
    su_input_win32_destroy(c);
    return 0;
}
