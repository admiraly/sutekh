# Issue 148: sanitizer build green, use-after-free in su_replay_open, ASan+UBSan CI job

## State and scope

Locally verified on `fix/issue-148-sanitizers`, rebased on `70ce82d` (origin/main, merge of PR 150). Not merged;
GitHub Actions results are in the PR, not here.

A Clang Debug build with `-fsanitize=address,undefined -fno-sanitize-recover=undefined` failed 21 of 136 tests
on unmodified `70ce82d` (21 of 126 on `374af9d`, the first base; the set was the same). The native tier
(`SUTEKH_NATIVE=ON`), which the issue did not cover, failed 16 more. Now:

- **Defect fixed (the reported one):** `su_replay_open` filled the error for "initial keyframe does not match the
  header digest" by reading `in->start_tick` after `su_replay_close(r)` freed the replay `in` points into. The
  error is now filled before the close. The other refusals in `su_replay_open` pass constants only. The writer's
  initial-snapshot refusal already copied the start tick before `free(rw)`.
- **Defect fixed (found by the audit; not a sanitizer finding):** the check that every digest record is present
  counted digest ticks one tick at a time from `start_tick` to `end_tick`, and both come from the input. An
  828-byte recording made `su_replay_open` spin forever: it keeps the real header and initial keyframe, sets
  `keyframe_interval = UINT64_MAX` and `digest_interval = 1`, and adds a second keyframe and the end record at tick
  `UINT64_MAX`. That passes every earlier check, and `u <= UINT64_MAX` never ends. The count is now arithmetic
  (`span/di - span/lcm(di, kf)`).
- **Test-code defects fixed:** three tests leaked (host_rollback x2, the two RTS world handler tests). The native
  W^X probe child let ASan print a report for its deliberate fault.
- **Instrumentation incompatibilities:** handled by a documented test environment in ASan builds
  (`cmake/SutekhSanitizers.cmake`), plus `-fno-sanitize=function` in native-tier sanitizer builds. No test is
  skipped for sanitizers. No assertion was weakened, except that containment's RLIMIT_AS check expects unlimited
  when `SUTEKH_WORKER_AS_BYTES=0`. That branch was already in the test.
- **New CMake option `SUTEKH_SANITIZE`** (a `-fsanitize=` list, empty by default; other builds unchanged).
- **New CI job** `sanitizers` (Linux, clang, Debug, ASan+UBSan, matrix native OFF/ON). Every instrumented process
  logs to `sanitizer-logs/`, and `tools/scan_sanitizer_logs.py` fails the job on any report.

Exact modified paths:

- `src/host/su_replay.c`
- `tests/host/test_replay.c` (regressions `test_initial_keyframe_mismatch`, `test_huge_span`)
- `tests/host/test_host_rollback.c`
- `tests/rts/control/test_world_set_fields.c`
- `tests/rts/control/test_world_spawn_batch.c`
- `tests/native/native_test_mem.h`
- `CMakeLists.txt` (include of the sanitizer module, deferred test-environment call, `host_replay` TIMEOUT 300)
- `cmake/SutekhSanitizers.cmake` (new)
- `tools/scan_sanitizer_logs.py` (new)
- `tests/tools/test_scan_sanitizer_logs.py` (new)
- `.github/workflows/repository-checks.yml` (new `sanitizers` job; the validate job also runs the scanner tests)
- `planning/handoffs/2026-10-26-sanitizers.md` (this file)

## Sanitizer environment (ASan builds only)

Applied to every registered test by `sutekh_sanitizer_test_environment`. That runs as a deferred call, after the
tests that `cmake/SutekhNumeric3D.cmake` registers in its own deferred call. It is used for `SUTEKH_SANITIZE`
containing `address`, or for `-fsanitize=...address` in `CMAKE_C_FLAGS`.

