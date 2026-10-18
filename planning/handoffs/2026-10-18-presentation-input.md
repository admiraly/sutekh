# Handoff: host events, rollback-grade state operations, native input, first presentation slice

Branch `feat/presentation-input`, based on `main` at `641b450`. Authorization and scope: the extension section
of `planning/amendments/2026-10-18-downstream-host-work.md` (owner-authorized downstream work; no M3 gate is
closed by it). Machine and toolchain as in `2026-10-18-windows-native-host.md` (i5-12400F, 16 GB, Windows 11 Pro
26200, clang 23.1.3, MSVC 14.44.35207 libraries, SDK 10.0.26100.0), plus an NVIDIA Quadro P600 2 GB; the user
connects over RDP.

This file is updated as each item lands.

## 1. Host events and rollback-grade state operations

New in `include/sutekh/host.h` (additive; `SU_HOST_API_VERSION` stays 1, nothing existing changed):

| Function | What it does |
|---|---|
| `su_host_event_next_seq` | sequence number the next event will get (the caught-up cursor) |
| `su_host_events_read` | up to `max` events with `event_seq >= cursor`, oldest first; next cursor; how many were evicted before the cursor could read them |
| `su_host_events_for_tick` | retained events whose `tick` equals a tick, in sequence order; total matching; whether the tick is complete (nothing of it can have been evicted) |
| `su_host_snapshot_restore_inplace` | `su_host_snapshot_restore` into the existing storage: full validation, then an all-or-nothing write; first call allocates a scratch entity table and event log, later calls allocate nothing |
| `su_host_digest_fast` | **non-canonical** XXH64 (seed 0) of the canonical snapshot bytes; `SU_HOST_FAST_DIGEST_VERSION` 1; `su_host_digest` (SHA-256) stays the reference |

`SuHostEvent` has the layout of the engine's `SuEvent` (40 bytes, static asserts). Engine kinds are mirrored as
`SU_HOST_EVENT_SPAWNED`, `_DESPAWNED`, `_REJECTED` and `SU_HOST_EVENT_SYSTEM_HOST`.

Semantics worth knowing:

- Event order is the engine's canonical commit order (`event_seq`): by tick, then system, then slot, then per-row
  emission order; host spawn/despawn records sit where the host made them.
- A system event carries the tick its step published (a step from T to T + 1 emits tick T + 1); a host spawn or
  despawn record carries the committed tick at the call. So `su_host_events_for_tick(T)` returns the systems'
  events of the step that produced T, then any host records made while at T.
- The log is state: restoring a snapshot rewinds it, and re-simulating the same inputs re-emits identical events
  with identical sequence numbers (tested). A cursor past the restored log is `SU_E_INVALID_ARGUMENT`; clamp it to
  `su_host_event_next_seq`.
- The ring holds `event_log_max` events (host default 1024). Readers are told about evictions (`skipped`,
  `complete`); nothing is silently lost.

Engine support, all allocation-free: `su_ents_canonical_parse_into` and `su_ents_swap` (`src/world/entities/
su_ents.c`), `su_evlog_parse_into` and `su_evlog_swap` (`su_cmdq.c`), `su_world_restore_canonical_check` and
`su_world_restore_canonical_commit` (`su_world.c`). The existing allocating parsers now share their header and
record checks with these, so the two paths cannot drift apart. The "no allocation after the first call" claim is
by construction (code reading); no allocation counter was instrumented.

Fast digest: implemented from the published XXH64 description (`src/host/su_xxh64.c`, no third-party code
copied). Reference vectors from python-xxhash 4.0.1 (in a throwaway venv outside the repository) caught a
mistyped constant during development; all 16 vectors pass now.

### Commands and results (item 1)

| Command (from the worktree, after `. .\tools\env-windows.ps1`) | Result |
|---|---|
| `cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang` then `cmake --build build-win` | no warnings, no errors |
| `ctest --test-dir build-win -j 8` (Debug) | 64 passed, 0 failed, 2 skipped (`path_fifo`, `containment`, as before) of 66 |
| `build-win\test_host_rollback.exe` / `test_host_rollback_dll.exe` | 93,906 / 93,887 checks, 0 failures (the DLL build omits the internal XXH64 vectors) |
| same, Release (`build-rel`) | 93,906 checks, 0 failures |
| differential fuzz inside `test_host_rollback` (3,000 corrupted snapshots, both restore paths) | 1,017 accepted, 1,983 rejected, 0 status-code differences; accepted bytes round-trip; every refused restore left the world byte-identical |
| `python -I tools/validate_pack.py --check-report` | passed |
| `build-rel\bench_host.exe 2000` | `planning/evidence/presentation-input/bench-host-rollback-release.json` (below) |

