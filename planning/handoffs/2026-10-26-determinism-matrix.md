# Issue 141: cross-compiler and cross-OS determinism matrix for `fixed_q32_v1`

## State and scope

Branch `feat/issue-141-determinism`, based on `origin/main` at `07d06de`. Linux results were verified locally. The
Windows legs exist only in CI, and section "CI" records exactly what those jobs reported; nothing about Windows is
claimed beyond it.

Exact modified paths:

- `.github/workflows/repository-checks.yml`: new jobs `determinism-linux`, `determinism-windows` and
  `determinism-compare`.
- `CMakeLists.txt`: includes `cmake/SutekhDeterminism.cmake`.
- `cmake/SutekhDeterminism.cmake` (new): `det_matrix` and `det_matrix_dll`, `test_det_regress`, and the ctests
  `determinism_*`.
- `tests/determinism/` (new):
  - `det_matrix.c`, `gen_workloads.py`, `compare_manifests.py`, `rts_engine_golden.py`, `test_det_regress.c`;
  - `sixdof.{capsule,lir}.json`, `churn.{capsule,lir}.json`, `rts/{damage,goal,combat,move}.capsule.json`;
  - `expected_digests.txt`, `rts_engine_golden.rtsreplay`, `regress_space3d_half_min.inc`.
- `profiles/space3d/native/space3d_geom.c`: finding D1, `sign_mul`.
- `docs/DETERMINISM_MATRIX.md` (new), `docs/DETERMINISM_AND_BFME.md`, `docs/NUMERIC_FIXED_Q32.md`.
- `PACK_VALIDATION.json`: regenerated.
- `planning/handoffs/2026-10-26-determinism-matrix.md`.

The claim, the workloads, the audit table and the findings are in `docs/DETERMINISM_MATRIX.md`. In short:

- Five workloads go through the public host API: `rts_duel16`, `rts_mixed1024`, `racer16`, `sixdof` and `churn`.
- Each writes per-keyframe `su_host_digest`, a SHA-256 over every per-tick digest, the fast digest, the event cursor
  and the replay-bytes SHA-256 to a manifest.
- Every build and tier must reproduce the committed `expected_digests.txt` (Linux GCC 16.2.1 Release, reference
  tier).
- The full six-system RTS world (native `rts.spatial` and `rts.scatter`) is checked through `sutekh serve` against
  a committed engine recording (`determinism_rts_engine`).

Conservative choices and their reasons:

- **RTS through the host API.** The RTS operators are deliberately absent from the host API (process-wide scratch),
  so the host-API RTS workloads run the four IR systems. Copies of the capsules have the edges to the operators
  removed (generated, `--check`ed), and the host stands in for the operators with input records. The real
  operators are covered by the engine recording.
- **`churn` is not replayed.** Host spawns are not part of `SUREPLAY`, so `churn` compares per-tick digests only.
- **Contract headers unchanged.** No frozen contract header (`include/sutekh/*.h`) was edited; the claim lives in
  the docs.
- **D1 fix.** It keeps the value every compiler produced before (wrapping product), so no defined result and no
  pinned digest changed.
- **D4 left alone.** The VM tier globals are documented, not changed: the file belongs to the VM owner and the race
  is tier-neutral.

## Fix round after the independent review (2026-10-26)

The review found no divergence. It failed the PR on scope: `docs/DETERMINISM_MATRIX.md` section 1 claimed more than
the matrix tested. Each point was either covered by new tests or removed from the claim. Points in the review's
order:

1. **space3d.**
   - Four new workloads: `s3_query`, `s3_prox`, `s3_shape` and `s3_compound`, one per operator variant.
     - Each has 96 sphere, capsule or compound movers in a 24 m arena.
     - The obstacles are spheres, rotating boxes, rotating capsules, rotating meshes from operator data, gates and a
       floor box, plus three D1-style boxes.
     - `det.s3move` / `det.s3move_c` bounce the movers on contacts 0 and 1, so the operator's results feed motion.
   - Activity is pinned in the expectation: 10,780 to 17,234 contacts per 600 ticks, proximity hits, sub-shape
     contacts and truncation flags.
   - The reviewer's mutation (libm `sqrt` in `ray_sphere`) is now caught by all four workloads at tick 60
     (`run_mutants.py`).
   - Two exclusions were measured with an instrumented build: `INT64_MIN` extents never reach the narrow phase, while
     −1 m extents reach it in 6 to 2,650 sweeps per run. Both are stated in section 1.