| Setting | Why | What it costs |
|---|---|---|
| `SUTEKH_WORKER_AS_BYTES=0` (every test) | ASan reserves about 15 TiB of virtual shadow at start-up (`ReserveShadowMemoryRange failed ... Perhaps you're using ulimit -v`). Under the default 4 GiB RLIMIT_AS, every worker and disposable child dies before main. | The 4 GiB worker limit is not exercised under ASan. Every non-sanitizer job still covers it. containment then expects an unlimited RLIMIT_AS; its other limits (CPU, fsize, nofile) are still checked. |
| `SUTEKH_WORKER_SANDBOX=0` (every test except containment; honoured only in sanitizer builds, `su_sandbox.c`) | At exit, LeakSanitizer forks (`TestPTrace`) and ptrace-attaches. The worker self-sandbox denies fork, so the leak check aborts ("LeakSanitizer has encountered a fatal error") and the worker exits nonzero. | The self-sandbox is exercised under ASan only by containment, which keeps it on and checks it reports `on`. In exchange, LeakSanitizer now runs in ordinary workers. |
| `-fno-sanitize=function` (Clang, `SUTEKH_NATIVE=ON`, every target) | Clang's function sanitizer reads a type signature stored before the callee. AsmJit kernels have none, so the read before a kernel at the start of a page faults (SEGV at `su_native.c:989`). This is the same tooling issue as D3 in PR 145. | No function-type checking of indirect calls in native-tier sanitizer builds. The default build keeps it. |

Remaining coverage gap: disposable children run under the containment seccomp filter, which denies `ptrace`
(101), so LeakSanitizer cannot run in them ("Could not attach to thread ... (errno 1)"). The scanner accepts such
a log only when it holds nothing but those lines, and reports the count. That was 103 logs in the default lane and
138 in the native lane. Those processes are not leak-checked. They are not counted as passes.

## Per-test triage (base `70ce82d`, clang 22.1.8, ASan+UBSan, no adjustments)

| Test | Report or failure on main | Class | Resolution | Regression evidence |
|---|---|---|---|---|
| host_replay | heap-use-after-free READ 8 at `su_replay.c:696` in `su_replay_open` (freed by `su_replay_close`) | **Engine defect** | Error filled before close (c91cc4f) | `test_initial_keyframe_mismatch` runs first; without the fix ASan reports the UAF from it (`test_replay.c:363`) |
| host_rollback, host_rollback_dll | LeakSanitizer: 2 x 2020 B from `snap()` in `test_inplace_equivalence` | Test defect | Snapshots freed | Clean under LSan |
| rts_world_set_fields, rts_world_spawn_batch | LeakSanitizer: about 2.6 MB (world, schedule, entity runtime and event log from `h_world_create`) | Test defect (the test frees only its `WorkerState`) | Call worker.c's `free_worlds` first, as `su_worker_main` does | Clean under LSan |
| control | `su_test_proc_clean_exit` CHECK x2 (`test_control.c:968, 1066`): the worker exits nonzero because LSan cannot fork under the self-sandbox | Instrumentation | `SUTEKH_WORKER_SANDBOX=0` | The worker's leak check now runs and is clean; both checks pass |
| containment | Children fail to start: ASan shadow reservation under RLIMIT_AS 4 GiB | Instrumentation | `SUTEKH_WORKER_AS_BYTES=0` (test already supported it) | 59 checks pass; sandbox reported `on` |
| rts_load_engine, rts_caps_engine, rts_replay_engine, rts_model_digest, rts_ctrl_engine, rts_e2e_edges, rts_projectile_mutant, rts_b1/b2/b3_hidden, rts_b1/b2/b3_install, space3d_serve (14 tests) | `world.create` -> `SU_E_WORKER_FAILED`: every worker died at ASan start-up under RLIMIT_AS | Instrumentation | `SUTEKH_WORKER_AS_BYTES=0` | All pass. No worker reports in the logs |
| native_scalar, native_avx2, native_versions, native_fault, native_chunked, safe_retire, specialize_admission, specialize_inflight, specialize_churn, ir03_native_diff, ir04_native_diff, host_ops, host_ops_dll, example_arena_host_native, example_arena_host_respond_native, host_rollback_bench_smoke (native lane, 16) | SEGV in `su_native_run_checked` (`su_native.c:989`) reading 8 bytes before a JIT kernel; one in `native_test_worker` | Instrumentation (`-fsanitize=function` vs JIT code) | `-fno-sanitize=function` with the native tier | All pass |
| native_scalar, native_avx2 (after the above; tests passed, scanner only) | ASan "SEGV on unknown address" from the forked child of `su_nt_store_faults`, which stores into RX code on purpose | Test harness | Child resets SIGSEGV/SIGBUS to SIG_DFL before the store. Result unchanged (killed by signal = faults) | No report; W^X checks still pass |
| (none: audit of `su_replay.c`) | `su_replay_open` does not return on a crafted 828-byte recording (60 s timeout, 100% CPU) | **Engine defect** (hostile input) | Arithmetic digest-tick count (4b2256a) | `test_huge_span`: before the fix `test_replay` did not finish in 60 s; after, SU_E_PARSE "missing digest records". The new count equals the old loop on 345,600 (start, end, kf, di) cases |

