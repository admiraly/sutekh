# Issue 143: submodule consumer build (options, Sutekh:: targets, include hygiene)

## State and scope

Locally verified on `feat/issue-143-consumer-build`, based on `374af9d` (origin/main, merge of PR 146). Code commit
`2461f20`; this handoff and `planning/evidence/consumer-build/` are a separate commit on top. Not merged.

Sutekh can now be consumed with `add_subdirectory` (as a git submodule) without the consumer seeing Sutekh's tests,
engine internals or warning policy, and without writing into the submodule. Top-level builds are unchanged (same
targets, tests, test commands and link lines; see "Before and after").

Issue items:

1. `SUTEKH_BUILD_TESTS`, `SUTEKH_BUILD_BENCH`, `SUTEKH_BUILD_EXAMPLES` (`CMakeLists.txt`): default
   `${PROJECT_IS_TOP_LEVEL}`. Every `add_test`, test executable, test-only library (`su_agent_hooks`,
   `su_agent_test`, the `*_objs` seam builds) and scratch `file(MAKE_DIRECTORY)` is under `SUTEKH_BUILD_TESTS`;
   `enable_testing()` too. Bench programs (`bench_host`, `bench_audio_system`, `bench_audio_system_reference`) are
   under `SUTEKH_BUILD_BENCH`, examples (`input_probe`) under `SUTEKH_BUILD_EXAMPLES`. Programs a test runs are built
   when their option or the tests are on: `racer_microcosm`, `bench_host_rollback` (bench), `minimal_host`,
   `arena_host`, `space_demo` (examples), and the `su_audio_event_reference` objects. `sutekh` and `asset_cook`
   (tools) stay unconditional. Regions are wrapped without re-indentation (`endif() # SUTEKH_BUILD_TESTS`) so the
   diff shows only the guards; the audio section of `cmake/SutekhPresentation.cmake` was reordered (libraries first,
   then tests) because libraries and tests were interleaved there.
2. `cmake/SutekhPublic.cmake` (new, included last): `Sutekh::host`, `host_shared` (DLL), `input`, `window`,
   `render`, `render_util`, `asset`, `asset_import`, `audio`, `audio_events`, `audio_offline`, `audio_inspect`,
   `audio_system`, plus the existing `Sutekh::vulkan_headers`, and executable aliases `Sutekh::sutekh`,
   `Sutekh::asset_cook`. Optional ones exist only when built. The list is the INTERNAL cache entry
   `SUTEKH_PUBLIC_TARGETS` (a directory variable would not reach the consumer). Not aliased: the internal
   `su_*` core libraries, `su_space3d` (its header is in `profiles/`, not `include/`), and the interface shims
   `su_audio_bus/parameter/voice_policy/cluster` (all are `su_audio_events`). README documents the list.
3. Include hygiene: `su_host` links `su_vm su_world su_ents su_ir su_jobs su_cmdq su_platform` and `su_space3d`
   (and `su_native` on native builds) PRIVATE; `su_asset` and `su_audio` link `su_platform` PRIVATE;
   `su_audio_events` links `su_ir` PRIVATE. For static libraries CMake keeps PRIVATE dependencies on the link line
   (`$<LINK_ONLY:...>`), so link lines are unchanged. Every in-repo consumer still builds: tests that need internals
   already add `src` themselves. `SU_HAVE_NATIVE` stays a PUBLIC definition of `su_host` on native builds (tests that
   read the internal world need the same `SuSysEntry` layout); it is a define, not an include directory, and no
   public header uses it.
