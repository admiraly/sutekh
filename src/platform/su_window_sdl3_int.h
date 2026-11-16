/* Internal seam of the SDL3 window backend (window_sdl3.c): the translation of SDL events into portable input
 * events (sutekh/input.h). It needs only the SDL headers, not the library, so tests can feed it synthetic SDL
 * events without a display. Not a public header. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SU_WINDOW_SDL3_INT_H
#define SU_WINDOW_SDL3_INT_H

#include <SDL3/SDL_events.h>

#include "sutekh/input.h"

/* Translation state: pad slots by joystick instance id, the fractional parts of mouse motion and wheel, and the
 * last reported absolute pointer position in window coordinates. */
typedef struct SuSdlTranslate {
    SDL_JoystickID pad_id[SU_INPUT_MAX_PADS]; /* 0: free slot */
    double rem_x, rem_y, rem_wheel;
    int32_t last_pointer_x, last_pointer_y;
    int have_pointer;
    int window_width, window_height; /* logical bounds; zero when unknown in standalone translation */
} SuSdlTranslate;

#define SU_SDL_MAX_EVENTS_PER_SDL_EVENT 3u

/* Translates one SDL event into at most SU_SDL_MAX_EVENTS_PER_SDL_EVENT portable events stamped now_ns; returns
 * how many (0 for an event without input meaning). SDL_EVENT_MOUSE_MOTION always emits SU_INPUT_EV_MOUSE_POS
 * (rounded/clamped window coordinates, value = 1 inside) and, when the truncated motion delta is non-zero, also emits
 * SU_INPUT_EV_MOUSE_MOTION. SDL_EVENT_WINDOW_MOUSE_ENTER/LEAVE emit SU_INPUT_EV_MOUSE_POS (value = 1 or 0); ENTER
 * without a known position emits nothing. Motion outside known window bounds emits value = 0.
 * GAMEPAD_ADDED takes the first free slot (none free: ignored) and emits PAD_CONNECT; GAMEPAD_REMOVED frees it.
 * *slot (optional) receives the slot of an ADDED/REMOVED event, or UINT32_MAX. Integer conversions: sticks keep
 * SDL's value with y negated to +up (clamped to 32767), triggers are value >> 7 (0..32767 to 0..255), motion and
 * wheel (x 120, physical direction) are truncated toward zero and carry their fractions to the next event, so no
 * motion is lost or invented. */
uint32_t su_window_sdl3_translate(SuSdlTranslate *t, const SDL_Event *e, uint64_t now_ns, SuInputEvent *out,
                                  uint32_t *slot);

#endif