Skipped under sanitizers: none. Pre-existing skips stay as they were (`audio_device`: no audio device on this
machine; `native_unwind`: Windows-only, registered by `sutekh_unsupported_test`).

## Commands and results

Machine: i7-14700K, Nobara 44 (Fedora 44), kernel 7.2.9. Compilers: clang 22.1.8 and GCC 16.2.1. CMake 4.3.0,
Ninja. Builds were under a scratch directory outside the tree, `-j 6`.

Reproduction on `70ce82d` (tree from `git archive origin/main`):

    cmake -S <main> -B <b> -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=OFF \
      -DSUTEKH_RENDER=OFF -DSUTEKH_WINDOW=OFF -DSUTEKH_ASSET_IMPORT=OFF \
      "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined
    ctest -j 6 --output-on-failure        # 21 failed of 136 (table above), 1 skipped

Branch, CI-equivalent (env set before configure, as in the job):

    ASAN_OPTIONS=log_path=<logs>/san:detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:log_path=<logs>/san
    cmake -S . -B <b> -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
      -DSUTEKH_NATIVE=OFF|ON -DSUTEKH_RENDER=OFF -DSUTEKH_WINDOW=OFF -DSUTEKH_ASSET_IMPORT=OFF -DSUTEKH_SANITIZE=address,undefined
    ctest -j 6 --timeout 900 --output-on-failure && python -I tools/scan_sanitizer_logs.py <logs>

| Lane | Result | Scanner |
|---|---|---|
| ASan+UBSan, native OFF | 136 tests, 0 failures, 1 skipped (audio_device), 217 s wall | 103 logs, 0 reports, 103 leak checks not run |
| ASan+UBSan, native ON | 153 tests, 0 failures, 2 skipped (native_unwind, audio_device), 241 s wall | 138 logs, 0 reports, 138 leak checks not run |
| GCC Debug, renderer, window and importer ON, `-Werror` | 157 tests, 0 failures, 1 skipped (audio_device) | n/a |
| GCC Release, same options | 157 tests, 0 failures, 1 skipped (audio_device) | n/a |

The test counts equal origin/main's for the same configurations (157 / 136 / 153). No test was added or removed.
The new checks live inside `host_replay`.

The scanner was checked against real reports: with the su_replay.c and test_host_rollback.c changes stashed, it
found 2 reports (the UAF and the 4040-byte leak) and exited 1. `python -I tests/tools/test_scan_sanitizer_logs.py`
ran 8 tests, OK. `tools/check_repository_license.py` passed. `tools/validate_pack.py --check-report` passed.
`git diff --check` was clean.

GitHub Actions on PR 154 (run 40288625904, Ubuntu clang 21.1.8): every job passed. `sanitizers` native OFF:
139 tests, 0 failures, scanner 103 logs / 0 reports, 15m54s. Native ON: 156 tests, 0 failures, scanner 138 logs /
0 reports, 17m57s. The runner registers more tests than the local lanes (139/156 against 136/153).

## Unrun

- ASan+UBSan with renderer, window or importer on (lavapipe and SDL are not instrumented; not attempted).
- GCC sanitizers. TSan. MSan. Windows/MSVC sanitizers (`SUTEKH_SANITIZE` refuses MSVC).
- Leak checking inside seccomp-contained disposable children (ptrace denied by design; see the gap above).
- Windows and MSVC builds of the test changes. They are portable C or inside `#else` of `_WIN32`, but were not
  compiled on Windows.

## Next work

- If the leak-coverage gap in contained children matters, a sanitizer-only opt-out of the ptrace denial in the
  containment filter would close it. It would weaken that filter in sanitizer builds, so it is left for a decision.
- The 4 GiB worker RLIMIT_AS cannot be tested under ASan. An LSan-only lane (`-fsanitize=leak`, no shadow memory)
  could keep it, if wanted.
