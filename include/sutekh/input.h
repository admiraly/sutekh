/* Sutekh native input (generation 1): device state, a data-driven mapping to fixed-layout per-tick records, and
 * the conversion of those records into host input writes (sutekh/host.h).
 *
 * Layers
 *   1. Device state (SuInputState): what the platform layer samples: keys, mouse motion and buttons, gamepads,
 *      with monotonic timestamps. Relative quantities (mouse motion, wheel) are accumulated between samples.
 *      On Windows it comes from Raw Input (keyboard, mouse) and XInput (gamepads): see the Win32 section below.
 *      On Linux it comes from the window backend (sutekh/window.h, SDL3), through the portable device events
 *      below, which convert to the same conventions. Tests and other platforms can fill it directly.
 *   2. Mapping (SuInputMap, SuInputMapper): a table of bindings from device sources to actions, applied once
 *      per simulation tick. Dead zones, response curves, the mouse virtual stick, scaling and clamping live
 *      here, never in the simulation. All arithmetic is integer (Q16.16), so a mapper fed the same sequence
 *      of states produces the same records on every machine.
 *   3. Record (SuInputRecord): the fixed-layout result for one tick: 32 buttons and 16 axes. This, not raw
 *      device events, is what a game stores in replays and sends over a network: su_input_record_writes turns
 *      it into SuHostInput writes for su_host_input_push, and su_host_input_encode records those canonically.
 *
 * Axis values are Q16.16 integers in [-SU_INPUT_ONE, SU_INPUT_ONE] (1.0 = 65536), written to u32 columns as
 * two's complement. Keys are named by physical position: the PC scan code set 1 make code, with 0x100 added for
 * E0-prefixed keys (so the key left of S is SU_KEY_A on every keyboard layout).
 *
 * Library: su_input (static). The mapping, record and device-event parts are portable C17; the Win32 part is
 * built on Windows only. Threading: one mapper or Win32 context must not be used from two threads at once. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_INPUT_H
#define SUTEKH_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/host.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* su_input is a static library: its functions are never imported from the host DLL. */
#define SU_INPUT_API

#define SU_INPUT_API_VERSION 2u              /* 2: portable device events (su_input_state_apply) */
#define SU_INPUT_ONE 65536              /* Q16.16 full deflection */
#define SU_INPUT_KEY_COUNT 512u         /* scan code set 1, E0-prefixed keys at 0x100 + code */
#define SU_INPUT_MAX_PADS 4u
#define SU_INPUT_MAX_AXES 16u
#define SU_INPUT_MAX_BUTTONS 32u
#define SU_INPUT_MAX_BINDINGS 64u
#define SU_INPUT_MAX_CURVES 8u          /* curve 0 is always linear and not stored */
#define SU_INPUT_MAX_CURVE_POINTS 9u
#define SU_INPUT_NAME_MAX 32u           /* action names, including the terminating NUL */

