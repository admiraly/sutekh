# Issue 119: host-owned audio agent registration

State: implementation complete; local validation recorded below. Remote CI
is not an acceptance claim and is not awaited. No blockers or follow-up
implementation work identified. The host must explicitly opt in per session.

Base: `7aa23242a98fc29ebcd364b3a806129c69f7d1c1`.
Branch: `feat/issue-119-audio-control-plane`.

## Implemented

- Borrow the host's audio system through `SuAgentConfig.audio_system`.
- Discover and route all four methods through `su_audio_system_method` and
  `su_audio_system_control`; headless sessions retain their existing surface.
- Reuse the agent's strict envelopes, duplicate-key rejection and local-fd
  authorization boundary. Validate method parameters and normalize integer
  frame counts before passing them to the audio control API.
- Lock control access against pull rendering; preserve host ownership and
  live playback, while retaining trace-read drain semantics.
- Test discovery, populated and empty traces, direct-result equivalence,
  offline PCM reporting, one-session multi-request framing, strict invalid
  parameters, optional telemetry/journal errors and unchanged live state.
- Update registration/lifetime/auth documentation and stale integration notes.

## Modified paths

`src/agent/su_agent.c`, `src/agent/su_agent.h`,
`cmake/SutekhPresentation.cmake`, `tests/audio/test_audio_agent.c`,
`docs/AGENT_RPC.md`, `docs/EVENT_AUDIO_INSPECT.md`,
`docs/EVENT_AUDIO_INTEGRATION.md`, `PACK_VALIDATION.json`, and this handoff.
No shared audio implementation or frozen public contract was changed.

## Local validation

Toolchain setup: `. .\tools\env-windows.ps1` and
`python tools/bootstrap_asset_deps.py` (pinned dependencies verified).

- `cmake -S . -B build-i119 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_RENDER=OFF -DSUTEKH_NATIVE=OFF`
- `cmake --build build-i119 -j 2`: passed, Clang 23.1.3 strict C17,
  `-Wall -Wextra -pedantic -Werror`.
- `ctest --test-dir build-i119 --output-on-failure -j 2`: 124 passed,
  two platform skips (`path_fifo`, `containment`), zero failed; 69.68 seconds.
- `build-i119/test_audio_agent.exe .`: 443 checks, zero failures.
- MSVC 19.44.35229: selected `bin/Hostx64/x64` from the MSVC installation
  found by `env-windows.ps1`, plus the matching SDK `bin/<version>/x64`,
  on PATH (the locally installed VS does not provide `vcvars64.bat`).
- `cmake -S . -B build-i119-msvc -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF`
- `cmake --build build-i119-msvc -j 2`: entire default build passed,
  strict C17, `/W4 /WX`.
- `ctest --test-dir build-i119-msvc -R '^(audio_.*|control|recovery|proto|snapshot)$' --output-on-failure -j 2`:
  all 25 selected tests passed, zero failures/skips; 7.52 seconds.
  A redundant full MSVC Debug suite was stopped during unrelated unoptimized
  geometry tests (mesh alone took 129.82 seconds); no full-MSVC-suite pass
  is claimed. The complete Clang suite above is the full local regression run.
- `python tools/validate_pack.py --write-report`: passed; regenerated report.
- `python tools/validate_pack.py --check-report`: passed.
- `python tools/check_repository_license.py`: passed.
- `python -I tests/tools/test_validate_pack.py`: eight tests passed.
- `git diff --check`: passed.

Initial checks exposed interrupted-test defects: the system-create diagnostic
argument had the wrong type; trace reads drain entries rather than returning
repeatable populated results; request IDs must be strings; duplicate-key
parse errors have null IDs. Corrected the tests before the full successful run.
The initial focused run also hit the existing timing-sensitive `audio_system`
pull-reader journal-count assertion; the complete rerun passed without changing
that test or runtime.

Unrun: GCC 16.2/Linux, sanitizers, native-tier and Vulkan-renderer lanes.
These are not claimed as locally verified. No performance claim is made.
Next: publish the branch and PR closing issue 119; CI remains the remote gate.

## SHA-256 evidence

| Path | SHA-256 |
| --- | --- |
| src/agent/su_agent.c | f344306ecc478ed8208fd86dc0c3df569aeeb6c4e1fb0f61476782ccef03fadb |
| src/agent/su_agent.h | 51fb68d8f10bd21d23817482051f80a700b1317f99f88f2ca5095223d35abc81 |
| cmake/SutekhPresentation.cmake | 31028fde2a3a69647f373199e1da840ee610a8866c34063b9278271573031dfc |
| tests/audio/test_audio_agent.c | b949166f67f5df84d675e4b850b3f3f507f108b3a3ff9e4c8aef5f5f65caf998 |
| docs/AGENT_RPC.md | 7d52453fa644c2f158a2977d00ec6ccb509aa99b7a530d505a88cbc5491ff1e3 |
| docs/EVENT_AUDIO_INSPECT.md | f9e4a9b8473896511d31ae62210f389237e26136364f7a58b11df89a4f5800ff |
| docs/EVENT_AUDIO_INTEGRATION.md | 17bcedd99f34e400bba1bce69fd83d9700aad39d7877dc6f99d0b2f96ee8e0e6 |
| build-i119/test_audio_agent.exe | 8e93810443fda472854e9291665a2a1a0161eabe64fc61bac70274c50de8c435 |
| build-i119-msvc/test_audio_agent.exe | 97c2a17f3cea73009d2b900cd43dff2557a7ce7935d2c988dd07a6fab389efce |