Timing observations (this machine, not qualification; per call, median (p95), microseconds; 2-field demo capsule;
event log 64; source commit `f121fa1`, binary hash and conditions in the evidence file):

| Entities | restore (allocating) | restore in place | digest (SHA-256) | digest_fast (XXH64) | events_for_tick | rollback, 8 ticks stepped one by one | rollback, 8 ticks in one step call |
|---|---|---|---|---|---|---|---|
| 64 | 2.6 (3.1) | 0.5 (0.6) | 11.5 (12.3) | 0.5 (0.6) | 0.7 (0.8) | 9.0 (9.6) | 8.9 (9.6) |
| 1,024 | 10.4 (11.8) | 4.0 (4.4) | 57.9 (63.0) | 2.8 (3.0) | 0.7 (0.8) | 97.0 (107.1) | 96.8 (106.1) |
| 16,384 | 143.4 (428.0) | 60.3 (108.9) | 808.9 (1,137.0) | 37.4 (61.4) | 0.7 (0.9) | 1,544.4 (2,124.5) | 1,521.5 (2,109.3) |

A rollback is "in-place restore, re-simulate 8 ticks with 4 input writes each, fast digest". After this change
the re-simulated ticks dominate it (about 11 us per tick at 1,024 rows on the checked VM). Queuing all eight
records and calling `su_host_step` once gives the same time, so the per-call setup in `su_ent_run_ticks`
(command queue and job pool creation) is not the cost; the per-tick system execution is. Faster rollback beyond
this needs faster ticks (VM or native tier work, owned elsewhere), not more host-API work.

## 2. Native input layer

New header `include/sutekh/input.h`, static library `su_input` (`cmake/SutekhPresentation.cmake`, included with one
line after the host block of `CMakeLists.txt`). Three layers:

1. **Device state** `SuInputState`: 512-bit key set (scan code set 1 make codes, E0 keys at 0x100 + code, so keys
   are physical positions), mouse motion, wheel and buttons accumulated between samples, four gamepads (XInput
   layout), monotonic timestamps (`sample_ns`, `key_ns`, `mouse_ns`, per-pad `changed_ns`), focus, and flags
   (`MOUSE_ABSOLUTE` for remote-desktop style absolute mouse reports, `NO_XINPUT`, `FOCUS_LOST`).
   `su_input_state_split` divides one frame's motion over n ticks exactly (parts sum to the whole, differ by at
   most one count).
2. **Mapping** `SuInputMap` / `SuInputMapper`: up to 64 bindings from keys, mouse buttons, pad buttons, sticks
   (radial or axial dead zone), triggers, mouse motion per tick, and a mouse virtual stick (counts per full
   deflection, recentering per tick) to 16 axes and 32 buttons; saturation, 9-point monotonic curves, scale,
   analog-to-button thresholds. Integer Q16.16 only. Data-driven: `su_input_map_from_json` reads the
   `sutekh.input_map` v1 format (fractions as integer thousandths, converted exactly); example
   `examples/input/default_map.json` (generic 6DOF flight bindings for pad, keyboard and mouse).
