# Event-audio integration checkpoint

Date: 2026-10-20. Tracking issue #113, merged in order as PRs #114 (voices and
spatial filtering), #115 (parameters, buses, music, limiter), #116 (clustering
feed, direct checkpoints) and #117 (combined system API, control methods,
integrated acceptance, benchmark, migration guide). The integrator owns the
shared headers, runtime and CMake for this area.

Changed paths: `include/sutekh/audio_event.h` (versioned hook,
`su_audio_events_attach`, direct checkpoints), `include/sutekh/audio_voice_policy.h`
(controls, `add_at`/`stop_at`), new `include/sutekh/audio_system.h`;
`src/audio/su_audio_event.c`, `su_audio_voice_policy.c`, `su_audio_bus.c`,
`su_audio_parameter.c`, `su_audio_music.c`, `su_audio_cluster.c`,
`su_audio_spatial.c`, `su_audio_graph.c` (snapshot bound), `su_audio_inspect.c`
(capability list), `su_audio_wasapi.c`/`su_audio_nodevice.c` (pull-mode open),
new `su_audio_system.c` and private headers `su_audio_state.h`,
`su_audio_voice_int.h`, `su_audio_bus_int.h`, `su_audio_parameter_int.h`,
`su_audio_cluster_int.h`; tests `test_audio_runtime.c`, `test_audio_system.c`,
an integrated stage in `test_audio_acceptance.c`; `bench/audio/bench_audio_system.c`;
`cmake/SutekhPresentation.cmake` (voice pool, bus, parameter, music and
cluster sources now build into `su_audio_events`; their old targets are
interface aliases; new `su_audio_system`, tests and benchmark); docs
`EVENT_AUDIO_INTEGRATION.md`, `EVENT_AUDIO_MIGRATION.md` and notes in the
component docs.

Local evidence: 21 audio CTests pass with Clang 23.1.3 Debug and Release
(`-Werror`), Clang AddressSanitizer, and a full MSVC 19.44 `/W4 /WX` build.
`audio_runtime` 7714 checks, `audio_system` 4542, `audio_system_alloc` 0
allocations after creation over 480,000 frames (1.24 MB payload),
`audio_acceptance` 15832 including real-time equals offline PCM. GCC and Linux
results come from PR CI.

Measured (Release, i5-12400F, 10 s): full configuration of 512 logical / 64
physical voices, 16 buses, 8 ducks, 3 stems and clustering costs 90-91 ms CPU
per second of audio; details in `docs/EVENT_AUDIO_INTEGRATION.md`.

Open: live switch/blend re-selection (#118), control-plane registration
(#119), device latency and underrun policy (#120), virtual-voice control cost
(#121), loudness-window input and an object renderer for clusters (#122).
