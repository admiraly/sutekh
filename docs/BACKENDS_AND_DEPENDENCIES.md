# Backends and dependency decisions

## 1. Minimize new research on the critical path

The project's novelty should be the development/runtime contract, not simultaneously reinventing JSON, instruction encoding, a linker, a shader compiler, an operating-system sandbox, and a complete programming language. Reuse small pinned tools behind narrow adapters. A dependency compiled once is different from a dependency rebuilt on every gameplay edit.

M0 requires C17, native platform support, the build toolchain, a strict JSON library, and a reviewed hashing implementation. Python standard-library tooling is outside the hot runtime. The native JSON default is yyjson; its own documentation describes the library and APIs [S10]. Review its parser configuration and enforce duplicate-key/semantic policy at our boundary rather than assuming all defaults meet SU-LIR requirements.

## 2. Native backend decision

M1's default is **AsmJit in a narrow C++ translation-unit boundary exposing a C ABI**. It already provides machine-code emission and optional register allocation [S01]. The rest of the core remains C17; AsmJit headers do not spread through `include/sutekh/`.

Initially lower only SU-LIR 0.1. Implement a scalar backend before AVX2. Use an explicit stack/register plan rather than a general optimizer. Record source mappings from IR instruction indices to native ranges and preserve a disassembly inspection path. Do not hand-encode VEX/EVEX instructions to satisfy an “assembly purity” metric.

If a concrete platform/tooling issue blocks AsmJit, record an architecture decision with evidence and an alternative. Cranelift can emit callable code into memory and is an available reference or future optimizing backend [S02], but M1 must not grow two backends merely because both are interesting. A Rust dependency in a prebuilt optional tool is not the same as imposing Rust compilation on all game edits.

## 3. Handwritten assembly

Raw x86-64 kernels are optional trusted native components. Every kernel has an ABI, read/write/alignment contract, scalar reference, zero/tail/bounds tests, differential corpus, and a measured justification. The assembly lane uses a pinned assembler when source changes; normal Live IR does not spawn it.

Avoid handwritten replacements for vetted security primitives or general memory routines without a demonstrated bottleneck and expert-quality test coverage. Assembly is not a substitute for an algorithmic or layout fix.

## 4. Native memory and safety

Generated code pages follow writable-then-executable publication, with no permanent RWX mapping. Handle allocation/protection failures explicitly and preserve the previous implementation. Apply platform cache synchronization rules [S03]. Debug/unwind registration, return-address handling, and ABI tests are part of backend completion.

Checked code generation is still a trusted compiler implementation, not a complete malicious native-code verifier. Admit unreviewed native candidates only in isolated workers. A later optional WebAssembly sandbox can be evaluated when mod/plugin requirements justify it; it is not imposed as an extra M0 runtime.

## 5. Shader/backend tooling

Use an established shader compiler through an adapter when rendering begins. Cache outputs by source, includes, options, target capabilities, compiler version, and resource layout. Do not begin with a custom SPIR-V compiler.

The driver can still compile pipeline/shader work internally, and pipeline creation may be expensive [S11]. Schedule it off the render-critical path and keep a compatible old pipeline or safe fallback until readiness. Native CPU code generation does not remove GPU compilation costs.

## 6. Toolchain locking

Bootstrap records exact versions/commits, download origins, checksums, license texts, patches, target triples, compiler/linker flags, and cache key inputs. Unsupported platform configurations are reported rather than silently emulated through undefined ABI assumptions.

Do not fetch dependencies at runtime in a shipped game. Development builds can operate offline after approved dependencies are present. A new dependency must identify its problem, footprint/build cost, update policy, and simpler alternatives. No arbitrary “zero dependencies” rule should force years of unnecessary reimplementation.

## 7. Decision ledger