/* ---- keys (scan code set 1; physical positions of a US keyboard) ---- */
#define SU_KEY_ESCAPE 0x01u
#define SU_KEY_1 0x02u
#define SU_KEY_2 0x03u
#define SU_KEY_3 0x04u
#define SU_KEY_4 0x05u
#define SU_KEY_5 0x06u
#define SU_KEY_6 0x07u
#define SU_KEY_7 0x08u
#define SU_KEY_8 0x09u
#define SU_KEY_9 0x0Au
#define SU_KEY_0 0x0Bu
#define SU_KEY_MINUS 0x0Cu
#define SU_KEY_EQUALS 0x0Du
#define SU_KEY_BACKSPACE 0x0Eu
#define SU_KEY_TAB 0x0Fu
#define SU_KEY_Q 0x10u
#define SU_KEY_W 0x11u
#define SU_KEY_E 0x12u
#define SU_KEY_R 0x13u
#define SU_KEY_T 0x14u
#define SU_KEY_Y 0x15u
#define SU_KEY_U 0x16u
#define SU_KEY_I 0x17u
#define SU_KEY_O 0x18u
#define SU_KEY_P 0x19u
#define SU_KEY_LBRACKET 0x1Au
#define SU_KEY_RBRACKET 0x1Bu
#define SU_KEY_ENTER 0x1Cu
#define SU_KEY_LCTRL 0x1Du
#define SU_KEY_A 0x1Eu
#define SU_KEY_S 0x1Fu
#define SU_KEY_D 0x20u
#define SU_KEY_F 0x21u
#define SU_KEY_G 0x22u
#define SU_KEY_H 0x23u
#define SU_KEY_J 0x24u
#define SU_KEY_K 0x25u
#define SU_KEY_L 0x26u
#define SU_KEY_SEMICOLON 0x27u
#define SU_KEY_APOSTROPHE 0x28u
#define SU_KEY_GRAVE 0x29u
#define SU_KEY_LSHIFT 0x2Au
#define SU_KEY_BACKSLASH 0x2Bu
#define SU_KEY_Z 0x2Cu
#define SU_KEY_X 0x2Du
#define SU_KEY_C 0x2Eu
#define SU_KEY_V 0x2Fu
#define SU_KEY_B 0x30u
#define SU_KEY_N 0x31u
#define SU_KEY_M 0x32u
#define SU_KEY_COMMA 0x33u
#define SU_KEY_PERIOD 0x34u
#define SU_KEY_SLASH 0x35u
#define SU_KEY_RSHIFT 0x36u
#define SU_KEY_KP_MULTIPLY 0x37u
#define SU_KEY_LALT 0x38u
#define SU_KEY_SPACE 0x39u
#define SU_KEY_CAPSLOCK 0x3Au
#define SU_KEY_F1 0x3Bu
#define SU_KEY_F2 0x3Cu
#define SU_KEY_F3 0x3Du
#define SU_KEY_F4 0x3Eu
#define SU_KEY_F5 0x3Fu
#define SU_KEY_F6 0x40u
#define SU_KEY_F7 0x41u
#define SU_KEY_F8 0x42u
#define SU_KEY_F9 0x43u
#define SU_KEY_F10 0x44u
#define SU_KEY_NUMLOCK 0x45u
#define SU_KEY_SCROLLLOCK 0x46u
#define SU_KEY_KP_7 0x47u
#define SU_KEY_KP_8 0x48u
#define SU_KEY_KP_9 0x49u
#define SU_KEY_KP_MINUS 0x4Au
#define SU_KEY_KP_4 0x4Bu
#define SU_KEY_KP_5 0x4Cu
#define SU_KEY_KP_6 0x4Du
#define SU_KEY_KP_PLUS 0x4Eu
#define SU_KEY_KP_1 0x4Fu
#define SU_KEY_KP_2 0x50u
#define SU_KEY_KP_3 0x51u
#define SU_KEY_KP_0 0x52u
#define SU_KEY_KP_PERIOD 0x53u
#define SU_KEY_F11 0x57u
#define SU_KEY_F12 0x58u
#define SU_KEY_KP_ENTER 0x11Cu
#define SU_KEY_RCTRL 0x11Du
#define SU_KEY_KP_DIVIDE 0x135u
#define SU_KEY_RALT 0x138u
#define SU_KEY_HOME 0x147u
#define SU_KEY_UP 0x148u
#define SU_KEY_PAGEUP 0x149u
#define SU_KEY_LEFT 0x14Bu
#define SU_KEY_RIGHT 0x14Du
#define SU_KEY_END 0x14Fu
#define SU_KEY_DOWN 0x150u
#define SU_KEY_PAGEDOWN 0x151u
#define SU_KEY_INSERT 0x152u
#define SU_KEY_DELETE 0x153u
#define SU_KEY_LMETA 0x15Bu
#define SU_KEY_RMETA 0x15Cu
#define SU_KEY_MENU 0x15Du

/* Key code of a name ("W", "SPACE", "LSHIFT", "F1", "KP_8", "UP", ...; case-sensitive, the SU_KEY_ suffix),
 * or SU_E_INVALID_ARGUMENT. su_input_key_name returns the name or NULL. */
SU_INPUT_API uint32_t su_input_key_from_name(const char *name, uint32_t *key);
SU_INPUT_API const char *su_input_key_name(uint32_t key);

