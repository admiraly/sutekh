# Handoff: native Windows host, in-process host API, fixed-step clock, command-stream replay

Branch `feat/windows-native-host`, based on `main` at `01aaf27`. Authorization and scope:
`planning/amendments/2026-10-18-downstream-host-work.md` (owner-authorized downstream work outside the M3 order;
no M3 gate is closed by it). Draft PR #29 exists only so the Linux CI job runs on this branch; the integrator
merges.

## Machine and toolchain (observed, 18 October 2026)

Windows 11 Pro build 26200, 12th Gen Intel Core i5-12400F (12 threads), 16 GB. No WSL and no Linux on this
machine: every POSIX result below comes from GitHub CI.

| Tool | Version | Identity |
|---|---|---|
| Clang / LLD | 23.1.3 (llvm-project `0d261d1ca552`) | clang.exe sha256 `f3425b2b...f263`, lld-link.exe `8d1dd3ba...0680` (same as `planning/toolchain-windows.json`); installer `5153f848...76de9` |
| MSVC headers and libraries | 14.44.35207 (VS 2022 Build Tools) | replaces 14.29.30133 of the earlier machine |
| Windows SDK | 10.0.26100.0 | replaces 10.0.19041.0 |
| CMake / Ninja | 4.3.4 / 1.13.0 (pip, `.venv`) | |
| Python | 3.13.15 (`.venv`) | |

Setup: `. .\tools\env-windows.ps1` (unchanged). Configure: `cmake -S . -B build-win -G Ninja
-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang`. `planning/toolchain-windows.json` was not edited (it
records the earlier machine); the table above is this machine's record.

## 1. Native Windows build and test suite

The control plane is ported through the platform layer; the POSIX code paths are unchanged in behaviour.

- `src/platform`: `su_thread.h` (pthreads, or SRWLOCK/CONDITION_VARIABLE/_beginthreadex), `su_posix.h` (POSIX
  file and process names over the UCRT; `O_BINARY`; 64-bit `stat`), realpath through
  `GetFinalPathNameByHandleW` with `/` separators, temp dir (TMPDIR first) and mkdtemp, self path, CPU count,
  sleep, process init (binary stdio, no crash or abort dialogs) and a kill-self test seam (exit code 137).
- `src/agent/su_proc.c` + `su_proc_win.inc`: `CreateProcessW` suspended, assigned to a job object, then resumed;
  explicit inherited-handle list; scrubbed, sorted environment (plus `SystemRoot`, `SystemDrive`, `windir`,
  `TEMP`, `TMP`). The supervisor's pipe ends are overlapped named pipes (one instance, remote clients refused),
  so `su_proto` enforces request deadlines with cancellable I/O. Exception exits map to POSIX signal numbers.
- Containment on Windows (reported in plain words in `containment`): job object kill-on-close, active-process
  limit 1 (no process creation; the worker verifies this from inside as its self-sandbox), per-process commit
  limit for `address_space_bytes`, user-time limit for `cpu_seconds`, no crash dialogs; disposable read-only
  workers run with a low-integrity token, verified on the child (`fs_write_denied: "low_integrity"`). **Not
  enforced on Windows:** network denial, file-size limit, open-handle limit, the `writable_dir` test seam.
- Path policy: a leading separator is absolute on Windows; an absolute path outside the project root is now
  `SU_E_CAPABILITY_DENIED` even when it does not exist, on both platforms (it was `SU_E_IO`), so replies do not
  reveal which files exist outside the root. Relative missing paths remain `SU_E_IO`.
- Tests: C drivers share `tests/common/su_test_os.h` (supervisor in a forked child on POSIX, on a thread on
  Windows). `path_fifo` and the Linux `containment` test are registered as skipped on Windows with the reason;
  `containment_win` checks the job, low-integrity and crash reporting from inside the child, each with a
  control. Python tests run through `SUTEKH_PYTHON` and write LF files.
- Harness: `tests/harness/run_m0.py` reads process facts from `tests/harness/os_facts_win.py` (ctypes) on
  Windows. `tests/platform/run_platform.py` implements the `win64-native` lane.

## 2-4. Host library: API, clock, replay