3. **Record** `SuInputRecord` (tick, 32 button bits, 16 Q16.16 axes; canonical 88-byte `SUINREC` encoding) and
   `su_input_record_writes`, which turns a record into `SuHostInput` writes (axes as two's-complement u32) for
   `su_host_input_push`. Replays therefore store the mapped record through the existing `SUINPUT`/`SUREPLAY`
   path; raw device events are never recorded.

Windows device layer (`src/platform/input_win32.c`): Raw Input for keyboard and mouse, either on the game's
window (forward messages to `su_input_win32_message`) or on a hidden message-only window with `RIDEV_INPUTSINK`;
XInput loaded at run time (`xinput1_4.dll`, then `xinput9_1_0.dll`, `xinput1_3.dll`), empty slots polled at most
once per second; focus loss releases every key and button. Pause/Break and the E0 fake shifts are ignored.
Absolute mouse reports (what an RDP session delivers for a physical mouse) become pixel differences.

### Commands and results (item 2)

| Command | Result |
|---|---|
| `build-win\test_input.exe` (portable; also on Linux CI) | 800 checks, 0 failures: key names, splitting property, digital, radial/axial sticks, curves, triggers, mouse stick and delta, record encoding, 14 JSON refusals, records driving a world (byte-identical recorded inputs across two runs) |
| `build-win\test_input_win32.exe` (synthetic `RAWINPUT` and a fake `XInputGetState`) | 50 checks, 0 failures |
| `build-win\input_probe.exe --inject` (SendInput through the real Windows input stack in this RDP session: right Ctrl, mouse +40/-40 px) | right Ctrl seen and released; mouse dx 40; reports were relative, not absolute |
| `build-win\input_probe.exe --seconds 2` | XInput loaded; no gamepad connected (none is attached to this machine) |
| `ctest --test-dir build-win -j 8` (Debug, after merging main at `66cf3a7`) | 75 passed, 0 failed, 2 skipped (`path_fifo`, `containment`) of 77 |

Not run: a physical gamepad (none attached; the XInput path is covered only by the fake), a physical keyboard
or mouse driven by a person (injected input checks registration, delivery and translation, not a device). The
absolute-mouse path is covered by the synthetic test only: in this session injected motion arrived relative.
Raw Input registration is per process, so one `SuInputWin32` per process.

## 3. First Vulkan presentation slice (M5)

`include/sutekh/render.h`; libraries `su_render_util` (portable, always built) and `su_render` (Vulkan backend,
built when Vulkan headers are found: the pinned bootstrap checkout or `VULKAN_SDK`; `-DSUTEKH_RENDER=OFF|AUTO|ON`).
Headless builds have no Vulkan dependency, and `su_render` links no Vulkan library: `vulkan-1.dll` /
`libvulkan.so.1` is opened at run time, so a machine without Vulkan gets a status and a reason, not a crash.

- Portable: float math in Vulkan clip space (`SuVec3f`, `SuQuatf`, `SuMat4f`, `su_mat4f_look_at`,
  `su_mat4f_perspective`), chase camera (frame-rate independent smoothing) and free camera, procedural meshes
  (box, 24 x 16 sphere, arrow ship), debug lines (`su_debug_line`, `_vector`, `_ellipse`, `_plane`, `_box`,
  `_axes`, with a dropped-line count), PPM writer.
- Vulkan: `su_render_enumerate` (name, type including CPU rasterizers, API and driver versions, device-local
  memory, timestamp support, and why a device does not meet the baseline), `su_render_create` (window HWND or
  offscreen; Vulkan 1.2 instance and device, graphics queue that can present, depth and sRGB colour formats,
  optional `VK_LAYER_KHRONOS_validation` with an error counter), `su_render_frame` (instances grouped by mesh into
  one instanced draw per mesh, then a line-list draw; swapchain recreation on resize, out-of-date or suboptimal;
  zero-size windows pause), `su_render_stats` (CPU, wait, present and GPU times, draws, counts, dropped,
  recreations, validation), `su_render_readback` (offscreen), `su_render_last_error`.
- Shaders: GLSL in `src/render/shaders`, compiled offline with glslang 16.6.0 (`tools/compile_shaders.py`)
  into `src/render/su_render_shaders.h`, which records each source's SHA-256; `render_shaders_fresh` fails when a
  source changed without regenerating.
- Renderer boundary: it receives float instances and lines per frame and never sees a world. The demo reads
  committed state through `su_host_read_i64` and interpolates with the clock's alpha; `--offscreen` steps a
  shadow world without a renderer on the same inputs and compares fast digests every tick.

Demo `examples/presentation/space_demo.c` with `flight.capsule.json` / `flight.lir.json` (IR 0.3, 72
instructions, Q32.32 position, velocity and orientation; input axes arrive as Q16.16 i32 fields and are shifted to
Q32.32 in the system; attitude by `quat_integrate`, thrust along the nose, a spring toward the origin, per-entity
drag; a ship crossing the gate disc emits event kind 1). Window, Raw Input/XInput through the default map, 120 Hz
clock with up to 8 ticks per frame, per-tick input split, chase camera (free camera on the `camera_next` button),
velocity vectors, gate ellipse that flashes on the gate event (read with `su_host_events_for_tick`), grid and
axes, telemetry in the console and title bar.

Machine facts for this item: `vulkaninfo --summary` over the RDP session lists one Vulkan device, the physical
**Quadro P600** (discrete, Vulkan 1.4.312, NVIDIA 582.16, loader 1.4.309); the Microsoft Remote Display Adapter
is not a Vulkan device. Window presentation works over RDP. The Vulkan SDK was not installed: `winget install
KhronosGroup.VulkanSDK` stopped at a UAC elevation prompt in the RDP session (this shell is not elevated), so the
validation layer was not available for any run; glslang 16.6.0 (release zip, sha256 `82bf434e...8d9b`) and
Vulkan-Headers v1.4.365 (`tools/bootstrap_vulkan_headers.py`) were used instead, both recorded in
`THIRD_PARTY_NOTICES.md`.

### Commands and results (item 3)

| Command | Result |
|---|---|
| `python -I tools/bootstrap_vulkan_headers.py` then `--check` | v1.4.365 at `c46850864f46`, license sha256 recorded, check ok |
| `python -I tools/compile_shaders.py --check --compiler <glslang.exe>` | 4 sources, recompiled, identical SPIR-V |
| `build-win\test_render_util.exe` | 42 checks, 0 failures (projection and view against hand values, cameras, mesh volumes 1.0 / 0.5127 / 0.042 and outward normals, debug lines, PPM) |
| `build-win\test_render_offscreen.exe` (Debug) and `build-rel\...` (Release) | 17 checks, 0 failures each on the P600: corner = clear colour (89, 89, 108), centre = lit red box (255, 0, 0: depth test and winding), green sphere beside it, blue ship, yellow line on the predicted row, two frames byte-identical |
| `space_demo --offscreen ... --frames 240` (Release) | 479 ticks, gate event at tick 187, shadow world: 0 of 479 tick digests differed, image written |
| `space_demo --script --seconds 5` (window, FIFO) | presented through the swapchain over RDP; 32 fps (the wait is in acquire: about 31 ms, the remote display's pace), 120 ticks/s, 0 dropped ticks |
| `space_demo --script --seconds 5 --no-vsync` (window, MAILBOX) | about 7,700 to 8,300 fps, 120 ticks/s, 0 dropped ticks |
| `ctest --test-dir build-win -j 8` (Debug, renderer built) | 78 passed, 0 failed, 2 skipped (`path_fifo`, `containment`) of 80 |
| CI run 39122156097 (commit `91e9baa`): Linux checked runtime (GCC, `SUTEKH_RENDER=ON`, Mesa lavapipe) | 78 of 78 passed, `render_offscreen` included (rendered and pixel-checked on a CPU Vulkan device) |
| same run: Windows checked runtime (clang, `SUTEKH_RENDER=ON`) | 77 passed, 0 failed, 3 skipped of 80: `path_fifo`, `containment`, and `render_offscreen` (no Vulkan device on the runner); `input_win32` passed on the runner |
| same run: `validate` jobs | failed: `PACK_VALIDATION.json` stale after the workflow edit; regenerated in the next commit |

Logs and images: `planning/evidence/presentation-input/renderer-runs.log` (source commit, all commands and
output), `render-offscreen.png`, `space-demo-offscreen.png` (converted from the PPM readbacks).

Timing observations (Release, P600 over RDP, trivial scene; not the Arc A770 target and not qualification):
render CPU time per frame 0.02 to 0.12 ms, GPU time (timestamp queries) 0.02 to 0.06 ms in the window and 0.31 ms
for the 1280 x 720 offscreen frame, simulation step 0.07 ms per tick for 12 IR 0.3 ships (0.28 ms while the
MAILBOX loop saturates the CPU).

Not run or not done: the validation layer (SDK not installed, see above); a physical-console run and any run on
the Arc A770 (frame pacing over RDP is the remote display's, so FIFO numbers say nothing about the target); window
presentation on Linux (offscreen only there); text/HUD, culling, pipeline caches and asynchronous pipeline
preparation, resource retirement beyond create/destroy (M5 items after this slice). The Windows CI runner has no
Vulkan device, so `render_offscreen` skips there; the Linux CI job installs Mesa's lavapipe to run it on a CPU
device.