| Decision | Status | Revisit trigger |
|---|---|---|
| C17 resident core | Selected | Measured concrete blocker, not fashion |
| JSON IR before custom syntax | Selected for M0 | M0 complete; author ergonomics measured |
| Reference VM before JIT | Selected | Never remove semantic oracle |
| AsmJit C-ABI adapter | Selected for M1 | Documented unsupported need/performance evidence |
| No CPU/GPU autotuner in M0 | Selected | Representative GPU workloads and cost model exist |
| Static SoA before auto-layout | Selected | Real access traces and migration tests exist |
| AsmJit pin (M1) | Candidate: commit `0d7f01052f2d3f6d6b9bb6714ef32bd7b80177eb` (v1.23.0, zlib, C++20); not yet adopted | Owner approval of fetch-vs-vendor; M1-01 W^X and differential evidence |
| Engine license | PolyForm Perimeter 1.0.1, selected by owner (licence card, 12 October 2026); `LICENSE` is authoritative | Third-party components retain their own terms |
| Repository / final name | Public `admiraly/sutekh`; final branding pending | Before commercial branding |
| Linux window, surface and input (issue 138) | Selected (26 October 2026): SDL 3 (zlib) opened at run time by `su_window`; pinned 3.2 headers `release-3.2.30` (commit `f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc`); the renderer creates the Wayland/Xlib surface itself. Section 8 | A platform target where SDL 3 is unavailable or measurably adds input latency; Steam Input or gamepad behaviour SDL cannot provide; owner preference for vendoring |
| Non-Windows audio output (issue 174) | Selected (27 October 2026): the same runtime-loaded SDL 3, in `su_audio`, for the default playback device; headers at build time only. Section 8 | A platform whose SDL 3 audio adds too much latency; a target without SDL 3 (then the defined refusal stays) |

## 8. Linux window, surface and input backend (issue 138)

**Problem.** The swapchain path was Win32 only (`VK_KHR_win32_surface`) and native input covered Raw Input and
XInput only, so on Linux the renderer ran offscreen. A downstream game is developed on Linux (KDE Plasma, Wayland)
and also targets the Steam Deck. Needed: a window with a `VkSurfaceKHR` on Wayland and on X11 (also XWayland),
resize/minimize/occlusion/close handled as on Win32, keyboard, mouse (with relative motion for a camera) and
gamepads mapped into `include/sutekh/input.h` so that per-tick records are identical across platforms, graceful
failure without a display, and no windowing dependency for headless builds.

**Decision.** SDL 3 behind a narrow adapter (`include/sutekh/window.h`, `src/platform/window_sdl3.c`):

- *Run-time loading, as for Vulkan.* `su_window` opens `libSDL3.so.0` (3.2.0 or later; `SUTEKH_SDL3_LIBRARY`
  overrides the name) with `dlopen` and resolves 25 entry points; nothing links against SDL. Every pointer type is
  checked at compile time against the header declaration (`_Generic` in a static assertion, unevaluated). Without
  the library, or without a Wayland or X11 display, `su_window_create` returns `SU_E_UNSUPPORTED_METHOD` with a
  reason; offscreen rendering is unaffected. Only the `wayland` and `x11` video drivers are allowed (never a console
  KMS/DRM or dummy driver); `SDL_VIDEO_DRIVER` still overrides.
- *The renderer owns the surface.* SDL provides the native handles (`wl_display`/`wl_surface`, or Xlib
  `Display`/`Window`); the window is created with `SDL_WINDOW_VULKAN`, as SDL requires, so SDL loads the Vulkan
  loader too, but it creates no Vulkan objects. `su_window` stops only the SDL subsystems it started
  (`SDL_QuitSubSystem`), so other SDL use in the host is unaffected. `su_render` creates the surface through `VK_KHR_wayland_surface` or `VK_KHR_xlib_surface`
  (`SuRenderDesc.window_system`, API 12). The renderer does not depend on SDL, and any other windowing layer can
  supply the same handles. The WSI headers need no Wayland or X11 headers (local Xlib typedefs in one translation
  unit).
- *Input keeps one model.* SDL events become portable device events (`SuInputEvent`, `su_input_state_apply`, in
  the portable part of `su_input`): USB HID scancodes to scan code set 1 positions, SDL gamepads to XInput bits and
  ranges (stick y negated to +up, triggers `>> 7`), mouse motion and wheel with carried fractions. The resulting
  `SuInputState` has the Win32 conventions, so the mapper produces the same records; tested with synthetic events
  through the shipped default map.
