# Issue 141: cross-compiler and cross-OS determinism matrix for `fixed_q32_v1`

## State and scope

Branch `feat/issue-141-determinism`, based on `origin/main` at `07d06de`. Everything below was verified locally
on Linux. The Windows legs exist only in CI; section "CI" records what the PR's checks reported, and nothing about
Windows is claimed beyond it.

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

Run `<to be filled from the PR checks>`. Until this section names a run and its job results, no Windows result is
claimed.

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
  - anything on Windows: no machine is available;
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