2. **Operation coverage.**
   - `probe04` (`det.bits04` and `det.gather04`, IR 0.4 with a row filter) runs the 45 missing operations with edge
     operands.
   - `determinism_op_coverage` reads the table from the library (`det_op_list`) and fails on any operation without a
     live use in a matrix workload. Live means its result reaches a store, an event or a despawn; there is a
     self-test for this.
   - Coverage is now 110 of 110. The overclaim at old lines 87-89 was corrected, and IR 0.4's status is stated in
     section 1.
3. **Native fallback.** `tests/determinism/expected_tiers.txt` pins each workload's per-system placement in each
   tier mode (`--expect-tiers`, every matrix ctest). A copy that expects `det.sixdof=planned` under native fails
   with `PLACEMENT MISMATCH`.
4. **RTS operators.**
   - Section 1 lists the limits.
   - The engine recording gained a congested blob of 200 armed units: `truncated_rows` is 200 for 20 ticks.
   - It was re-recorded with `CAP` 512, at most 420 rows (still one chunk).
   - A native-tier serve run was not added (optional).
5. **Tiers and CI manifests.**
   - The claim now says "uniform per-run tiers and the mixed placement".
   - `--tier mixed` cycles native / reference / planned per system (`mixed_vm` without native); ctest
     `determinism_matrix_mixed`.
   - CI writes and compares DLL manifests for every tier mode.
6. **Cross-chunk merge.** `churn3k`: 3,072 slots, about 2,800 live entities, host spawns and despawns, IR deaths and
   events. A reversed merge order is caught by `churn3k` and not by `churn` (one chunk), which shows the merge is now
   reached.
7. **Rollback.** Every workload, churn included, restores the snapshot of a mid-run tick off a keyframe, in place
   and into a fresh world on another tier. It re-simulates the same commands from the saved generator state, and
   every per-tick digest must match. This runs in every matrix ctest.
8. **Compiler pinning.**
   - `tests/determinism/ci_compilers.txt` lists every CI leg's compiler.
   - `-DSUTEKH_DET_COMPILER_LEG=<leg>` makes the configure step fail on any other compiler.
   - The file's header says it changes only deliberately.
   - `det_matrix` also prints the compiler that built it.
9. **Cross-playback directions.**
   - Each Windows leg plays the Linux GCC and Linux Clang replays.
   - The new job `determinism-play-windows-on-linux` plays every Windows leg's replays on Linux GCC, in all four tier
     modes.
10. **Nits.**
    - `play_foreign` checks the played tick count, the end tick, the keyframe count and the digest count.
    - The RTS stand-in's distance is computed in `uint64_t` from absolute differences.
    - `tests/determinism/fixtures/racer16.json` is committed, and `determinism_fixture_racer_committed` checks that
      `racer_microcosm` still dumps it byte for byte.
    - D1: the wrapping product against the saturating negation is documented at `sign_mul` and at the slab test in
      `space3d_geom.c`, and in section 5. Half extents stay unvalidated, because validating them would change the
      operator's semantics.

New and changed paths in this round:

- `tests/determinism/`:
  - `det_matrix.c`, `gen_workloads.py`, `rts_engine_golden.py` and `rts_engine_golden.rtsreplay`;
  - `expected_digests.txt` (11 workloads, 191 lines);
  - new: `expected_tiers.txt`, `ci_compilers.txt`, `check_op_coverage.py`, `det_op_list.c`, `run_mutants.py`,
    `fixtures/racer16.json`, `{s3move,s3move_c,bits04,gather04}.{capsule,lir}.json`.
- `cmake/SutekhDeterminism.cmake`, `.github/workflows/repository-checks.yml`.
- `profiles/space3d/native/space3d_geom.c` (comments only).
- `docs/DETERMINISM_MATRIX.md` (rewritten section 1, workloads, results, D5), `docs/DETERMINISM_AND_BFME.md`,
  `docs/NUMERIC_FIXED_Q32.md`.
