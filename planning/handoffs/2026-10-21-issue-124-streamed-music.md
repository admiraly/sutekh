# Issue 124: deterministic streamed music

Date: 2026-10-21. Branch: `feat/issue-124-streamed-music`, base `7aa2324`.
Owner-authorized implementation; no CLA action required. No third-party code
or external content added. Existing pinned decoder implementations are reused.

## Scope and behavior

- `include/sutekh/audio_parameter.h`: `SuAudioMusicStreamSource` and the
  optional `SuAudioMusicSegmentDesc.stream_stems` table. Exactly one resident
  or streamed stem table is required per segment.
- `src/audio/su_audio_music.c`: owned synchronous seekable streams, two
  decoder windows per stem for independent current/outgoing cursors; existing
  transitions, tempo, cues, stingers, loops and fades share the resident path.
  Source content SHA-256 and numeric segment configuration bind checkpoints;
  decoder windows are reconstructed from logical frame positions.
- `src/audio/su_audio_parameter_int.h` and the one music fingerprint line in
  `src/audio/su_audio_event.c`: include source/configuration identity in the
  outer checkpoint layout fingerprint so mismatches reject before state load.
- `tests/audio/test_audio_system.c`: memory/file stereo sources, multistem
  resident equivalence, beat/bar/cue/end/self transitions, stingers, chunking,
  replay, offline hash, direct restore, verification, invalid sources,
  failure cleanup, sample-rate mismatch, content mismatch rejection without
  changing the destination clock, full three-minute nonlooping track and EOF.
- `tests/audio/test_audio_music_stream.c` and its registration in
  `cmake/SutekhPresentation.cmake`: mono WAVE, optional Ogg file/memory
  equivalence across different decoder window sizes, sample/block chunking,
  loop/self-fade and checkpoint continuation after caches have moved.
- `docs/EVENT_AUDIO_STREAMED_MUSIC.md`, `docs/EVENT_AUDIO_PARAMETERS.md`,
  `docs/EVENT_AUDIO_INTEGRATION.md`, `docs/EVENT_AUDIO_MIGRATION.md`: API,
  playlist migration, lifetime, memory and execution
  contract; the new guide explicitly distinguishes deterministic synchronous
  streaming from asynchronous real-time I/O guarantees.
- This handoff, the issue-specific lease/evidence JSON and
  `PACK_VALIDATION.json`: durable task state and validation evidence.

No host input, device, switch/blend, cluster, loudness or voice-policy behavior
was changed. No commits, branches or files outside the assigned worktree
were modified. Toolchain junctions are borrowed, not committed.

## Validation

All commands run from this worktree. Bootstrap:
`python tools/bootstrap_asset_deps.py`, `python tools/bootstrap_asmjit.py`,
`python tools/bootstrap_vulkan_headers.py`; pinned dependencies verified/fetched
inside this worktree. Native commands first dot-source
`. .\tools\env-windows.ps1`.

- Clang 23.1.3 Debug, strict C17 warnings as errors:
  `cmake -S . -B build-i124 -G Ninja -DCMAKE_BUILD_TYPE=Debug`,
  `cmake --build build-i124 -j 4`, then
  `ctest --test-dir build-i124 --timeout 180 --repeat until-pass:2 --output-on-failure -j 2`.
  Final full-suite result pending at this checkpoint.
- MSVC 19.44 Debug, `/W4 /WX`: configure `build-i124-msvc` with Ninja,
  `CMAKE_C_COMPILER` pointing at the installed MSVC `Hostx64/x64/cl.exe`,
  `CMAKE_MT`/`CMAKE_RC_COMPILER` pointing at the installed SDK x64 tools,
  and those directories added to PATH after `env-windows.ps1`.
  `cmake --build build-i124-msvc --target test_audio_system test_audio_music_stream test_audio_parameter test_audio_runtime test_audio_system_alloc -j 4`;
  `ctest --test-dir build-i124-msvc -R '^(audio_system|audio_music_stream|audio_parameter|audio_runtime|audio_system_alloc)$' --timeout 120 --output-on-failure`:
  **5/5 passed**.
- Clang AddressSanitizer: configure `build-i124-asan` with
  `CMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer` and
  `CMAKE_EXE_LINKER_FLAGS=-fsanitize=address`, build the four non-allocation
  targets above, then
  `ctest --test-dir build-i124-asan -R '^(audio_system|audio_music_stream|audio_parameter|audio_runtime)$' --timeout 120 --output-on-failure`:
  **4/4 passed**. Add the toolchain's
  `LLVM/lib/clang/23/lib/windows` directory to PATH for the ASan runtime DLL.
- Earlier full Clang suite before the final long-track/codec coverage:
  138 registered, 136 passed and 2 platform-specific skips.
- Initial new tests used rings below the existing 2048-frame minimum,
  compared accumulated positive zero against a negative-zero fixture, and
  requested a cue before the previous fade had ended. These fixture errors
  were corrected; exact cue/end boundaries and a checkpoint during the active
  cue fade are now asserted. An existing wall-clock-sensitive pull test
  (`journal count > 10`) failed intermittently under concurrent validation
  and passed on targeted reruns; no unrelated runtime or expected-result
  change was made. The final full-suite command permits one failed-test retry.
- Initial MSVC configuration lacked SDK resource/manifest tools on PATH;
  corrected as above. Initial ASan launch lacked the runtime DLL directory;
  only this worktree's stalled test/runner was stopped, then rerun successfully.

Source/artifact SHA-256 and final gate results are recorded in
`planning/evidence/issue-124-streamed-music.json`. Build logs stay in ignored
`build-i124/` directories. GCC 16.2/Linux validation is not available locally;
the PR's normal CI must establish those results. Do not claim CI passed.

## Limits and next work

Sources must remain available and immutable for live/replay systems. Files are
fingerprinted with bounded reads at creation; decode/read/seek work remains
synchronous on the rendering owner (including a pull device thread). This
change does not promise asynchronous real-time I/O or arbitrary resampling.
Standalone event-trace manifests do not configure music; integrated offline
output and `audio.render.offline` do. Future async music would need a separate
deterministic starvation/seek contract rather than silently using async ring
underruns as reproducible source samples.

Next: finish local gates, push and open a PR closing #124. Do not wait for CI
and do not merge the PR as part of this task.