/* ---- device state ---- */

/* Gamepad buttons: the XInput bit layout. */
#define SU_PAD_DPAD_UP 0x0001u
#define SU_PAD_DPAD_DOWN 0x0002u
#define SU_PAD_DPAD_LEFT 0x0004u
#define SU_PAD_DPAD_RIGHT 0x0008u
#define SU_PAD_START 0x0010u
#define SU_PAD_BACK 0x0020u
#define SU_PAD_LTHUMB 0x0040u
#define SU_PAD_RTHUMB 0x0080u
#define SU_PAD_LSHOULDER 0x0100u
#define SU_PAD_RSHOULDER 0x0200u
#define SU_PAD_A 0x1000u
#define SU_PAD_B 0x2000u
#define SU_PAD_X 0x4000u
#define SU_PAD_Y 0x8000u

typedef struct SuInputPad {
    uint32_t connected;      /* 1 when the slot has a controller */
    uint32_t buttons;        /* SU_PAD_* */
    int16_t lx, ly, rx, ry;  /* sticks, -32768..32767, +y up */
    uint8_t lt, rt;          /* triggers, 0..255 */
    uint8_t reserved[2];
    uint32_t packet;         /* changes whenever the controller reports a change */
    uint64_t changed_ns;     /* monotonic time the state last changed */
} SuInputPad;

#define SU_INPUT_MOUSE_LEFT 0x01u
#define SU_INPUT_MOUSE_RIGHT 0x02u
#define SU_INPUT_MOUSE_MIDDLE 0x04u
#define SU_INPUT_MOUSE_X1 0x08u
#define SU_INPUT_MOUSE_X2 0x10u

/* Facts about where a state came from (SuInputState.flags). */
#define SU_INPUT_FLAG_MOUSE_ABSOLUTE 0x1u  /* the mouse reported absolute positions (typical of remote desktop;
                                              on Linux whenever relative mouse mode is off); motion is the
                                              difference of successive positions, in pixels */
#define SU_INPUT_FLAG_NO_XINPUT 0x2u       /* no XInput library (Linux: no gamepad subsystem): gamepads are never
                                              connected */
#define SU_INPUT_FLAG_FOCUS_LOST 0x4u      /* focus was lost since the previous sample: keys and buttons were released */

typedef struct SuInputState {
    uint64_t sample_ns;               /* monotonic time of the sample */
    uint64_t keys[SU_INPUT_KEY_COUNT / 64u];  /* bit k set: key k is down */
    int32_t mouse_dx, mouse_dy;       /* motion since the previous sample, in counts (+y down) */
    int32_t wheel;                    /* wheel motion since the previous sample, 120 per notch, + away from the user */
    uint32_t mouse_buttons;           /* SU_INPUT_MOUSE_* */
    uint64_t key_ns;                  /* monotonic time of the last keyboard event (0: none yet) */
    uint64_t mouse_ns;                /* monotonic time of the last mouse event (0: none yet) */
    SuInputPad pads[SU_INPUT_MAX_PADS];
    uint32_t focused;                 /* 1 while the input window has focus */
    uint32_t flags;                   /* SU_INPUT_FLAG_* */
} SuInputState;

static inline int su_input_key_down(const SuInputState *s, uint32_t key) {
    return key < SU_INPUT_KEY_COUNT && ((s->keys[key / 64u] >> (key % 64u)) & 1u) != 0u;
}
static inline void su_input_set_key(SuInputState *s, uint32_t key, int down) {
    if (key < SU_INPUT_KEY_COUNT) {
        if (down) {
            s->keys[key / 64u] |= (uint64_t)1u << (key % 64u);
        } else {
            s->keys[key / 64u] &= ~((uint64_t)1u << (key % 64u));
        }
    }
}

/* Part index of n (0 <= index < n) of a frame's state, for running n ticks from one sample: the relative
 * quantities (mouse motion, wheel) are divided so that the parts sum exactly to the whole and differ by at
 * most one count; everything else is copied. */
SU_INPUT_API uint32_t su_input_state_split(const SuInputState *frame, uint32_t n, uint32_t index, SuInputState *out);