- `PACK_VALIDATION.json`.

Local validation of this round:

- **Builds.** Same machine and toolchains as below. The build directories moved to a second disk because
  the root filesystem ran out of space; the full suites were re-run there from fresh configures.
- **Matrix.** Eight builds: GCC/Clang Release with native, GCC Debug with native, Clang Debug, Clang `-march=native`,
  GCC portable, GCC `-ffast-math`, and Clang UBSan with native. On every tier mode each build has (reference,
  planned, mixed, and native where built):
  - the manifest SHA-256 was `3ce716273cc36c7b682847da0be8dedf803620a6efcc8368a3e1909450e03925`, equal to the
    expectation;
  - placements equal `expected_tiers.txt`;
  - the rollback check passed for 11 of 11 workloads.
- **Per build.** The DLL (planned) passed. Each build played GCC's nine replays (reference, planned), the GCC DLL
  played each build's replays, and every replay file was byte-identical to GCC's. `test_det_regress` gave 108/108,
  op coverage 110/110, and `rts_engine_golden` matched 241/241.
- **Full ctest on fresh builds.** GCC Release native, Clang Release native, and Clang UBSan (halt on error): 156
  tests each, 0 failures, 2 skips (`native_unwind`, `audio_device`). The UBSan LastTest.log has 0 `runtime error`
  lines.
- **Mutation set.** `run_mutants.py` on the final source: 6 of 6 caught (table in `docs/DETERMINISM_MATRIX.md`
  section 2).
- **After merging `origin/main` (`31192d9`, registered host operators and the Linux window).** The PR was conflicting
  in `PACK_VALIDATION.json` only, which was regenerated. GCC Release native, Clang Release native and Clang UBSan
  were rebuilt: full ctest 167 tests each, 0 failures, the same 2 skips, and 0 `runtime error` lines. The matrix
  ctests passed against the unchanged expectations, so main's changes did not move any digest.
- **Second merge of `origin/main` (`70ce82d`, consumer build).** Main now gates tests behind `SUTEKH_BUILD_TESTS`, so
  `cmake/SutekhDeterminism.cmake` returns early when tests are off (its compiler-leg check still runs). A configure
  with tests, bench and examples off succeeds. GCC, Clang and UBSan were rebuilt: full ctest 167/167 each, the same 2
  skips, 0 UBSan runtime errors, and 14 `determinism_*` tests registered.

Source hashes of this round:

| File | SHA-256 |
|---|---|
| `tests/determinism/det_matrix.c` | `276c4153dedc2844d367f05b8270dbec3055e89b1e8ea02ecd6db02ed9cdb055` |
| `tests/determinism/expected_digests.txt` | `b31a09332321d255c78eeeaf55dc9ea6acbb74d1810a4e55dc034285dc71aeab` |
| `tests/determinism/expected_tiers.txt` | `5f6407e469bd7db0be3891bab51a532fb4702a58644664c70643b911bc8a4176` |
| `tests/determinism/rts_engine_golden.rtsreplay` | `a914e83028ac925e21e02c1a3c1c4200ea6bb98adf2736397e5fa0d3b324f103` |
| `tests/determinism/fixtures/racer16.json` | `f438f286f961752b6e636d3ef8e48df8abdc56f947f299c2cefe853271b4e4c1` |
| `tests/determinism/bits04.lir.json` | `5d34e6d88049eb39c6e99d211dc90939e2d8ed5c8aeeba4ebc6c598144597aff` |
| `tests/determinism/gather04.lir.json` | `f58cddf948af2e7c893e7a6c17b0de985c0512fe0007aeb359955a557daf6124` |
| `tests/determinism/s3move.lir.json` | `1f7621175904e5be6cf82e1aa452575747f85bbf26a1720381aefb61c39a23af` |
| `tests/determinism/s3move_c.lir.json` | `68ac7ad20cef8e9809f9580bcfcea9fbf591d8974fe87b553c02453fa0f0e96d` |
| `profiles/space3d/native/space3d_geom.c` | `c07355afe27360ed9f5dd67aee4a74d657f9844fd87ea346fad2e2462dc40758` |

