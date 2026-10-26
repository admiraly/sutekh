# Issue 143: submodule consumer build (options, Sutekh:: targets, include hygiene)

## State and scope

Locally verified on `feat/issue-143-consumer-build`, with origin/main `31192d9` (merge of PR 147) merged in for the
review round (see "Fix round" below). PR 150. Not merged.

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
   (tools) stay unconditional. PR 147's registered-operator tests (`test_host_registered{,_dll}`, the ten
   `host_registered*` tests and their `host_registered` scratch directory) are guarded the same way. Regions are wrapped without re-indentation (`endif() # SUTEKH_BUILD_TESTS`) so the
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
   set), then `VULKAN_SDK`, then, when `SUTEKH_VULKAN_SYSTEM` allows it, system headers of 1.3.204 or later
   (`SUTEKH_VULKAN_SYSTEM_MIN`, parsed from `VK_HEADER_VERSION_COMPLETE`/`VK_HEADER_VERSION`).
   `SUTEKH_VULKAN_SYSTEM=AUTO|ON|OFF`: AUTO (default) enables the system search only when Sutekh is not the top-level
   project, so top-level discovery is exactly as before. The search checks every candidate directory in order
   (`CMAKE_INCLUDE_PATH`, `CMAKE_PREFIX_PATH/include`, the compiler's implicit include directories,
   `CMAKE_SYSTEM_INCLUDE_PATH`, `CMAKE_SYSTEM_PREFIX_PATH/include`) and takes the first acceptable one; nothing is
   cached, so a rejected directory is re-checked on the next configure. `SUTEKH_VULKAN_SYSTEM_INCLUDE` names one
   directory to check instead. CMake never passes a compiler-implicit directory with `-I`, so of the implicit
   directories only the first one (in the compiler's order) holding `vulkan/vulkan_core.h` is what the compiler
   reads; a later implicit one is reported as shadowed rather than accepted (otherwise an old
   `/usr/local/include/vulkan` would be compiled while `/usr/include`'s version was checked). A non-implicit
   candidate (passed with `-I`) is not affected. 1.3.204 is the oldest release built and tested here (Ubuntu 22.04's
   package version); the renderer requests the Vulkan 1.2 API. The SDK path keeps its old behavior (no version
   check). Bootstrap destination: `--dest DIR` for `bootstrap_vulkan_headers.py`, `bootstrap_sdl3_headers.py` and
   `bootstrap_asset_deps.py`, with `-DSUTEKH_DEPS_DIR=DIR`. With `--dest` the tracked lock file is verified, never
   written, and an unusable `DIR/<name>` is replaced only with `--force` (the in-tree path keeps its old replace
   behavior). AsmJit (`SUTEKH_NATIVE`) stays in `third_party/asmjit`.
5. Messages: `skipped (SUTEKH_RENDER=OFF)`; without headers, every rejected system directory with its reason
   (`1.3.200, older than 1.3.204`; `version not readable: no VK_HEADER_VERSION_COMPLETE, so before 1.2.175`;
   `shadowed by ..., which the compiler searches first`) and the ways to get headers, naming
   `SUTEKH_VULKAN_SYSTEM_INCLUDE` (system search on) or `-DSUTEKH_VULKAN_SYSTEM=ON` (off, the top-level default).
6. `CMAKE_BINARY_DIR` → `CMAKE_CURRENT_BINARY_DIR` everywhere (identical at top level). In a consumer with
   `SUTEKH_BUILD_TESTS=ON`, all Sutekh tests pass and the consumer's top-level build directory receives nothing
   but ctest's own `Testing/`.
7. `SUTEKH_WERROR` defaults `${PROJECT_IS_TOP_LEVEL}` (`cmake/SutekhWarnings.cmake`). In a consumer, each public
   library gets `include/` as `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` (`-isystem`), and `Sutekh::vulkan_headers` is
   `SYSTEM INTERFACE` (unused in-repo). Top level is unchanged (engine tests keep `-I`). `su_native`'s own
   `-Wall -Wextra -Werror` (C and C++) now takes `-Werror` from `SUTEKH_WERROR` too (same flags and order at top
   level).
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
- `planning/evidence/consumer-build/` (new: `snapshot_build.py`, `snapshot-{full,headless,native,nodeps}.txt`)
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

`defaults` (the four options are OFF), `build`, `ctest-list` (exactly `consumer_host`, `consumer_public_headers`; any other test fails it),
`no-engine-tests` (CMake file API: the Sutekh subdirectory defines no executable except the tools `sutekh` and
`asset_cook`, and no `test_*`/`bench_*` target, so a test or program added later without a guard fails it), `internal-headers[src|src_platform|space3d]`
(each probe fails, and the failure is the missing header), `system-headers` (GCC/Clang: the Sutekh tree appears only
as `-isystem <sutekh>/include` or `-isystem` fetched `third_party/` headers), `system-headers[control]` (the same
public-headers compile with `-I` instead fails on `-Wpadded`, so the SYSTEM check is not vacuous), `run` (ctest),
`build-dir` (after the run, the consumer's top-level build directory holds only CMake's, the generator's, ctest's and
the consumer's own files: a Sutekh file there, e.g. from a `CMAKE_BINARY_DIR` path, fails it), `tree-unchanged` (no file of the checkout created, changed or removed; `build*/`, `out/`, `.venv/`, `.cache/`, `.git`
are not compared).

Mutation checks (driver run against modified copies of the tree):
- su_host's dependencies back to PUBLIC, the SYSTEM property removed, `SUTEKH_BUILD_TESTS` forced ON: FAIL for
  `defaults`, `build` (the `-Wpadded -Werror` TU), `ctest-list`, `no-engine-tests`, all three `internal-headers`,
  `system-headers`, `run` and `tree-unchanged` (Sutekh's Python tests write `__pycache__`).
- A future regression like PR 147's (an unguarded `add_executable(test_future_unguarded ...)` + `add_test`, and
  `file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/future_scratch")`): FAIL for `ctest-list`, `no-engine-tests` and
  `build-dir`; everything else passes.

The CI job `consumer` (ubuntu-latest) installs CMake 3.21.4 in a venv and the pinned CMake 4.3.4, installs the
distribution's `libvulkan-dev` (system-header fallback), bootstraps SDL3 and the asset deps with `--dest
$RUNNER_TEMP/deps`, runs the driver with 3.21.4 (Makefiles, Debug) and 4.3.4 (Ninja, Release) with `SUTEKH_RENDER=ON`,
then requires `git status --porcelain --ignored` to be empty. The whole job was reproduced locally in a fresh clone
of the branch (GCC 16, system Vulkan 1.4.341): all checks PASS, status clean.

## Before and after (top level)

`planning/evidence/consumer-build/snapshot_build.py` records targets (file API: name, type, artifacts), tests (names,
properties) and the generated `CTestTestfile.cmake` (full commands, built or not), with `<SRC>`/`<BLD>` paths.
Compared on origin/main (`31192d9`, `git archive` plus the same fetched third-party files) and the branch, Ninja,
Debug:

| configuration | targets | tests | snapshot diff |
| --- | --- | --- | --- |
| full (`SUTEKH_RENDER=ON SUTEKH_ASSET_IMPORT=ON`, SDL3) | 155 | 157 | none |
| headless (`SUTEKH_RENDER=OFF SUTEKH_WINDOW=OFF`) | 140 | 137 | none |
| native (`SUTEKH_NATIVE=ON SUTEKH_RENDER=ON`) | 169 | 174 | none |
| no third-party files (fresh `git archive`, defaults; system Vulkan 1.4.341 installed) | 137 | 136 | none |

The snapshots are in the evidence directory. The last row is the case the system-header fallback could have
changed; with `SUTEKH_VULKAN_SYSTEM=AUTO` a top-level build still skips the renderer there, as on main. Also
compared, full and native: link lines (`LINK_LIBRARIES` of every linked target in `build.ninja`) identical;
`compile_commands.json` (258 and 344 lines) identical except 70 compile lines in each, which only lose flags (none
added, none reordered). Full: the leaked include directories `-I src/platform`, `-I src`,
`-I profiles/space3d/native`. Native: the same, and on the C targets that link `su_host` (`test_host`, `test_clock`,
`test_replay`, `test_input`, `test_host03`, `test_host04`, `test_host_ops`, `test_host_rollback`, `bench_host`,
`bench_host_rollback`, `space_demo`) also AsmJit's usage requirements that leaked through `su_host PUBLIC su_native`
(`-DASMJIT_STATIC -DASMJIT_NO_AARCH64 -DASMJIT_NO_UJIT`, `-isystem third_party/asmjit`, `-I <repository root>`);
none of these C sources includes AsmJit or tests those macros (the review confirmed equal preprocessed output).

Guard audit: the top level configured with `SUTEKH_BUILD_TESTS=OFF SUTEKH_BUILD_BENCH=OFF SUTEKH_BUILD_EXAMPLES=OFF`
(render on; native off and on) registers 0 tests, generates no `CTestTestfile.cmake`, and defines no executable but
`sutekh` and `asset_cook` and no `test_*`/`bench_*` target. No `CMAKE_BINARY_DIR` or `CMAKE_SOURCE_DIR` remains in
`CMakeLists.txt`, `cmake/` or `tests/consumer/`. Option combinations (tests/bench/examples on and off, native on and
off) configure and generate without warnings (first round).

## Validation

Machine: i7-14700K, Intel Arc A770, Nobara 44 (Fedora 44). GCC 16.2.1, Clang 22.1.8, CMake 4.3.0 (and 3.21.4 from
PyPI), Ninja 1.13.2, Python 3.14.7, Mesa Vulkan driver (render tests draw, not skip). All runs below are of the
fix-round tree (origin/main `31192d9` merged in); builds and scratch outside the checkout.

- origin/main baseline (`31192d9`), Debug GCC full: 157 tests, 0 failures, 1 skip (`audio_device`, no audio device;
  pre-existing).
- Branch Debug GCC full (`-Werror`): 157 / 0 failures / 1 skip (`audio_device`).
- Branch Release GCC full: 157 / 0 / 1 skip (`audio_device`).
- Branch native Debug GCC (`SUTEKH_NATIVE=ON`, render on): 174 / 0 / 2 skips (`native_unwind`, Windows only by
  design; `audio_device`).
- CMake 3.21.4, Clang 22 Debug full (top level): 157 / 0 / 1 skip (`audio_device`).
- Consumer checks (all PASS, including `build-dir`): CMake 4.3 + GCC Ninja Debug (pinned deps in-tree), CMake 3.21.4 +
  GCC Debug (`--dest` deps, system Vulkan 1.4.341 through AUTO), CMake 3.21.4 + Clang Release (`--dest` deps including
  the pinned Vulkan headers), and the CI job reproduction in a fresh clone (3.21.4 Debug and 4.3 Ninja Release;
  `git status --porcelain --ignored` empty).
- Consumer with `-DSUTEKH_BUILD_TESTS=ON`: 159 tests (157 Sutekh + 2 consumer), 0 failures, 1 skip
  (`audio_device`); the consumer's top-level build directory gains only ctest's `Testing/` (no `host_registered`).
- Vulkan discovery (configure): top level with no headers skips the renderer and suggests `-DSUTEKH_VULKAN_SYSTEM=ON`;
  `SUTEKH_VULKAN_SYSTEM=ON` finds 1.4.341; an old 1.3.200 header in a compiler-implicit directory ahead of
  `/usr/include` (`C_INCLUDE_PATH`) rejects both, the second as shadowed; the same old header in `CMAKE_INCLUDE_PATH`
  (passed with `-I`) is rejected and `/usr/include` 1.4.341 is used; a header without `VK_HEADER_VERSION_COMPLETE`
  is reported as "version not readable"; `SUTEKH_RENDER=ON` without headers is a configure error with the hint.
  First round (same code path for accepted headers): Vulkan-Headers v1.3.204, v1.3.275 and the system 1.4.341 each
  build `su_render` and pass all 15 `render_*` tests, none skipped.
- `su_native` with `SUTEKH_WERROR=OFF`: no `-Werror` on its C or C++ sources; with ON (top level) the same flags and
  order as before.
- Bootstrap: `--dest` fetch and `--check` for all three scripts (rerun in the fix round); the in-tree lock is not
  written; a non-pinned `DIR/vulkan-headers` is refused without `--force` (first round).
- `python tools/check_repository_license.py`, `python tools/validate_pack.py --check-report` and `git diff --check`
  pass.
- Source: `include/` and `src/` are those of `31192d9` (this PR changes neither).

## Unrun and limitations

- Windows (clang-cl and MSVC) not built locally. Windows-only edits: `input_probe` moved under
  `SUTEKH_BUILD_EXAMPLES`, and `find_package(Python3)` is REQUIRED only with the tests (a consumer without Python
  can configure). The Windows CI jobs build the top level. The SYSTEM check in the driver is GCC/Clang only (MSVC
  needs `/external:I`, which CMake emits only for newer MSVC and CMake versions).
- GitHub Actions not waited for; the new `consumer` job was reproduced locally only.
- CMake 3.21.0 to 3.21.3 not tested (3.21.4 is the oldest tested). Vulkan headers between 1.2.x and 1.3.203 not tested.
- The render build against v1.3.204/v1.3.275 was not repeated in the fix round (the accepted-header path is
  unchanged; only discovery changed, and it was rechecked at configure).
- With `SUTEKH_BUILD_TESTS=ON` in a consumer, the Python tests still write `__pycache__/` (gitignored) into the Sutekh
  checkout, as at top level. Not in the issue's scope; tests are off by default for consumers.
- `SUTEKH_DEPS_DIR` does not cover AsmJit; `bootstrap_asmjit.py` has no `--dest`.
- `CMAKE_MSVC_RUNTIME_LIBRARY` (static CRT) is still set for Sutekh's directory on Windows when the consumer has not
  set it; a consumer using the DLL CRT should set it before `add_subdirectory`. Unchanged behavior, not tested.

## Fix round (review of PR 150)

The review passed everything except the base: main had moved to `31192d9` (PR 147).

1. Merged origin/main `31192d9` into the branch (a merge rather than a rebase, so the PR branch needs no
   force-push; the resulting tree is the one verified). PR 147's registered-operator block in `cmake/SutekhHostOps.cmake`
   (`test_host_registered{,_dll}`, ten `host_registered*` tests, `file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/host_registered")`)
   is now under `SUTEKH_BUILD_TESTS` and uses `CMAKE_CURRENT_BINARY_DIR`; the guard audit covers the whole tree. The
   consumer driver no longer relies on a fixed program list: `no-engine-tests` reads the CMake file API (any Sutekh
   executable but the tools, or any `test_*`/`bench_*` target, fails), `ctest-list` fails on any test but the
   consumer's, and the new `build-dir` check fails on any unexpected entry in the consumer's top-level build
   directory. A mutant reproducing PR 147's pattern fails all three. Evidence snapshots regenerated on the new base;
   a no-third-party snapshot added; `nodeps-main-vs-branch.diff` removed (now empty).
2. `SUTEKH_VULKAN_SYSTEM=AUTO|ON|OFF`; AUTO = only when not top level. Top-level discovery is as before (last row of
   the table).
3. Version check: separate messages for an unreadable and an old version; every candidate directory is checked (no
   `find_path`, nothing cached); shadowed implicit directories are reported; the hint names
   `SUTEKH_VULKAN_SYSTEM_INCLUDE` (or `-DSUTEKH_VULKAN_SYSTEM=ON` when the search is off).
4. `su_native`'s hard-coded `-Werror` follows `SUTEKH_WERROR`.

## Blockers

None.

## Next

Review and CI. A Windows consumer run (MSVC `/external:I`, DLL CRT) would close the remaining platform gap.