/* ---- mapping ---- */

/* Binding sources. */
#define SU_INPUT_SRC_KEY 1u           /* code = key */
#define SU_INPUT_SRC_MOUSE_BUTTON 2u  /* code = SU_INPUT_MOUSE_* bit */
#define SU_INPUT_SRC_PAD_BUTTON 3u    /* code = SU_PAD_* bit, device = pad */
#define SU_INPUT_SRC_PAD_STICK 4u     /* code = SU_INPUT_STICK_*, device = pad; radial dead zone unless AXIAL */
#define SU_INPUT_SRC_PAD_TRIGGER 5u   /* code = 0 left, 1 right; device = pad */
#define SU_INPUT_SRC_MOUSE_DELTA 6u   /* code = SU_INPUT_MOUSE_AXIS_*: this tick's motion / counts */
#define SU_INPUT_SRC_MOUSE_STICK 7u   /* code = SU_INPUT_MOUSE_AXIS_*: virtual stick moved by motion / counts */

#define SU_INPUT_STICK_LX 0u
#define SU_INPUT_STICK_LY 1u
#define SU_INPUT_STICK_RX 2u
#define SU_INPUT_STICK_RY 3u
#define SU_INPUT_MOUSE_AXIS_X 0u
#define SU_INPUT_MOUSE_AXIS_Y 1u
#define SU_INPUT_MOUSE_AXIS_WHEEL 2u

/* Targets: an axis or a button of the record. */
#define SU_INPUT_AXIS(i) ((uint32_t)(i))
#define SU_INPUT_BUTTON(i) (0x100u | (uint32_t)(i))
#define SU_INPUT_TARGET_IS_BUTTON(t) (((t) & 0x100u) != 0u)
#define SU_INPUT_TARGET_INDEX(t) ((t) & 0xFFu)

#define SU_INPUT_BIND_AXIAL 0x1u       /* stick source: dead zone per axis instead of radial */

/* One binding. Conditioning of an analog value v (|v| <= ONE): values with |v| <= dead_zone give 0; the rest
 * is rescaled to (0, ONE] up to saturation (larger values give ONE), passed through the curve, and the sign
 * restored (sticks: radially, on the stick's magnitude, keeping its direction). Digital sources give 0 or ONE.
 * Axis targets add value * scale / ONE; the sum of all bindings of an axis is clamped to [-ONE, ONE]. Button
 * targets are pressed when a digital source is down, or when an analog value * scale / ONE >= threshold. */
typedef struct SuInputBinding {
    uint32_t source;      /* SU_INPUT_SRC_* */
    uint32_t code;        /* see the sources */
    uint32_t device;      /* gamepad index for pad sources, else 0 */
    uint32_t target;      /* SU_INPUT_AXIS(i) or SU_INPUT_BUTTON(i) */
    int32_t scale;        /* Q16.16; ONE for 1.0, -ONE to invert; 0 is invalid */
    int32_t dead_zone;    /* Q16.16 in [0, saturation) */
    int32_t saturation;   /* Q16.16 in (dead_zone, ONE] */
    uint32_t curve;       /* 0 = linear, else 1..curve_count */
    int32_t threshold;    /* Q16.16 in (0, ONE], analog source to button target */
    int32_t counts;       /* mouse sources: counts for a full deflection, >= 1 */
    int32_t recenter;     /* mouse stick: Q16.16 moved back toward 0 per tick, >= 0 */
    uint32_t flags;       /* SU_INPUT_BIND_* */
} SuInputBinding;

/* Monotonic piecewise-linear curve on [0, ONE]: point x values strictly increase from 0 to ONE, y values do not
 * decrease, start at 0 and stay within [0, ONE]. */
typedef struct SuInputCurve {
    uint32_t count;                           /* 2..SU_INPUT_MAX_CURVE_POINTS */
    int32_t x[SU_INPUT_MAX_CURVE_POINTS];
    int32_t y[SU_INPUT_MAX_CURVE_POINTS];
} SuInputCurve;