CI of this round: see section "CI", "Fix round".

The sections below record the first round as it was. Its manifest hash `4dee8c73…` and the five-workload counts are
superseded.

## Local validation

Machine and tools:

- Intel Core i7-14700K, Linux 7.2.9-200.nobara.fc44.x86_64, glibc 2.43.
- GCC 16.2.1 20260819 (Red Hat 16.2.1-2), clang 22.1.8 (Fedora 22.1.8-4.fc44).
- CMake 4.3.0, Ninja 1.13.2, Python 3.14.7.
- AsmJit at the pinned commit (`tools/bootstrap_asmjit.py`, `--check` ok).

Builds: each was configured with `cmake -S . -B /tmp/wt-141-<n> -G Ninja -DCMAKE_BUILD_TYPE=<bt>
-DSUTEKH_NATIVE=<on/off> [-DCMAKE_C_FLAGS=...]`, built with `-j 6`, and used `-Wall -Wextra -pedantic -Werror`
throughout.

| `<n>` | Compiler | Build type | Native | Extra C flags |
|---|---|---|---|---|
| `gcc` | gcc | Release | ON | |
| `clang` | clang | Release | ON | |
| `gcc-debug` | gcc | Debug | ON | |
| `clang-debug` | clang | Debug | OFF | |
| `clang-march` | clang | Release | OFF | `-march=native` |
| `gcc-portable` | gcc | Release | OFF | `-DSU_NUM_FAST_PORTABLE` |
| `gcc-fastmath` | gcc | Release | OFF | `-ffast-math -march=native` |
| `ubsan` | clang | RelWithDebInfo | ON | `-fsanitize=undefined -fno-sanitize=function -fno-sanitize-recover=all` (C and C++) |

Commands (repository root; `F` = the build's `det_fixtures`, written by `gen_workloads.py fixtures` and
`racer_microcosm --dump-fixture`):

```sh
python3 -I tests/determinism/gen_workloads.py --check                   # committed capsules = generator
<b>/det_matrix --fixtures $F --tier {reference,planned,native} --expect tests/determinism/expected_digests.txt --out m.txt
<b>/det_matrix_dll --fixtures $F --tier planned --expect tests/determinism/expected_digests.txt
<b>/det_matrix --fixtures $F --tier {reference,native} --play-dir <other build>/det_fixtures   # cross-compiler playback
<b>/test_det_regress
python3 -I tests/determinism/rts_engine_golden.py check <b>/sutekh
ctest --test-dir <b> -j 6                                               # full suite
```

Results:

- **Matrix.** Every build in the table, on every tier it has, wrote a manifest with SHA-256
  `4dee8c734aa29da4986aef324d4dd2992371cede603f303b9d887609b1a245cc`, equal to the expectation. The DLL variant
  passed on every build.
- **Native placement.** On native, `rts.goal`, `rts.combat`, `rts.move`, `racer.move` and `det.sixdof` ran as native
  kernels. `rts.damage`, `racer.race` and `det.churn` (entity operations) stayed planned, and `space3d.query` ran as
  an operator.
- **Cross-compiler playback.** Clang Release played GCC's four replays on the reference and native tiers, and the GCC
  DLL played Clang's: every digest matched. The replay files of GCC and Clang are byte-identical (`cmp`).
- **Regression fixture and RTS engine.** `test_det_regress` reported 108 known answers and 0 mismatches on every
  build. `rts_engine_golden.py check` matched all 241 digest records on every build.
- **Full ctest.** `gcc`, `clang` and `ubsan`: 152 tests, 0 failures, 2 skipped (`native_unwind`, Windows-only;
  `audio_device`, no device). The `ubsan` LastTest.log contains 0 `runtime error` lines.
- **Mutation check.** One hex digit of `sixdof kf 600` was changed in a copy of the expectation. `det_matrix` exited
  1 with `MISMATCH`, printed both values, and named `sixdof tick 600` as the first differing keyframe.
