# Issue 138: Linux windowed presentation and native input

## State and scope

Locally verified on `feat/issue-138-linux-window`, based on
`07d06de` (origin/main, merge of PR 129). CI and integration pending; not merged.

The Vulkan swapchain path presented to a Win32 HWND only and native input covered Raw Input and XInput only, so
on Linux the renderer ran offscreen. Now:

- `include/sutekh/window.h` (new; library `su_window`, Linux and other non-Windows builds): a Wayland or X11
  window through SDL 3, opened at run time with `dlopen` (`libSDL3.so.0`, 3.2.0 or later;
  `SUTEKH_SDL3_LIBRARY` overrides). Only the `wayland` and `x11` video drivers are allowed. `su_window_poll`
  reports close, size (pixels; 0 x 0 while minimized, hidden or occluded, as Win32's WM_SIZE does), focus and
  connected pads; `su_window_sample_input` gives an `SuInputState` with the Win32 conventions; relative mouse
  mode, title, size/minimize/restore requests, info.
- `include/sutekh/render.h` API 12: `SuRenderDesc.window_system`, `window_display`, `window_surface`,
  `window_id` (`SU_RENDER_WINDOW_WAYLAND`, `SU_RENDER_WINDOW_XLIB`). The renderer enables `VK_KHR_surface` plus
  the matching WSI extension and creates the surface itself; SDL never touches Vulkan objects. No Wayland or X11
  headers are needed (local Xlib typedefs, in `su_render_vk.c` only). API 11 descriptors keep working
  (`SU_RENDER_DESC_V2_SIZE`, 80 bytes on LP64, static assertion). `su_render_resize(r, 0, 0)` now pauses
  explicitly: frame calls return `SU_E_RESOURCE_LIMIT` without acquiring an image until a non-zero size arrives
  (needed because an X11 surface keeps its extent while iconified, and Wayland surfaces report none). With a
  surface that reports no extent, the host size is clamped to the surface limits.
- `include/sutekh/input.h` API 2: portable device events (`SuInputEvent`, `su_input_state_apply`,
  `su_input_state_take`, `su_input_key_from_hid`) in the portable part of `su_input`. USB HID usages map to scan
  code set 1 positions; gamepads use XInput bits and ranges; Win32 code is unchanged.
- `examples/presentation/space_demo.c` builds and runs on Linux: windowed (Wayland, or X11 with
  `SDL_VIDEO_DRIVER=x11`), offscreen and headless. Platform shims keep the Win32 path as it was. Linux grabs the
  mouse (relative mode) by default for the mouse-stick camera; `--no-mouse-grab` turns that off. Without a
  display or SDL 3 it prints the reason and exits 3, as with no Vulkan. While a frame is paused (minimized) the
  loop sleeps 1 ms (on Windows too) instead of spinning.
- Dependency decision recorded in `docs/BACKENDS_AND_DEPENDENCIES.md` section 8 and the ledger: SDL 3 opened at
  run time, pinned 3.2 headers fetched by `tools/bootstrap_sdl3_headers.py` (sparse clone of `include/` and
  `LICENSE.txt` at `release-3.2.30`, commit `f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc`; lock
  `third_party/sdl3-headers.lock.json`), with footprint and alternatives (native wayland-client/xcb + evdev or
  libinput, GLFW, vendoring SDL, SDL's own Vulkan surface helper).

Exact modified paths:

- `.github/workflows/repository-checks.yml` (both Linux jobs fetch the SDL3 headers; the GCC 16.2 container job
  also installs `libsdl3-0 xvfb xauth`, so the window tests run under Xvfb on lavapipe)
- `.gitignore`
- `README.md`
- `THIRD_PARTY_NOTICES.md`
- `cmake/SutekhPresentation.cmake`
- `docs/BACKENDS_AND_DEPENDENCIES.md`
- `docs/IMPLEMENTATION_STATUS.md`
- `docs/RENDERING_AND_ASSETS.md`
- `examples/presentation/space_demo.c`
- `include/sutekh/input.h`
- `include/sutekh/render.h`
- `include/sutekh/window.h` (new)
- `src/input/su_input_events.c` (new)
- `src/platform/su_window_sdl3_int.h` (new)
- `src/platform/window_sdl3.c` (new)
- `src/render/su_render_vk.c`
- `src/render/su_render_vk_int.h`
- `tests/input/test_input_events.c` (new)
- `tests/window/test_window_events.c` (new)
- `tests/window/test_window_swapchain.c` (new)
- `third_party/sdl3-headers.lock.json` (new)
- `tools/bootstrap_sdl3_headers.py` (new)
- `planning/evidence/linux-window/` (new: `runs.log`, `space-demo-wayland.png`, `space-demo-offscreen-linux.png`)
- `planning/handoffs/2026-10-26-linux-window.md`, `planning/handoffs/2026-10-26-linux-window.state.json`
- `PACK_VALIDATION.json` (regenerated)

New tests (CMake names): `input_events` (always), `window_events` (with the SDL3 headers; needs no library or
display), `window_swapchain` (session display; skip 77 with the reason without display, SDL 3 or a presenting
Vulkan device), `window_no_display` (removes `WAYLAND_DISPLAY`, `DISPLAY`, `XDG_RUNTIME_DIR`, `SDL_VIDEO_DRIVER`;
structured failure, refused bad descriptors, offscreen still renders, also with an API 11 sized descriptor),
`window_swapchain_xvfb` (registered when `xvfb-run` exists), and on Linux the demo tests that were Windows-only:
`demo_headless_rollback`, `demo_offscreen_rollback` (same pinned digest as Windows) and `demo_window_exercise`
(skip on exit 3).

## Local validation

Machine: Intel Core i7-14700K, Intel Arc A770 (DG2) and the CPU's integrated GPU, Nobara Linux 44 (Fedora 44
base, kernel 7.2.9), KDE Plasma Wayland session (XWayland at `:0`), Mesa 26.2.4, Vulkan loader 1.4.341, system SDL
3.4.16, GCC 16.2.1, Clang 22.1.8, CMake 4.3.0, Ninja. The Khronos validation layer is not installed here, so every
local Vulkan run is without validation (`validation not available`); CI's runtime job requires it.

```sh
python3 tools/bootstrap_vulkan_headers.py      # rewrote the lock's license_sha256 (LF checkout); reverted, not committed
python3 tools/bootstrap_asset_deps.py
python3 -I tools/bootstrap_sdl3_headers.py && python3 -I tools/bootstrap_sdl3_headers.py --check
cmake -S . -B /tmp/wt-138-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON -DSUTEKH_ASSET_IMPORT=ON
cmake --build /tmp/wt-138-build -j 6 && ctest --test-dir /tmp/wt-138-build -j 6
cmake -S . -B /tmp/wt-138-build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=ON -DSUTEKH_ASSET_IMPORT=ON
cmake --build /tmp/wt-138-build-rel -j 6 && ctest --test-dir /tmp/wt-138-build-rel -j 6
```

| Check | Result |
|---|---|
| Baseline, origin/main `07d06de`, Debug | 137 tests, 0 failures, 1 skip (`audio_device`) |
| Debug GCC 16.2 `-Wall -Wextra -pedantic -Werror` | no warnings; 145 tests, 0 failures, 1 skip (`audio_device`, as before) |
| Release GCC 16.2 `-Werror` | no warnings; 145 tests, 0 failures, 1 skip (`audio_device`) |
| Headless: `-DSUTEKH_RENDER=OFF -DSUTEKH_WINDOW=OFF` | builds; 127 tests, 0 failures, 1 skip; no `su_window`, no demo |
| `-DSUTEKH_RENDER=ON -DSUTEKH_WINDOW=OFF` | demo builds without a window backend; windowed request exits 3 with the reason |
| `ldd` / `nm -u` of `space_demo` | no SDL, Vulkan, Wayland or X11 library linked or symbol imported |
| Clang 22.1.8 `-fsyntax-only -Werror -pedantic` on every new or changed C file | clean |
| mingw-w64 GCC `-fsyntax-only -Werror -pedantic` (Windows branches of `space_demo.c`, `su_render_vk.c`, `su_input_events.c`, `input_win32.c`) | clean (this is not MSVC and not a Windows run) |
| Clang ASan + UBSan (`detect_leaks=0`): `test_input_events`, `test_window_events`, `test_input`, `test_window_swapchain` (both modes, Wayland), `space_demo --script --frames 200 --exercise-window` | no reports; all pass |
| `test_window_swapchain`, Wayland, Arc A770 | 120 frames FIFO at the 165 Hz display; resize 640x360 to 800x500 recreates the swapchain; zero-size pause refuses frames and resumes; minimize reported (KDE suspends the window: occluded); restore not attempted (no Wayland unminimize) |
| same, `SDL_VIDEO_DRIVER=x11` (XWayland), Arc A770 | resize, minimize reported, restore presents again |
| same under `xvfb-run` (no DRI3) | Mesa's hardware drivers cannot present; llvmpipe presents (FIFO); resize and restore pass; minimize not reported (no window manager) |
| `space_demo --seconds 6 --no-mouse-grab`, Wayland, devices | about 170 fps (FIFO), 120 ticks/s, 0 dropped ticks, 1026 frames drawn, 0 paused, 0 failed, GPU median 0.810 ms p95 0.894 ms, render CPU median 0.788 ms p95 3.879 ms (observations, not a benchmark); screenshot `space-demo-wayland.png` |
| `space_demo --script --frames 400 --exercise-window --rollback-check`, Wayland and XWayland | 0 failed frames, 0 rollback mismatches; swapchain recreations 2 (Wayland, minimize skipped) and 4 (XWayland, 29 frames paused while minimized) |
| `space_demo --offscreen ... --expect-digest fa02b76e...defd` | Linux reaches the Windows-pinned digest; shadow world 0 differences |
| CI image `gcc:16.2` (Debian 13.7) under podman, Release, `ctest -R "window\|input\|demo_\|render_offscreen"` | 10 tests: 8 passed, `window_swapchain` and `demo_window_exercise` skipped (no display); `window_swapchain_xvfb` passed with SDL **3.2.10** (the minimum-version series) on llvmpipe 19.1.7 |
| `tools/check_repository_license.py`; `tools/validate_pack.py --check-report`; `git diff --check` | passed |

Logs: `planning/evidence/linux-window/runs.log` (commands, machine, outputs).

## Untested and unrun

- Native Windows and MSVC: not built here (mingw syntax check only). The Win32 paths changed only by
  restructuring the demo into shims, the renderer's descriptor handling and its explicit pause on
  `su_render_resize(0, 0)`; the Windows CI jobs are the first real check.
- Khronos validation layer: not installed locally; no window or swapchain run here had validation. CI's runtime
  job requires it, but that job has no display or SDL 3, so its window tests skip.
- Real gamepad input: no controller was connected; the SDL gamepad path is tested with synthetic SDL events only.
  Steam Deck and Steam Input: not run.
- Real keyboard and mouse input in the windowed demo: no human input was given. Relative mouse mode was only
  enabled (`space_demo --seconds 2` on Wayland: accepted, 358 frames, 0 failed); no motion was produced under it.
- Fractional scaling (pixel density other than 1) and fullscreen (`SU_WINDOW_FULLSCREEN`): not run.
- X11 on a real Xorg server (not XWayland, not Xvfb): not run.
- GitHub Actions: not waited for.

## Limitations

- Wayland has no client request to unminimize; a minimized window stays minimized until the user restores it.
  KDE reports it as suspended (SDL: occluded), which pauses presentation; other compositors may report nothing.
  `space_demo --exercise-window` skips minimize/restore on Wayland.
- One window per process (SDL video is process-global).
- With relative mouse mode off, Linux motion is pointer motion in pixels and `SU_INPUT_FLAG_MOUSE_ABSOLUTE` is set
  (Raw Input reports device counts); records match Windows only with relative mode on.
- Pads beyond four are ignored; buttons XInput lacks (guide, paddles, touchpad) are not reported.
- If `WAYLAND_DISPLAY` and `DISPLAY` are unset but `$XDG_RUNTIME_DIR/wayland-0` exists, libwayland still
  connects to it (standard behaviour); "no display" means no reachable compositor or X server.
- Linux audio output is still the no-device backend (the demo mixes offline).
- Observation, not changed here: the header names `SU_KEY_NUMLOCK` 0x45, and Linux maps NumLock to 0x45; Raw
  Input is commonly documented to report NumLock with the E0 flag (0x145). Not checked on Windows.

## Next

Review and CI; then a Windows-native run of the demo (window, resize, minimize) to confirm the restructured Win32
path; a gamepad and Steam Deck run; a decision on shipping or locating `libSDL3.so.0` for release builds.