typedef struct SuInputMap {
    uint32_t axis_count;                      /* axes the record carries, 0..SU_INPUT_MAX_AXES */
    uint32_t button_count;                    /* 0..SU_INPUT_MAX_BUTTONS */
    uint32_t binding_count;
    uint32_t curve_count;
    char axis_names[SU_INPUT_MAX_AXES][SU_INPUT_NAME_MAX];
    char button_names[SU_INPUT_MAX_BUTTONS][SU_INPUT_NAME_MAX];
    SuInputBinding bindings[SU_INPUT_MAX_BINDINGS];
    SuInputCurve curves[SU_INPUT_MAX_CURVES]; /* curves[k - 1] is curve k */
} SuInputMap;

/* A binding with the defaults: scale ONE, dead zone 0, saturation ONE, linear, threshold ONE / 2, counts 1. */
SU_INPUT_API SuInputBinding su_input_binding(uint32_t source, uint32_t code, uint32_t device, uint32_t target);

/* Checks every rule above. On failure *bad (optional) receives the index of the offending binding, or
 * SU_INPUT_MAX_BINDINGS + curve index for a curve, or UINT32_MAX for the counts. SU_E_INVALID_ARGUMENT. */
SU_INPUT_API uint32_t su_input_map_validate(const SuInputMap *map, uint32_t *bad);

/* Reads a map from JSON (format "sutekh.input_map", version 1). Fractions are written in thousandths as
 * integers (dead_zone_milli: 150 is 0.150) and converted exactly to Q16.16 (round half away from zero).
 *   {"format": "sutekh.input_map", "version": 1,
 *    "axes": ["pitch", "yaw"], "buttons": ["boost"],
 *    "curves": [{"name": "soft", "points_milli": [[0, 0], [500, 250], [1000, 1000]]}],
 *    "bindings": [
 *      {"target": "pitch", "source": "pad_stick", "pad": 0, "stick": "ly", "dead_zone_milli": 150,
 *       "saturation_milli": 950, "curve": "soft", "scale_milli": -1000, "axial": false},
 *      {"target": "pitch", "source": "key", "key": "S", "scale_milli": 1000},
 *      {"target": "yaw", "source": "mouse_stick", "axis": "x", "counts": 400, "recenter_milli": 20},
 *      {"target": "boost", "source": "pad_trigger", "pad": 0, "trigger": "right", "threshold_milli": 500},
 *      {"target": "boost", "source": "pad_button", "pad": 0, "button": "A"},
 *      {"target": "boost", "source": "mouse_button", "button": "left"},
 *      {"target": "yaw", "source": "mouse_delta", "axis": "wheel", "counts": 120}]}
 * Unknown members, names or values are SU_E_INVALID_ARGUMENT or SU_E_TYPE; malformed JSON is SU_E_PARSE.
 * message (optional, 160 bytes) receives a plain-language reason. The result is validated. */
SU_INPUT_API uint32_t su_input_map_from_json(const uint8_t *json, size_t len, SuInputMap *map, char message[160]);

/* Mapper: a map plus the per-binding state of mouse sticks. No allocation. */
typedef struct SuInputMapper {
    SuInputMap map;
    int32_t stick[SU_INPUT_MAX_BINDINGS];     /* mouse stick positions, Q16.16 */
    uint64_t ticks;                           /* records produced */
} SuInputMapper;

/* Fixed-layout record of one tick. */
typedef struct SuInputRecord {
    uint64_t tick;
    uint32_t buttons;                         /* bit i: button i */
    uint32_t axis_count;
    int32_t axes[SU_INPUT_MAX_AXES];          /* Q16.16; entries >= axis_count are 0 */
} SuInputRecord;

/* Copies and validates the map; mouse sticks start at 0. */
SU_INPUT_API uint32_t su_input_mapper_init(SuInputMapper *m, const SuInputMap *map);
/* Recenters every mouse stick (for example when the player releases control). */
SU_INPUT_API void su_input_mapper_reset(SuInputMapper *m);
/* Maps one tick's device state (one sample, or one part of su_input_state_split) to the record of tick. */
SU_INPUT_API uint32_t su_input_mapper_tick(SuInputMapper *m, const SuInputState *s, uint64_t tick, SuInputRecord *out);