- **Repository checks.** `python3 tools/validate_pack.py --write-report`, then `--check-report`: passed.
  `python3 -I tests/tools/test_validate_pack.py`: OK. `python3 tools/check_repository_license.py`: passed.

Findings and how each was established:

- **D1, UB found and fixed.**
  - Found by the audit's reading of the space3d sources and confirmed by UBSan on the pre-fix code:
    `space3d_geom.c:269:33: runtime error: signed integer overflow: -1 * -9223372036854775808`.
  - The 108 known answers were printed by `test_det_regress --print`, built against the pre-fix
    `libsu_space3d.a` of GCC `-O3` and Clang `-O3`. Both outputs are identical (`cmp`).
  - After the fix: GCC, Clang and UBSan all pass, and UBSan is silent.
- **D2, unspecified evaluation order.** Found in the driver while reviewing it before the expectation was
  committed. Two random draws sat in one expression. They were rewritten as sequenced statements in the same
  left-to-right order, and the manifest was unchanged (GCC and Clang had both evaluated left to right).
- **D3, sanitizer artifact.** The first UBSan build (`-fsanitize=undefined` alone) faulted in
  `su_native_run_checked` in `native_scalar`, `native_avx2`, `native_fault`, `ir03_native_diff`,
  `ir04_native_diff` and `determinism_matrix_native`. gdb showed `cmpl $0xc105cafe,-0x8(%rsi)` before `call *%rsi`:
  clang's `-fsanitize=function` reads the signature word in front of the JIT entry, which sits at a page start. A
  non-sanitized Clang Release build passes the same tests. With `-fno-sanitize=function` the whole suite passes.
