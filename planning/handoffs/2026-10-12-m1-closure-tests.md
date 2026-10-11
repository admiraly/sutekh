# M1 closure tests (handoff 2026-10-12, m1-closure-tests)

Scope of this patch: parts A (kill the audit's surviving mutants) and C (ASan false red in the churn test).
Parts B, TX5, TX6 and the C07 assertions are NOT done. They are left for a separate worker.

## Status

- A: done. Mutants H1, L1, L2 (from the audit) plus S1, S3, S6, W2, P1 are each killed by a ctest selection, in a scratch copy.
- C: done. `specialize_churn` gets `ASAN_OPTIONS=quarantine_size_mb=1` from CMake (kept if already set). Passes under ASan+UBSan with no manual env.
- B (chunked scheduler in the live loop, SUTEKH_TICK_WORKERS, effect check at world.create, N in {1,2,4} equivalence, 100,001-row world, failing-chunk test, mutants B/D, TSan): NOT DONE.
- TX5 "Chunked tick workers" and TX6 "Large world create/step/snapshot/restore": NOT DONE.
- C07 assertions (RLIMIT_AS not unlimited in the worker, SECRET_X absent from the worker environ): NOT DONE.
- Not run: native clang ctest, TSan, full harness (`run_m0.py`), native ASan+UBSan of the whole tree (only the four touched suites were run), the default-OFF harness run.
- No push. Commits are on the worktree branch.

## Changed paths

- `src/runtime/worker.c`: new test seam `SU_TEST_FAULT=perturb_trust` (compiled only under `SU_TEST_FAULT_HOOKS`). It hands `su_nv_trust_admitted` a perturbed hash. The equality check before it refuses first, so this reaches the second check directly. Release builds are unchanged.
- `tests/native/qualification/test_specialize_admission.c`: (a) admit-level refusals with a mismatching `expected_layout_generation` and with a zero `expected_semantic_hash` (both expect `SU_E_SCHEMA_MISMATCH`); (b) `scenario_trust_mismatch` (expects `SU_E_CANDIDATE_FAILED`, worker unchanged).
- `tests/native/qualification/run_mutants.sh` (new): applies one sed substitution per mutant to a scratch copy, checks the anchor occurs exactly once and the file changed, configures and builds with the native CMake warning set, then runs a ctest selection. Usage: `sh tests/native/qualification/run_mutants.sh <scratch>`; `MUTANT_ONLY="L2 P1"` runs a subset.
- `CMakeLists.txt`: `specialize_churn` ENVIRONMENT property `ASAN_OPTIONS=quarantine_size_mb=1`.

## Correction to the audit

The audit (planning/audit/M1-audit.md, section 4 and D2) calls L1 and L2 "install-time" checks. They are not. Both are in `h_internal_specialize_admit` (worker.c around the layout and semantic checks, lines ~2233 and ~2249), i.e. the disposable admission path. The install path (`h_internal_specialize_install`) has its own semantic and layout checks (~2472, ~2476), and the artifact-hash equality (~2500) runs before `su_nv_trust_admitted`. That is why H1 looked equivalent: it is defence in depth behind the ~2500 check, so it needed the new trust seam to be reachable.

## Mutation results (scratch copies, native gcc Release, `-Werror`)

Each mutant built and was run with the ctest selection shown.

| Mutant | Anchor (file) | Selection | Result |
|---|---|---|---|
| H1 artifact-hash equality in trust | native_versions.c | specialize_admission | killed (1/1 failed) |
| L1 layout check at admission | worker.c | specialize_admission | killed (1/1 failed) |
| L2 semantic check at admission | worker.c | specialize_admission | killed (1/1 failed) |
| S1 scalar ADD emits sub | su_native_asmjit.cpp | native_scalar\|native_avx2 | killed (2/4 failed) |
| S3 scalar MUL emits add | su_native_asmjit.cpp | native_scalar\|native_avx2 | killed (2/4 failed) |
| S6 scalar EQ inverted | su_native_asmjit.cpp | native_scalar\|native_avx2 | killed (2/4 failed) |
| W2 wrapper layout-generation check | su_native.c | native_scalar\|native_avx2 | killed (2/4 failed) |
| P1 final mapping RWX | su_native.c | native_scalar\|native_versions\|specialize_churn | killed (3/4 failed) |

Notes on the run history:
- The first P1 mutant (RWX at the initial mmap) SURVIVED. The later mprotect to R+X makes the final mapping R+X, so the mutant was equivalent in its end state. The mutant was changed to weaken the final mprotect to R+W+X, which is the intent in the audit. That mutant is killed.
- The first L2 attempt was INVALID: an unescaped `[0]` in the sed pattern made the substitution a no-op. Fixed (`\[0\]`) and rerun: killed.
- The first full run was built against an unrelated tree: `/tmp/.../scratchpad/native-gcc` was configured by another worktree. Results from that tree were discarded. Scratch builds used by this handoff are `scratchpad/impl-gcc` (native gcc) and `scratchpad/impl-asan` (ASan+UBSan), both configured against this worktree.

## Test results

- Native gcc Release, ctest in `scratchpad/impl-gcc`: 100% passed, 19/19, 29.8 s.
- test_specialize_admission (gcc native build): 270 checks, 0 failures (was 259 before this patch).
- ASan+UBSan (`-fsanitize=address,undefined -fno-sanitize-recover=all`, Debug), ctest `-R "specialize_churn|specialize_admission|native_versions|specialize_inflight"`: 4/4 passed, 44.2 s. `specialize_churn` ran without any manual ASAN_OPTIONS.
- Default OFF dev build (`python3 tools/dev.py build --profile dev`): 52/52 steps. ctest in `build/`: 9/9 passed. `python3 -I tests/harness/run_m0.py --self-test`: rc 0, 176 checks, 0 failures.

## Next work

1. Part B (chunked scheduler wiring, effect check, equivalence tests, TSan) in its own worker.
2. TX5 and TX6 harness tests and the C07 environment and RLIMIT_AS assertions.
3. Run the full harness (`run_m0.py`, about 100 s) and native clang ctest.