`su_host` (static) and `sutekh_host` (DLL; users define `SUTEKH_HOST_DLL`), built from `src/host/` plus the
schedule, entity-tick and host-declaration sources in their default configuration (no control plane, no native
tier, no seams). Headers: `include/sutekh/host.h`, `clock.h`, `replay.h`. 46 functions are exported (32 world, 3 clock, 11 replay):

- World: `su_host_world_create`, `su_host_world_destroy`, `su_host_api_version`, `su_host_status_name`,
  `su_host_schema_id`, `su_host_tick`, `su_host_capacity`, `su_host_tick_rate`, `su_host_set_tick_rate`,
  `su_host_seed`, `su_host_field_count`, `su_host_field_info`, `su_host_field_id`, `su_host_system_count`,
  `su_host_system_info`, `su_host_world_identity`.
- Entities: `su_host_spawn`, `su_host_despawn`, `su_host_entity_at`, `su_host_alive`.
- Inputs: `su_host_input_push`, `su_host_input_clear`, `su_host_input_encode`, `su_host_input_push_encoded`.
- Step and read: `su_host_step`, `su_host_column` (zero-copy), `su_host_read` (copy).
- State: `su_host_snapshot_size`, `su_host_snapshot_write`, `su_host_snapshot_restore`, `su_host_digest`,
  `su_host_hex`.
- Clock: `su_host_clock_init`, `su_host_clock_advance`, `su_host_clock_alpha`.
- Replay: `su_replay_writer_begin`, `su_replay_writer_step`, `su_replay_writer_finish`,
  `su_replay_writer_destroy`, `su_replay_free`, `su_replay_open`, `su_replay_close`, `su_replay_info`,
  `su_replay_system`, `su_replay_seek`, `su_replay_play`.

Formats (all canonical, little-endian, documented in the headers): input record `SUINPUT` v1 (writes sorted by
slot then field, unique), snapshot `SUHSNAP` v1 (world identity, tick, `SUWSTATE` v2, `SUEL` event log), replay
`SUREPLAY` v1. A desync is reported as `SU_E_INTERNAL` with `err.index == SU_REPLAY_DESYNC` because
`sutekh/status.h` is a frozen contract; no status code was added.

Determinism claim, exactly as tested: the same runtime build re-simulating the same inputs reproduces the same
digests, in one process and across processes (a recording made through the static library is verified through
the DLL in another process). Cross-compiler and cross-platform bit identity is not claimed or tested.

## Commands and results

| Command (from the worktree, after `env-windows.ps1`) | Result |
|---|---|
| `ctest --test-dir build-win -j 8` (Debug) | 62 passed, 0 failed, 2 skipped (`path_fifo`, `containment`) of 64 |
| `python -I tests/platform/run_platform.py --lanes win64-native,linux-native,macos,arm64 --evidence-dir planning/evidence/windows-host` | 7 passed, 0 failed, 6 unavailable; Release ctest 62 passed, 2 skipped of 64 (`platform-windows-native.json`) |
| `python -I tests/harness/run_m0.py --self-test` | 260 checks, 0 failures |
| `python -I tests/harness/run_m0.py --binary build-platform/win64-native/sutekh.exe --report-dir planning/evidence/windows-host/m0-harness-release` (Release, defaults) | 21 passed, 0 failed, 3 not_run, 3 unavailable |
| same, Debug binary, `--no-build --reload-iterations 60 --latency-samples 5` | 21 passed, 0 failed, 3 not_run (T00 skipped by flag, T12, TX6), 3 unavailable; T10 passed (17 checks) |
| Linux CI, PR #29, commit `768a7ba` (GCC 13.3.0, Debug) | 60 of 60 passed |
| Linux CI, PR #29, commit `9ec0653` merged onto `01aaf27` (GCC 13.3.0, Debug; host API, clock, replay and the shared library included) | 63 of 63 passed; `validate` jobs (ubuntu, windows) passed |
| `test_host` / `test_host_dll` | 139 checks, 0 failures each |
| `test_clock` | 42,036 checks, 0 failures |
| `test_replay`; `host_replay_record` then `host_replay_verify_dll` | 1,292 checks, 0 failures; cross-process verify passed |
| `test_containment_win` | 13 checks, 0 failures |
| `build-rel/bench_host.exe 2000` (Release) | observations below |