- **D4, data race (audit).** Read in `src/vm/su_vm.c:73-96`; documented only.
- **windows-msvc helpers.** Checked by cross-compiling the audit's probe with `clang --target=x86_64-pc-windows-msvc
  -O0/-O2` (stub `math.h`) and `llvm-nm -u`.

## CI

PR #145. Observed from the job logs (`gh api .../actions/jobs/<id>/logs`), not only from the check badges.

**Run 40261543242, commit `9394d19`.** Every job passed except `determinism-windows` "clang native", whose configure
step failed: pwsh did not expand `-DCMAKE_CXX_COMPILER=$cxx`. Fixed in `bd24204`, which quotes the argument.

**Run 40262502027, commit `bd24204`.** All 13 jobs passed.

- **`determinism-linux`.** GCC 15.2.0 and Ubuntu clang 21.1.8, Release, native: 10/10 determinism ctests each, then
  manifests on three tiers.
- **`determinism-windows`.**
  - clang 20.1.8 (msvc target), Release: 9/9 determinism ctests. Linux GCC replays played on reference and planned,
    every digest matched.
  - clang 20.1.8 with native: 10/10, including `determinism_matrix_native`. `rts.goal`, `rts.combat`, `rts.move`,
    `racer.move` and `det.sixdof` ran as native kernels. Linux GCC replays played on three tiers, every digest
    matched.
  - MSVC 19.51.36260, Release: 9/9. Linux GCC replays played on reference and planned, every digest matched.
- **`determinism-compare`.** "13 of 13 manifests equal tests/determinism/expected_digests.txt".
- **Existing suites with the new ctests.**
  - `gcc16` (GCC 16.2.0 Release): 146/146.
  - `runtime` (Debug): 146/146, twice.
  - `runtime-native`: 152/152.
  - `runtime-windows` (clang Debug): 150/150.
  - `runtime-windows-msvc` (Debug): 136/136.
  - `validate` (Linux, Windows): pass.

The final head of the first round (`7bc562a`, docs only) passed all 13 jobs (run 40263702485).

**Fix round, run 40271629352, commit `16c99ca` (the fix round merged with `origin/main`).** All 14 jobs passed.
Observed in the job logs:

- **Compiler pins.** Every leg printed its pin and passed it: `linux-gcc` GNU 15.2.0, `linux-clang` Clang 21.1.8,
  `windows-clang` and `windows-clang-native` Clang 20.1.8, `windows-msvc` MSVC 19.51.36260.0.
- **`determinism-linux`.** gcc and clang: 14/14 determinism ctests each. These include the matrix in the reference,
  planned, mixed and native modes with the placement comparison and rollback, the DLL, op coverage and its
  self-test, the committed racer fixture, `determinism_regress` and `determinism_rts_engine`.
- **`determinism-windows`.**
  - clang: 13/13 ctests.
  - clang native: 14/14.
  - MSVC: 13/13.
  - Every leg played the nine Linux GCC and nine Linux Clang replays on each tier mode it has: 54, 72 and 54
    "every digest matches" lines.
- **`determinism-play-windows-on-linux`.** 108 of 108 playbacks matched: the nine replays of each of the three
  Windows legs, in four tier modes, on Linux GCC 15.2 with native.
- **`determinism-compare`.** "36 of 36 manifests equal tests/determinism/expected_digests.txt": static and DLL, every
  tier mode, every leg.
- **Existing jobs.** `gcc16`, `runtime`, `runtime-native`, `runtime-windows`, `runtime-windows-msvc` and `validate`
  passed.

Commit `3bc4244` (the fix round before the merge) ran no checks: GitHub reported the PR as conflicting, in
`PACK_VALIDATION.json` only. Two later heads run in the PR's checks, not recorded here: the docs commit that
records this section, and a second merge of `origin/main`.

## SHA-256 evidence

| File | SHA-256 |
|---|---|
| `profiles/space3d/native/space3d_geom.c` | `078807fa3ec3cdc12801fad6698a82752d051a2969fd6444667fb01ab343b227` |
| `tests/determinism/det_matrix.c` | `d7b1a11b009893bc364bc0979f22f66a34dcab5c063a858923b2445c80d0fc9c` |
| `tests/determinism/expected_digests.txt` | `55e39bfe58d0f1a87f971f2d68fe574d4796c1b10e83f700a3ff5e34b897aec0` |
| manifest content (all local builds and tiers) | `4dee8c734aa29da4986aef324d4dd2992371cede603f303b9d887609b1a245cc` |
| `tests/determinism/rts_engine_golden.rtsreplay` | `2296d35bec3663eb52f6d3d53a94307f36f0cb0f476fa3f21d1988fc2abdbcbe` |
| `tests/determinism/sixdof.lir.json` | `5e4fb174c642a7d419059dcb38a06585e79d17c7cb90766457ff8adfabec9893` |
| `tests/determinism/churn.lir.json` | `bc7b6fb391fc735761c6a3900c66d310a5a0e38b3d124b603f15101bb1169ad6` |
| `tests/determinism/regress_space3d_half_min.inc` | `cd536b50fc947f8dc61a9fa05746ec116234dce59262ee0948b331b5754af1a7` |
| GCC Release `det_matrix` | `9580bc8dd29f33e5577776f2bcfa816870bb5cdba87adc51b55e14d79410cbd5` |
| GCC Release `libsutekh_host.so` | `c536e57847941a6383fae700ec0b9baebf5bc42211a06e1bfa9a755732cc255d` |
| Clang Release `det_matrix` | `a6bd296ebef34579c7744b774bcf84daeb588d092309c5254ff409bafd8833ef` |
| Clang Release `libsutekh_host.so` | `e6c47bf0b0b9f6decb4d1adb1464a429ce3d32d2d6e1f96fd1aba9fe0ff20eba` |

Source hashes are of the files as committed; the binaries were built from them.

## Unrun and next

- **Not run locally:**
  - anything on Windows: no machine is available; the CI legs above are the only Windows evidence;
  - Valgrind or MemorySanitizer: not installed or not practical;
  - ARM64, macOS and 32-bit;
  - the native tier under MSVC `cl`: not supported by the build.
- **Not covered by the matrix:**
  - the space3d query variants other than `space3d.query` (`query_prox`, `query_shape`, `query_compound`, static
    meshes); their own pinned-digest tests run on every CI leg (`example_arena_host*`, `host_ops`);
  - rendering and audio.
- **Next:**
  - make the `su_vm.c` tier globals atomic (VM owner);
  - add host spawn/despawn records to a future replay format version, so `churn`-style sessions can be replayed;
  - extend the matrix to ARM64 once a runner exists.
