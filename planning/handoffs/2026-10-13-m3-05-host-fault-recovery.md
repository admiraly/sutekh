# M3-05: recover from faults inside RTS host operators

Base: `50268e9`. Task remains in progress; this completes the host-fault part of
T-RTS-REPLAY-3, not all M3-05 or M3 closure gates.

## Behavior

`world.step` now treats a worker's `SU_E_INTERNAL` response as an in-process
fault. It discards the untrusted worker and recovers acknowledged worlds from
the supervisor's checkpoints and accepted-operation log in a replacement
worker. The failed request returns `SU_E_WORKER_FAILED` with recovery metadata;
the caller retries against the last acknowledged revision.

The resumed patch originally injected `SU_E_INTERNAL` before a scheduler
phase. Replaced that simulation with the existing internal `RtsOpsCtx` seams:
point 1 follows anchor stores in spatial and the first damage store in scatter.
Only test workers accept the three `SUTEKH_TEST_HOST_FAULT_*` environment
settings. Arming resets per host invocation, cannot carry into a later tick or
world, and is one-shot via a token file. Scheduler source and its fault callback
contract remain unchanged. No host semantics, descriptor, or shared ABI changed.

The recovery test creates the full six-system RTS world with two duelling
pairs. A fault during tick 8 occurs after a tick-4 checkpoint and a logged step
through tick 6. Assertions require a different live worker PID, checkpoint 4,
nonempty log replay, consumed token, and post-retry state/revision/event equality
with an uninterrupted control session through ticks 10 and 16. Production
binaries are checked for absence of all three test environment names.

Modified source: `src/agent/su_agent.c`, `src/runtime/worker.c`, and
`tests/recovery/test_recovery.c`. Durable state: `planning/tasks.json`,
`planning/status.json`, this handoff, and `planning/evidence/m3-host-fault/`.
Pack inventory, generated copies, validation report, and checksums regenerated.

## Validation

WSL Ubuntu, GCC 15.2.0. Debug VM build and unspecified-build-type native build
both compiled with the configured warning/error policy. Commands and exact
source/binary hashes are in `planning/evidence/m3-host-fault/result.json`.

- `ctest --test-dir build -j 4 --output-on-failure`: 42/42 passed. `cmdq` took
  194.77 seconds after its invalid-system-count test allocated approximately
  8 GiB before refusal, causing heavy paging. This admission defect is next.
- `ctest --test-dir build-native -j 4 --output-on-failure -E '^cmdq$'`:
  52/53 passed. The excluded queue code was already tested in the default lane;
  repeating its paging run was avoided. `native_avx2` failed only its unchanged
  upper-YMM XSAVE feasibility probe (`CHECK feasible`, dirty state not observed),
  previously documented in the agent-control handoff. The native suite is not
  reported as all passing.
- After final test-config parser hardening, targeted builds and
  `ctest --test-dir build{,-native} -R '^recovery$' -V`: each 3,748 checks,
  zero failures.
- Isolated-worktree mutations: bypassing internal-fault recovery and not arming
  the actual operator hook both fail recovery assertions. Source restored,
  rebuilt, and positive recovery rerun passes. Mutation checks preceded the
  final parser overflow hardening; the tested recovery/hook branches were not
  changed by that hardening. Logs retained; mutant changes are not in main.

Native Windows, sanitizer execution, and independent review were not run for
this slice. Existing limitations and the N <= 65,536 recovery claim remain.

## Next

Reject oversized queue dimensions before allocating cap arrays or scanning
per-system inputs. Then continue M3-05's world-scale capacity, churn, and
engine/model comparison gates. No new milestone closure is claimed.