- *Build.* `tools/bootstrap_sdl3_headers.py` fetches the headers only (sparse checkout of `include/` and
  `LICENSE.txt`, 3.6 MB of headers), verifies commit and licence, and writes
  `third_party/sdl3-headers.lock.json`; not redistributed. The 3.2 headers are those of the oldest supported
  runtime, so the build cannot use a function only newer runtimes provide (SDL keeps ABI compatibility within 3.x).
  `SUTEKH_WINDOW=AUTO|ON|OFF`; on Windows the library is not built.

**Footprint (measured on the integration machine, Release GCC 16.2).** One translation unit; `libsu_window.a`
25.7 KB (10.7 KB text). No link-time dependency; headless and `SUTEKH_WINDOW=OFF` builds do not compile it, and a
process that never creates a window never loads SDL. At run time: the system's `libSDL3.so.0` (Fedora 44 SDL 3.4.16:
3.4 MB), or one a game ships and names with `SUTEKH_SDL3_LIBRARY` (whether a given Steam Linux Runtime provides
SDL 3 was not checked here). Update policy: bump the pinned headers only to
another 3.2.x tag (or raise the minimum runtime deliberately); the lock file records the commit and licence hash.

**Alternatives considered.**

- *Native wayland-client + xdg-shell (+ xdg-decoration, relative-pointer, pointer-constraints, libxkbcommon), xcb
  or Xlib for X11, evdev or libinput for gamepads.* No new third-party library, but several protocol
  implementations to write and maintain (Wayland code generated by wayland-scanner from the protocol XML,
  client-side decorations on compositors without server-side ones, keyboard focus and repeat, X11 ICCCM/EWMH
  details), gamepad mapping databases (button layouts differ per controller; SDL ships one), hot-plug through
  udev, and device-node permissions (libinput reads keyboard and mouse device nodes a desktop session does not open
  to applications; raw evdev gives gamepads but no mapping database). This is the larger and less tested path for
  the same result.
- *GLFW 3.4.* Window and Vulkan surface on Wayland and X11, and gamepads through a mapping database over the
  joystick devices, but no HIDAPI controller drivers (SDL has one for the Steam Deck's built-in controls) and no
  rumble; a second library would likely be needed for gamepads later.
- *Vendoring and building SDL from source.* Reproducible to the byte but adds minutes to every clean build and a
  long list of optional system development packages to get the Wayland and X11 backends; run-time loading of the
  platform's SDL 3 gives the same API with no build cost. Revisit if a target ships without SDL 3.
- *SDL's own Vulkan helpers* (`SDL_Vulkan_CreateSurface`). Rejected so that `su_render` keeps owning instance
  extensions and the surface, and does not depend on SDL.

Known limits: Wayland has no client request to unminimize a window (a minimized window stays minimized until the
user restores it; the compositor may report it as occluded, which pauses presentation); X11 iconify needs a window
manager (none under bare Xvfb).

**Audio output (issue 174) reuses this decision.** `src/audio/su_audio_sdl3.c` opens the default playback device
through the same runtime-loaded SDL 3 (`libSDL3.so.0` or `SUTEKH_SDL3_LIBRARY`, the pinned 3.2 headers at build
time) and the same one-discovery-of-the-headers rule, so a non-Windows build with the headers gets both backends
and nothing links against SDL. The loader is a separate copy in `su_audio`, because audio must work with
`SUTEKH_WINDOW=OFF` and in a host that never creates a window; the two share the process's loaded library and SDL
subsystem reference counts, and each stops only the subsystem it started. SDL owns the audio thread and calls the
backend once per device period (`SDL_OpenAudioDeviceStream`), which replaces the WASAPI render thread. The endpoint
name in `SuAudioDeviceInfo` carries SDL's driver in parentheses. Footprint: one translation unit, `libsu_audio.a`
grows by it; a process that opens no device never loads SDL. The device timing and underrun mapping is in
`docs/AUDIO_DEVICE_LATENCY.md`.

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