/* Canonical bytes of a record (92 bytes): "SUINREC" 0x00 | u32 version 1 | u64 tick | u32 buttons |
 * u32 axis_count | 16 x i32 axes, little-endian. Decoding is strict (SU_E_PARSE).
 * 2026-10-18: the size was given as 88, four bytes short of this layout, so encode wrote past an 88-byte
 * buffer and decode read past its length check (found by an MSVC /RTC1 run and AddressSanitizer). */
#define SU_INPUT_RECORD_BYTES 92u
SU_INPUT_API void su_input_record_encode(const SuInputRecord *r, uint8_t out[SU_INPUT_RECORD_BYTES]);
SU_INPUT_API uint32_t su_input_record_decode(const uint8_t *buf, size_t len, SuInputRecord *r);

/* Where a record goes in the world: the field that receives the button bits, and one field per axis
 * (SU_INPUT_NO_FIELD skips it). */
#define SU_INPUT_NO_FIELD UINT32_MAX
typedef struct SuInputFields {
    uint32_t buttons;
    uint32_t axes[SU_INPUT_MAX_AXES];
} SuInputFields;

/* Writes for su_host_input_push: one per mapped field, value = the button bits or the axis as a two's-complement
 * u32. out holds 1 + SU_INPUT_MAX_AXES entries; *count receives the number written. Duplicate field ids are
 * SU_E_INVALID_ARGUMENT. */
SU_INPUT_API uint32_t su_input_record_writes(const SuInputRecord *r, const SuInputFields *fields, uint64_t entity,
                                            SuHostInput *out, uint32_t *count);

/* ---- portable device events (the Linux window backend, sutekh/window.h; tests; hosts with their own loop) ----
 *
 * A platform layer that receives discrete device events translates them to SuInputEvent and applies them to a
 * live SuInputState; su_input_state_take then yields one sample, exactly as su_input_win32_sample does. The
 * result has the Win32 conventions, so the same physical input gives the same state, and therefore the same
 * records, on every platform: keys are converted from USB HID usages to scan code set 1 positions, gamepads use
 * the XInput button bits and ranges (+y up, triggers 0..255), mouse motion is in counts (+y down), the wheel in
 * 120ths of a notch. Integer only; no allocation. */

#define SU_INPUT_EV_KEY 1u           /* code: USB HID keyboard usage (page 0x07); value: 1 down, 0 up */
#define SU_INPUT_EV_MOUSE_MOTION 2u  /* x, y: relative counts (+y down) */
#define SU_INPUT_EV_MOUSE_BUTTON 3u  /* code: one SU_INPUT_MOUSE_* bit; value: 1 down, 0 up */
#define SU_INPUT_EV_WHEEL 4u         /* value: 120 per notch, + away from the user */
#define SU_INPUT_EV_PAD_CONNECT 5u   /* device: pad slot; value: 1 connected (all inputs at rest), 0 removed */
#define SU_INPUT_EV_PAD_BUTTON 6u    /* device: pad slot; code: one SU_PAD_* bit; value: 1 down, 0 up */
#define SU_INPUT_EV_PAD_AXIS 7u      /* device: pad slot; code: SU_INPUT_PAD_AXIS_*; value: sticks -32768..32767
                                        (+y up), triggers 0..255; out-of-range values are clamped */
#define SU_INPUT_EV_FOCUS 8u         /* value: 1 focused; 0 releases every key and button (SU_INPUT_FLAG_FOCUS_LOST) */

#define SU_INPUT_PAD_AXIS_LX 0u
#define SU_INPUT_PAD_AXIS_LY 1u
#define SU_INPUT_PAD_AXIS_RX 2u
#define SU_INPUT_PAD_AXIS_RY 3u
#define SU_INPUT_PAD_AXIS_LT 4u
#define SU_INPUT_PAD_AXIS_RT 5u

typedef struct SuInputEvent {
    uint32_t type;      /* SU_INPUT_EV_* */
    uint32_t code;
    uint32_t device;    /* pad slot, 0..SU_INPUT_MAX_PADS - 1, for pad events; else 0 */
    int32_t value;
    int32_t x, y;       /* mouse motion */
    uint64_t time_ns;   /* monotonic time of the event (key_ns, mouse_ns, changed_ns) */
} SuInputEvent;