4. Vulkan headers (`cmake/SutekhPresentation.cmake`): the pinned bootstrap checkout (now under `SUTEKH_DEPS_DIR` if
   set), then `VULKAN_SDK`, then system headers (`find_path`, cache `SUTEKH_VULKAN_SYSTEM_INCLUDE`), accepted when
   `VK_HEADER_VERSION_COMPLETE`/`VK_HEADER_VERSION` give 1.3.204 or later (`SUTEKH_VULKAN_SYSTEM_MIN`). 1.3.204 is the
   oldest release built and tested here (Ubuntu 22.04's package version); the renderer requests the Vulkan 1.2 API.
   Older or unparsable system headers are reported with their version and not used. The SDK path keeps its old
   behavior (no version check). Bootstrap destination: `--dest DIR` for `bootstrap_vulkan_headers.py`,
   `bootstrap_sdl3_headers.py` and `bootstrap_asset_deps.py`, with `-DSUTEKH_DEPS_DIR=DIR`. With `--dest` the
   tracked lock file is verified, never written, and an unusable `DIR/<name>` is replaced only with `--force` (the
   in-tree path keeps its old replace behavior). AsmJit (`SUTEKH_NATIVE`) stays in `third_party/asmjit`.
5. Messages: `skipped (SUTEKH_RENDER=OFF)`; without headers, the reason (including a rejected system version) and the
   three ways to get headers.
6. `CMAKE_BINARY_DIR` → `CMAKE_CURRENT_BINARY_DIR` everywhere (identical at top level). In a consumer with
   `SUTEKH_BUILD_TESTS=ON`, all 147 Sutekh tests pass and the consumer's top-level build directory receives nothing
   but ctest's own `Testing/`.
7. `SUTEKH_WERROR` defaults `${PROJECT_IS_TOP_LEVEL}` (`cmake/SutekhWarnings.cmake`). In a consumer, each public
   library gets `include/` as `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` (`-isystem`), and `Sutekh::vulkan_headers` is
   `SYSTEM INTERFACE` (unused in-repo). Top level is unchanged (engine tests keep `-I`).
8. `cmake_minimum_required(VERSION 3.21)`: CMake 3.20.5 fails to configure with GCC 16.2.1 and Clang 22.1.8 ("requires
   the language dialect C17, but CMake does not know the compile flags"); 3.21.4, 3.22.6, 3.23.3, 3.24.3 and 3.25.2
   configure. 3.21 is also the first with `PROJECT_IS_TOP_LEVEL`. Tested minimum: 3.21.4 (full top-level suite with
   Clang 22, consumer checks with GCC 16 and Clang 22). The policy level moves from 3.20 to 3.21 (CMP0121..CMP0126);
   the snapshots show no effect.

Also found and fixed: `third_party/vulkan-headers.lock.json` held the license digest of a CRLF checkout
(`a2d44079...`), so the bootstrap on Linux rewrote the tracked lock (`95ad366d...`). The digest is now taken with
CRLF normalized to LF, as `bootstrap_sdl3_headers.py` already does; lock and `THIRD_PARTY_NOTICES.md` updated.

Exact modified paths:

- `.github/workflows/repository-checks.yml` (new job `consumer`)
- `CMakeLists.txt`
- `README.md` ("Sutekh as a submodule")
- `THIRD_PARTY_NOTICES.md` (Vulkan-Headers license digest)
- `cmake/SutekhHostOps.cmake`, `cmake/SutekhNumeric3D.cmake`, `cmake/SutekhPlatform.cmake`,
  `cmake/SutekhPresentation.cmake`, `cmake/SutekhWarnings.cmake`
- `cmake/SutekhPublic.cmake` (new)
- `tests/consumer/CMakeLists.txt`, `tests/consumer/check_consumer.py`, `tests/consumer/consumer_host.c`,
  `tests/consumer/internal_probe.c` (new)
- `third_party/vulkan-headers.lock.json`
- `tools/bootstrap_asset_deps.py`, `tools/bootstrap_sdl3_headers.py`, `tools/bootstrap_vulkan_headers.py`
- `planning/evidence/consumer-build/` (new: `snapshot_build.py`, `snapshot-{full,headless,native}.txt`,
  `nodeps-main-vs-branch.diff`)
- `planning/handoffs/2026-10-26-consumer-build.md`, `planning/handoffs/2026-10-26-consumer-build.state.json`
- `PACK_VALIDATION.json` (regenerated for the new JSON file)

## Consumer test

`tests/consumer/` is a minimal project: `add_subdirectory(<sutekh> sutekh)`, a consumer `-Wall -Wextra -Wpedantic
-Werror` set, `consumer_host` (links `Sutekh::host` only; world from `examples/movement.*`, 120 ticks, snapshot at 60,
restore, re-simulate, digests must match), `consumer_public_headers` (every `include/sutekh/*.h` in one TU, plus
`-Wpadded`, linked against every library alias), and three `EXCLUDE_FROM_ALL` probes that include
`world/su_world.h` (src/), `su_platform.h` (src/platform/) and `space3d_geom.h` (profiles/space3d/native/).
`python -I tests/consumer/check_consumer.py [--cmake C] [--config Debug] [--build DIR] [-- cmake args]` configures
it in a fresh directory outside the checkout and reports:

`defaults` (the four options are OFF), `build`, `ctest-list` (exactly `consumer_host`, `consumer_public_headers`),
`no-engine-tests` (no Sutekh test, bench or example program was built), `internal-headers[src|src_platform|space3d]`
(each probe fails, and the failure is the missing header), `system-headers` (GCC/Clang: the Sutekh tree appears only
as `-isystem <sutekh>/include` or `-isystem` fetched `third_party/` headers), `system-headers[control]` (the same
public-headers compile with `-I` instead fails on `-Wpadded`, so the SYSTEM check is not vacuous), `run` (ctest),
`tree-unchanged` (no file of the checkout created, changed or removed; `build*/`, `out/`, `.venv/`, `.cache/`, `.git`
are not compared).

Mutation check: with su_host's dependencies back to PUBLIC, the SYSTEM property removed and
`SUTEKH_BUILD_TESTS` forced ON, the driver reports FAIL for `defaults`, `build` (the `-Wpadded -Werror` TU),
`ctest-list` (Sutekh's tests listed), `no-engine-tests`, all three `internal-headers`, `system-headers` and `run`.

The CI job `consumer` (ubuntu-latest) installs CMake 3.21.4 in a venv and the pinned CMake 4.3.4, installs the
distribution's `libvulkan-dev` (system-header fallback), bootstraps SDL3 and the asset deps with `--dest
$RUNNER_TEMP/deps`, runs the driver with 3.21.4 (Makefiles, Debug) and 4.3.4 (Ninja, Release) with `SUTEKH_RENDER=ON`,
then requires `git status --porcelain --ignored` to be empty. The whole job was reproduced locally in a fresh clone
of the branch (GCC 16, system Vulkan 1.4.341): all checks PASS, status clean.

## Before and after (top level)

`planning/evidence/consumer-build/snapshot_build.py` records targets (file API: name, type, artifacts), tests (names,
properties) and the generated `CTestTestfile.cmake` (full commands, built or not), with `<SRC>`/`<BLD>` paths.
Compared on origin/main (`374af9d`, `git archive` plus the same fetched third-party files) and the branch, Ninja,
Debug:

| configuration | targets | tests | snapshot diff |
| --- | --- | --- | --- |
| full (`SUTEKH_RENDER=ON SUTEKH_ASSET_IMPORT=ON`, SDL3) | 153 | 147 | none |
| headless (`SUTEKH_RENDER=OFF SUTEKH_WINDOW=OFF`) | 138 | 127 | none |
| native (`SUTEKH_NATIVE=ON SUTEKH_RENDER=ON`) | 167 | 164 | none |

The snapshots are in the evidence directory. Also compared, full and native: link lines (`LINK_LIBRARIES` of
every linked target in `build.ninja`) identical (118 and 82 linked targets); `compile_commands.json` identical
except 69 compile lines in each, which only lose flags (none added, none reordered). Full: the leaked include
directories `-I src/platform`, `-I src`, `-I profiles/space3d/native`. Native: the same, and on the 11 C targets
that link `su_host` (`test_host`, `test_clock`, `test_replay`, `test_input`, `test_host03`, `test_host04`,
`test_host_ops`, `test_host_rollback`, `bench_host`, `bench_host_rollback`, `space_demo`) also AsmJit's usage
requirements that leaked through `su_host PUBLIC su_native` (`-DASMJIT_STATIC -DASMJIT_NO_AARCH64 -DASMJIT_NO_UJIT`,
`-isystem third_party/asmjit`, `-I <repository root>`); none of these C sources includes AsmJit or tests those
macros. Option combinations (tests/bench/examples on and off, native on and off)
configure and generate without warnings; `SUTEKH_BUILD_TESTS=ON` alone still registers all 147 (164 native) tests.

The one intended top-level difference: a checkout with no bootstrapped or SDK Vulkan headers but system headers of
1.3.204 or later now builds the renderer instead of skipping it. In a fresh `git archive` of each side with no
third-party files on this machine (system headers 1.4.341), the branch adds `su_render`, ten `test_render_*`
programs, `space_demo` and their 15 tests and removes nothing (`nodeps-main-vs-branch.diff`); these are the same
targets main builds when headers are present. `-DSUTEKH_RENDER=OFF` gives the old headless result.

## Validation

Machine: i7-14700K, Intel Arc A770, Nobara 44 (Fedora 44). GCC 16.2.1, Clang 22.1.8, CMake 4.3.0 (and 3.21.4 from
PyPI), Ninja 1.13.2, Python 3.14.7, Mesa Vulkan driver (render tests draw, not skip).

- origin/main baseline, Debug GCC full: 147 tests, 0 failures, 1 skip (`audio_device`, no audio device; pre-existing).
- Branch Debug GCC full (`-Werror`), at `2461f20`: 147 / 0 failures / 1 skip (`audio_device`).
- Branch Release GCC full: 147 / 0 / 1 skip (`audio_device`). Run on the tree of `2461f20` before its last change
  (the `EXISTS` guard on the system-header path, which a build with the pinned headers does not reach).
- Branch native Debug GCC (`SUTEKH_NATIVE=ON`, render on), at `2461f20`: 164 / 0 / 2 skips (`native_unwind`, Windows
  only by design; `audio_device`).
- CMake 3.21.4, Clang 22 Debug full (top level): 147 / 0 / 1 skip (`audio_device`); same tree as the Release run.
- Consumer checks (all PASS): CMake 4.3 + GCC Debug (pinned deps in-tree), CMake 3.21.4 + GCC Debug (`--dest` deps,
  system Vulkan 1.4.341), CMake 3.21.4 + Clang Release (`--dest` deps including the pinned Vulkan headers), and the
  CI job reproduction above (3.21.4 Debug and 4.3 Ninja Release in a fresh clone).
- Consumer with `-DSUTEKH_BUILD_TESTS=ON`: 149 tests (147 Sutekh + 2 consumer), 0 failures, 1 skip
  (`audio_device`).
- Vulkan system fallback: Vulkan-Headers v1.3.204 and v1.3.275 (via `CMAKE_INCLUDE_PATH`, empty `SUTEKH_DEPS_DIR`) and
  the system 1.4.341: `su_render` builds (Release, GCC) and all 15 `render_*` tests pass, none skipped. A fake 1.3.200
  header is rejected with its version (AUTO: skipped; ON: configure error). A nonexistent
  `SUTEKH_VULKAN_SYSTEM_INCLUDE` is ignored.
- Bootstrap: `--dest` fetch and `--check` for all three scripts; the in-tree lock is not written; a non-pinned
  `DIR/vulkan-headers` is refused without `--force`. In-tree bootstrap and `--check` still pass.
- `python tools/check_repository_license.py`, `python tools/validate_pack.py --check-report` and `git diff --check`
  pass.

Source hashes (git trees at `2461f20`): `CMakeLists.txt` a43d7c67a023, `cmake/` 79a9b90de9d6, `tests/consumer/`
1b5ed8c8c4a1, `tools/` d290562ab5c2, `include/` 5227bb66a29e (unchanged), `src/` fb2918742664 (unchanged).

## Unrun and limitations

- Windows (clang-cl and MSVC) not built locally. Windows-only edits: `input_probe` moved under
  `SUTEKH_BUILD_EXAMPLES`, and `find_package(Python3)` is REQUIRED only with the tests (a consumer without Python
  can configure). The Windows CI jobs build the top level. The SYSTEM check in the driver is GCC/Clang only (MSVC
  needs `/external:I`, which CMake emits only for newer MSVC and CMake versions).
- GitHub Actions not waited for; the new `consumer` job was reproduced locally only.
- CMake 3.21.0 to 3.21.3 not tested (3.21.4 is the oldest tested). Vulkan headers between 1.2.x and 1.3.203 not tested.
- With `SUTEKH_BUILD_TESTS=ON` in a consumer, the Python tests still write `__pycache__/` (gitignored) into the Sutekh
  checkout, as at top level. Not in the issue's scope; tests are off by default for consumers.
- `SUTEKH_DEPS_DIR` does not cover AsmJit; `bootstrap_asmjit.py` has no `--dest`.
- `CMAKE_MSVC_RUNTIME_LIBRARY` (static CRT) is still set for Sutekh's directory on Windows when the consumer has not
  set it; a consumer using the DLL CRT should set it before `add_subdirectory`. Unchanged behavior, not tested.

## Blockers

None.

## Next

Review and CI. A Windows consumer run (MSVC `/external:I`, DLL CRT) would close the remaining platform gap.
