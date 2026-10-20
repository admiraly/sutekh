# A03 integration checkpoint

Date: 2026-10-20. Builds on the additive A01 registry and A02 scene.
Changed paths: `include/sutekh/audio_graph.h`, `src/audio/su_audio_graph.c`,
`tests/audio/test_audio_graph.c`, `schemas/audio_events.schema.json`,
`docs/EVENT_AUDIO_GRAPH.md`, graph-only CMake registration, the foundation
lease, this handoff and the generated pack report.

The immutable retained graph pins resident asset generations. Preparation
validates strict JSON, exact integer IDs, profiles, sample rate, references,
cycles, depth, edges, expansion and accumulated delay. The six container kinds
expand into caller-owned leaves without allocation. Numeric switch/parameter
snapshots and optional one-previous-child random history are caller-owned;
failure preserves output and history. Plain expansion has no implicit history.

Component commands, exact source/dependency/artifact SHA-256 values and
limitations are recorded in `docs/EVENT_AUDIO_GRAPH.md`. Strict Clang and MSVC
component suites pass 850 checks; allocation instrumentation passes 851 checks
and records zero graph heap operations during expansion. AddressSanitizer
passes 850 checks. The Draft 2020-12 schema and documented synthetic example
validate. These are local results, not a cross-platform reproducibility claim.

Integration gates export the exact staged snapshot into `build-pack`, build
`test_audio_graph` and `test_audio_graph_alloc` with the preceding audio tests,
run `ctest --test-dir build-a01-accepted -R '^audio_' --output-on-failure`, and
regenerate `PACK_VALIDATION.json` using the snapshot validator. Cross-platform
PR CI remains required before merge. A04 adds timestamped instance ownership;
A05-A10 follow the write scopes in `docs/EVENT_AUDIO.md`.