/* Scan code set 1 position (SU_KEY_*) of a USB HID keyboard usage, or 0 for a usage without one (Pause, which
 * Raw Input does not report either, media and reserved usages). The ISO key between left shift and Z (usage
 * 0x64) is 0x56; the ISO key left of Enter (0x32) shares SU_KEY_BACKSLASH, as on Windows. */
SU_INPUT_API uint32_t su_input_key_from_hid(uint32_t usage);

/* Applies one event to a live state. Mouse motion and wheel are accumulated (saturating at the i32 range). Keys
 * without a set 1 position are ignored (SU_OK). Pad events for a disconnected slot other than CONNECT are
 * ignored. SU_E_INVALID_ARGUMENT for an unknown type, a pad slot >= SU_INPUT_MAX_PADS, or a code that is not
 * one defined bit or axis; the state is then unchanged. */
SU_INPUT_API uint32_t su_input_state_apply(SuInputState *live, const SuInputEvent *e);

/* One sample: copies live to out with sample_ns = now_ns, then clears the relative quantities (motion, wheel)
 * and the per-sample flags (SU_INPUT_FLAG_FOCUS_LOST) of live. */
SU_INPUT_API void su_input_state_take(SuInputState *live, uint64_t now_ns, SuInputState *out);

/* ---- Windows: Raw Input (keyboard, mouse) and XInput (gamepads) ---- */
#if defined(_WIN32)

typedef struct SuInputWin32 SuInputWin32;

#define SU_INPUT_WIN32_BACKGROUND 0x1u  /* receive input while not in the foreground (RIDEV_INPUTSINK) */

/* hwnd: the game window (HWND) that receives WM_INPUT; forward its messages to su_input_win32_message. NULL
 * creates a hidden message-only window owned by the context (then call su_input_win32_pump), and implies
 * SU_INPUT_WIN32_BACKGROUND. XInput is loaded at run time (xinput1_4.dll, else xinput9_1_0.dll); without it
 * the context works and reports SU_INPUT_FLAG_NO_XINPUT. */
SU_INPUT_API uint32_t su_input_win32_create(void *hwnd, uint32_t flags, SuInputWin32 **out);
SU_INPUT_API void su_input_win32_destroy(SuInputWin32 *c);
/* For the game window's procedure: handles WM_INPUT, WM_SETFOCUS, WM_KILLFOCUS and WM_ACTIVATEAPP. Returns 1
 * when the message was WM_INPUT (the caller then still calls DefWindowProc, as Raw Input requires). */
SU_INPUT_API int su_input_win32_message(SuInputWin32 *c, void *hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);
/* Processes pending messages of the context's own hidden window (NULL hwnd at create); su_input_win32_sample
 * also does this. */
SU_INPUT_API void su_input_win32_pump(SuInputWin32 *c);
/* Losing focus releases every key and button (no stuck keys) and sets SU_INPUT_FLAG_FOCUS_LOST. */
SU_INPUT_API void su_input_win32_set_focus(SuInputWin32 *c, int focused);
/* Polls XInput, copies the state with sample_ns = now, then clears the relative quantities and the per-sample
 * flags. Disconnected gamepad slots are polled at most once per second (XInput is slow on empty slots). */
SU_INPUT_API uint32_t su_input_win32_sample(SuInputWin32 *c, SuInputState *out);
/* Applies one RAWINPUT structure (the bytes GetRawInputData returns) as if it had arrived; for tests and for
 * hosts that read Raw Input themselves. SU_E_INVALID_ARGUMENT for a truncated or unknown structure. */
SU_INPUT_API uint32_t su_input_win32_feed_raw(SuInputWin32 *c, const void *rawinput, size_t len, uint64_t now_ns);
/* Test seam: replaces XInputGetState (the XInput signature: DWORD (*)(DWORD, XINPUT_STATE *)); NULL restores the
 * loaded library. */
SU_INPUT_API void su_input_win32_set_xinput(SuInputWin32 *c, unsigned long (*get_state)(unsigned long, void *));

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif
