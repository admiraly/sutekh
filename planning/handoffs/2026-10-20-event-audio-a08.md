# A08 directional clustering checkpoint

Date: 2026-10-20. Builds on main `12b02d8` without changing any shared header,
schema or runtime source. Changed paths: `include/sutekh/audio_cluster.h`,
`src/audio/su_audio_cluster.c`, `tests/audio/test_audio_cluster.c`,
`docs/EVENT_AUDIO_CLUSTERING.md`, an additive `su_audio_cluster` library,
instrumented object library and `audio_cluster` test registration in
`cmake/SutekhPresentation.cmake`, and this handoff.

Delivered: optional persistent clustering with RMS-weighted centroids,
priority/proximity/flag exclusions, metadata compatibility, join/leave
hysteresis, hold, complementary crossfades, draining clusters, closest-pair
budget merges with an explicit infeasible flag, bed pass-through, a CPU stereo
reference renderer and diagnostics (outputs, associations, stats).

Local evidence: `test_audio_cluster` passes 810 checks with strict Clang 23.1.3
Debug, including zero steady-state allocations; the sources compile cleanly
under MSVC 19.44 `/W4 /WX` and Clang `-Wall -Wextra -Wpedantic -Wshadow`.
GCC and Linux results come from PR CI.

Open: no backend object adapter; A05 identity and A06 pre-master integration
are integrator work.
