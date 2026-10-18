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
