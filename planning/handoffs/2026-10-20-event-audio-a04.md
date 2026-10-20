# A04 integration checkpoint

Date: 2026-10-20. Builds on A01-A03 without changing legacy public ABI.
Changed paths: `include/sutekh/audio_event.h`, `src/audio/su_audio_event.c`,
`src/audio/su_audio_event_int.h`, private mixer helpers in
`src/audio/su_audio_mix.c`, `tests/audio/test_audio_event.c`,
`docs/EVENT_AUDIO_RUNTIME.md`, CMake runtime registration, foundation lease,
this handoff and the generated pack report.

The single-owner presentation runtime provides bounded timestamped instances,
exact start/stop/release boundaries, emitter and scope cancellation, persistent
session commit watermarks, failed-admission atomicity and deferred collection.
A private fixed-quantum cache makes arbitrary external render chunking produce
identical local PCM. No render-time allocation or resource release is required.
Physical admission is conservative until A05 adds virtualization. A device
adapter must serialize commands with rendering; this is not an automatic
attachment to the legacy device thread.

Strict Clang and MSVC runtime tests pass 2923 checks, using synthesized clips.
Review round 2 found release-boundary, direct-stop, hard-stop precedence and
live spatial propagation defects. Regression tests cover the repairs; round 3
found no further actionable A04 defect. All findings were resolved before
integration. Existing-binary review results are not substituted for final
staged rebuilds or CI.

Measured runtime payload is 8184 bytes for eight instances, four leaves per
instance, 32 physical slots and a two-command stop queue. This excludes the
mixer, graph, scene, assets and allocator overhead. One 48000-frame scalar
render observed approximately 1 ms CPU time in the Debug test on this machine;
the coarse CPU clock is not a latency distribution or performance gate.

Integration rebuilds all audio tests from the exact staged snapshot and runs
the full Clang suite plus affected MSVC tests. Cross-platform PR CI is required
before merge. Full DSP checkpoints, virtual voices, live event parameter
snapshots, bus processing, directional clustering and inspection remain for
A05-A10. Watermark restore is intentionally not a PCM/filter checkpoint.

Final local merged-tree evidence at main `29894fa`: full Clang Debug suite,
112 passed plus two explicit POSIX skips, zero failures (75.22 seconds).
Clang ASan spatial/graph/event targets: three passed, zero failures.
Commands and exact source/artifact hashes are in the runtime component note.
PRs: A01 #101, A02 #103, A03 #104, A04 #105; all are linked to this work thread.
CI and ordered merges remain external gates at this checkpoint.