Harness not_run and unavailable, with reasons: T10 in the Release run lost the race in all 12 attempts (the
evaluation finished before the kill landed; it passed in the Debug run, so no isolation failure is implied);
T12 needs strace (Linux); TX6 reads `/proc` limits (Linux); TX3, TX4, TX10 need the native (AsmJit) build.

Timing observations (this machine, not qualification, constitution C10), `planning/evidence/windows-host/
bench-host-release.json`, 2-field demo capsule, event log 64: per call median (p95) in microseconds:

| Entities | step (1 tick, 4 input writes) | snapshot write | snapshot restore | digest | snapshot bytes |
|---|---|---|---|---|---|
| 64 | 1.0 (1.1) | 0.2 (0.3) | 2.8 (3.2) | 11.6 (11.9) | 3,861 |
| 1,024 | 10.9 (13.0) | 1.5 (2.0) | 10.2 (13.2) | 57.8 (66.7) | 20,181 |
| 16,384 | 170.6 (288.4) | 21.1 (38.9) | 162.0 (523.6) | 838.2 (1,455.1) | 281,301 |

The digest is the reference SHA-256 over the snapshot bytes and dominates; restore allocates a new world.

## Unrun, failing or open

- POSIX was not built on this machine; it is covered by the Linux CI job of PR #29 only (63/63 at `9ec0653`).
- The native AsmJit tier (`SUTEKH_NATIVE=ON`) is not built or tested on Windows; the native ABI probe is
  unavailable there. Sanitizer lanes were not run on Windows.
- `tests/broker` stays Linux-only (fcntl, resource, process groups), as the CI workflow already notes.
- `clang --analyze` (23.1.3) is clean on the new and ported sources except the deliberate null write of the
  crash probe in `test_containment_win.c`. It reports null dereferences in pre-existing `su_agent.c` (lines
  2761, 2767, 2935, 2941, 2962, 3309) and `worker.c` (210) that this work did not touch; they were not reviewed.
- GCC's `-Wuse-after-free` found two error-path bugs in the new host code on Linux CI (input push, replay
  writer begin); both are fixed. No Windows sanitizer lane was run.
- Windows network denial and file-size/handle limits are not enforced (see above); symlink-escape checks in
  `test_control` are reported NOT RUN on Windows without Developer Mode.
- A Windows CI job is proposed in `planning/evidence/windows-host/ci-windows-job.patch`; the session's push
  token cannot change workflows, so it is not applied.

## Contract notes for other work

- No change to `LIVE_IR_SPEC.md`, `include/sutekh/ir.h`, the verifier or the VM. `sutekh/status.h` unchanged.
- The host API exposes columns as 32-bit cells (`SuHostFieldInfo.type` says how to read them). If the numeric
  work adds wider or non-32-bit column types, `su_host_column`/`su_host_read`/`SuHostInput` need a typed
  variant; `su_world_canonical_write` and the snapshot format would carry the new types.
- `su_proc.h`: `SuProc` gains `process`/`job` handles on Windows; `SU_PROC_FS_LOW_INTEGRITY` added;
  `su_agent.c` reports `fs_write_denied: "low_integrity"`.
- `su_platform.h` grows (threads are in `su_thread.h`); `su_jobs` and `su_cmdq` link `su_platform`.
- CMake: `cmake/SutekhPlatform.cmake` (static CRT and UCRT-deprecation defines on Windows, `SUTEKH_PYTHON`,
  `sutekh_unsupported_test`); Python tests use `${SUTEKH_PYTHON}`; the host block is fenced with BEGIN/END
  markers at the end of `CMakeLists.txt`.

## Next

1. Integrate after the PR's Linux job passes on the final commit; apply the Windows CI job patch with a token
   that has workflow scope.
2. Build the AsmJit native tier on Windows (C++ with clang and the MSVC STL) and run the native ABI probe in the
   win64-native lane.
3. Host API: typed column access once the numeric types land; an in-place restore (no allocation) and a
   streaming digest if rollback budgets need them (see the observations).
4. Optional Windows network denial for workers (AppContainer or WFP) if the threat model needs it.
