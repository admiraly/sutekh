# Issue 125: audio admission rules

Status: implemented and locally validated; ready for owner review via PR.
Branch: `feat/issue-125-admission-rules`; base: `7aa2324`.
Scope: audio event admission, telemetry, direct checkpoint history and tests.

## Behavior

Appended optional `event_id`, `min_retrigger_frames` and `group_id` to policy
rules. First-match selection is shared by admission and rendering. A rule
owns a cooldown timeline; a nonzero group refuses new posts while a matching
leaf is pending, playing, virtual or releasing. Whole-post checks precede
asset pins and logical voices. Refused posts leave history and commit cursor
unchanged and return the appended stable statuses 22/23 with counted trace
reasons. Ranking and the standalone voice pool API are unchanged.

Direct checkpoints advance to version 2, serialize the timestamp and flag
without padding, and fingerprint admission settings. Group occupancy derives
from saved leaves. Zero admission fields preserve previous behavior.

## Exact modified paths

- `include/sutekh/audio_event.h`
- `include/sutekh/audio_inspect.h`
- `include/sutekh/status.h`
- `src/audio/su_audio_event.c`
- `src/audio/su_audio_inspect.c`
- `tests/audio/test_audio_system.c`
- `docs/EVENT_AUDIO_VOICE_POLICY.md`
- `planning/contracts/M0_CONTRACT.md`
- `planning/handoffs/2026-10-21-issue-125-admission.md`

## Validation

All commands run in the issue worktree, after `. .\tools\env-windows.ps1`.
Dependencies: `python tools/bootstrap_asset_deps.py` and
`python tools/bootstrap_vulkan_headers.py` succeeded; lock files unchanged.

- `cmake -S . -B build-i125 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF -DSUTEKH_ASSET_IMPORT=ON`
- `cmake --build build-i125 -j 2`: passed with Clang 23.1.3, C17,
  `-Wall -Wextra -pedantic -Werror`.
- `ctest --test-dir build-i125 --output-on-failure -j 2`: passed, 125 tests,
  zero failures, 145.98 seconds. Existing Windows skips: `path_fifo` and
  `containment` (123 executed tests).
- `ctest --test-dir build-i125 -R 'audio_' --output-on-failure`: passed,
  21/21, after atomic-post and timestamp-overflow coverage was added.
- `cmake --build build-i125 -j 2` and
  `ctest --test-dir build-i125 -R '^audio_system' --output-on-failure`:
  passed, 2/2 in 1.82 seconds, with the final virtual-group and telemetry
  counter assertions. Two earlier loaded-machine reruns hit the existing
  PULL timing fixture's journal-count threshold at line 596 (not admission
  assertions); no unrelated test or runtime change was made.
- MSVC environment: import `vcvars64.bat && set` from the installed VS 2022
  BuildTools after the project environment script. The script prints a
  nonfatal `vswhere.exe` lookup warning but imports cl 19.44.35229.0.
- `cmake -S . -B build-i125-msvc -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF -DSUTEKH_ASSET_IMPORT=ON`
- `cmake --build build-i125-msvc -j 2`: passed, full build, `/W4 /WX`.
- `ctest --test-dir build-i125-msvc -R '^audio_system' --output-on-failure`:
  final source passed, 2/2 in 1.84 seconds.
- `ctest --test-dir build-i125-msvc --output-on-failure -j 2`: passed,
  125 tests, zero failures, 499.54 seconds; the same two existing Windows
  skips (123 executed tests). The final targeted test executable was rebuilt
  before this run reached the audio tests.
- `python tools/validate_pack.py --check-report`: passed; committed
  `PACK_VALIDATION.json` is current and needs no regeneration.
- `git diff --check`: passed.

Logs are retained in the ignored `build-i125/` and `build-i125-msvc/`
directories. GCC 16.2, renderer/native-backend configurations, sanitizers
and benchmarks were not run locally; no GCC installation is available in
the configured shell. CI is not awaited. No force-push or shared toolchain
edits are needed. Next: owner review of the PR closing issue 125; no remaining
implementation blockers or local verification debt for this scope.

## SHA-256 evidence

Hashes exclude this self-referential handoff and cover the final source and
targeted test executables:

```text
71060b87efaaf8e671550f73c0433d2a9ee2d04c5411102ceb320ba6c3bde6f8  include/sutekh/audio_event.h
696cb3d58c88df9c730a1e4911dae6a09b77941748ed3ffe5206740a1bcef900  include/sutekh/audio_inspect.h
9f9f905ea60aea64d54baa5458b1444e009eea4f6275c095ab8f822a1354fe73  include/sutekh/status.h
35393bc600b7e4339f5e035904e665c5e28f5a1125151c102778d3ef74aaf710  src/audio/su_audio_event.c
12ede47ba8ee5a7bd83e66ee1351ca88d5e31ac5fb9e5a3faf5f725ed85fcd92  src/audio/su_audio_inspect.c
3193a5a7cea4d00de0169c448e3388dc0a0d47b7494d920308d995d4dad39ab0  tests/audio/test_audio_system.c
90a20c62ffa3bd8fc777d6b76a1886f867945fb1da79d951cdef017b983d03da  docs/EVENT_AUDIO_VOICE_POLICY.md
de3ed44c149e7eadb568ee991e07779303ddca4b663d6bfe7f60ec060b7d7b19  planning/contracts/M0_CONTRACT.md
1c7c97a71c4b5d39a74c945eafdb0d07e11f9a176f5a614bc506bb66ea7c62ca  build-i125/test_audio_system.exe
a24502a5d3880405ae3f28a241daed9aef3ecd215630cfc17ffbc40a58752871  build-i125-msvc/test_audio_system.exe
```
