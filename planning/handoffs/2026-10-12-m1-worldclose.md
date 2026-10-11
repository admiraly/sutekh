# M1 handoff: world.close (free a world, release its slot)

Status: implemented and tested locally (GCC 13, ASan+UBSan, dev and release builds). Committed on the worktree branch, not pushed. `planning/tasks.json`, `CMakeLists.txt`, `include/`, `docs/AGENT_RPC.md` and `bench/` not touched.

## Behaviour

`world.close {world_id, expected_world_revision}` -> `{world_id, closed: true, released: {snapshots, systems}}`

- Check order: unknown `world_id` -> `SU_E_INVALID_ARGUMENT` (the code `world.inspect` returns for unknown ids); then stale revision -> `SU_E_STALE_REVISION`. Both run before any change, so the world is untouched on error.
- On success the worker frees the world instance, its schedule (entries, VM contexts, modules, field table), the checkpoints the world owns in the checkpoint store, and the registry records bound to it. The slot becomes reusable.
- Ids come from the worker's monotonic counter (`s->next_id`), so a closed id is never issued again. A later request naming a closed id fails as unknown for inspect, step, snapshot, restore, install and close.
- A snapshot of a closed world cannot be restored into another world (its checkpoint records are gone; restore is per world id).
- The supervisor (`su_agent`) forwards the method like the other world methods. It holds no world state of its own, so nothing else changes there. `engine.info` advertises `world.close`.

## Modified paths (sha256, first 16 hex)

| Path | sha256 |
|---|---|
| `src/runtime/worker.c` | 0281edcff50a1c13 |
| `src/runtime/worker.h` | 5a88f517dbad97eb (protocol comment only) |
| `src/runtime/registry.c` | a3448cbb0d017c6e |
| `src/runtime/registry.h` | 9438f6502950d780 |
| `src/agent/su_agent.c` | c2798e6fbb0c9e99 |
| `tests/control/test_control.c` | f9f2fed2c91b258a |
| `tests/harness/run_m0.py` | 630c98b557a013ba |

Added code: `su_reg_release_world` (registry), `ckpt_release_world`, `h_world_close`, `h_internal_stats` (worker), `h_world_close` (agent). `tests/reload`, `tests/snapshot_cp` unchanged.

## Deviation for the contract owner

- **`internal.stats` (worker only).** The worker answers `internal.stats {}` with world, checkpoint and registry counts read from the structures. The supervisor does not forward it and it is not advertised. The test uses it because a skipped release is otherwise invisible to every public reply. Contract owner: confirm this is acceptable as an internal worker method, or ask for it to be removed in favour of another check.
- Unknown-world on close returns `SU_E_INVALID_ARGUMENT`, as the other world methods do. The contract text does not name a separate code.

## Harness

- `ENGINE_METHODS["world.close"]` added.
- **Workaround removed.** T02 and T03 each opened one serve process per world size or case, because a worker held at most 4 worlds. They now use one session each and close every world after its checks (`close_world`). Both now require `world.close`.
- New extra test `TX1` ("World close beyond the 4-world cap (extra)", not a TEST_SPEC id): one serve session creates 4 worlds, refuses a 5th, refuses a stale close (world still correct afterwards), closes one, checks that the closed id fails for inspect, step and close, reuses the slot, and runs 8 more create/close rounds. It checks that no world id repeats.
- `--self-test`: 160 checks, 0 failures. New checks cover `close_mismatches` (good and bad replies), the method table entry, and the requirements of TX1, T02 and T03.

## Commands and results

- `python3 tools/dev.py build --profile dev`: OK. `--profile release`: OK. Dev rebuilt last.
- `ctest --test-dir build` (dev and release): 7/7 (world, ir, vm, control, reload, snapshot, snapshot_cp).
- `python3 -I tests/harness/run_m0.py --self-test`: 160 checks, 0 failures.
- Full harness against `build/sutekh` (dev), report in the session scratchpad outside the repo: passed 17, failed 0, not_run 0, unavailable 0 (T00-T15 and TX1). T11 1,000 reloads passed. T02 and T03 ran on one session each.
- `test_control` (gcc -O0, no sanitizers): 7239 checks, 0 failures.
- `test_control` under ASan+UBSan (`-fno-sanitize-recover=all`): 7239 checks, 0 failures, rc 0, no sanitizer output. The worker child's exit status is checked; LeakSanitizer exits nonzero, so a worker leak fails the test.
- `test_snapshot_cp` under ASan+UBSan: 127 checks, 0 failures. `test_reload` under ASan+UBSan: 2773 checks, 0 failures.
- Leak and RSS evidence in `test_control` (`test_world_close_worker`): 1,000 create, step, snapshot, close cycles on one bare worker. Counters returned to baseline after the loop (3 worlds, 0 checkpoints, 0 checkpoint records, 3 registry records, world sequence 1005, so no id reused).
  - Non-sanitizer build: worker RSS 2484 KiB after 100 cycles and 2484 KiB after 1000 cycles.
  - ASan build (quarantine capped to 1 MB by the test): 17744 KiB after 100 and 19904 KiB after 1000 cycles, +2160 KiB. This is not flat. The test's bound is 4096 KiB, which is a test-chosen number, not a spec number.

### Mutations (scratch copies outside the repo, gcc -O0, `test_control` run against each)

| Mutation | Result |
|---|---|
| baseline (unchanged copy) | rc 0, 7239 checks, 0 failures |
| skip checkpoint free (store release disabled in `ckpt_release_world`) | rc 1, 1006 failures (first: `released.snapshots` and `checkpoints == 1`) |
| allow stale close (revision check removed in `h_world_close`) | rc 1, 13 failures (first: `SU_E_STALE_REVISION` expected, got ok) |
| reuse world ids (id from slot index) | rc 1, 3 failures (`world-5` expected, got `world-2`; then unknown world) |
| skip registry release (`su_reg_release_world` disabled) | rc 1; `registry_in_use` mismatches, then the loop stops because the registry fills and `world.create` is refused |

## Unrun checks and gaps

- Clang build and clang sanitizers: not run (runtimes are not installed here, as in earlier handoffs).
- Windows: not applicable (POSIX only, as before).
- The 4 GiB `RLIMIT_AS` path in non-sanitizer runs was exercised only through the bare-worker test and the harness' `build/sutekh` runs; the ASan build uses no limit.
- Candidate records keep the txid of the last install into a closed world (`candidate.status.install`). They are not cleared on close.
- `candidate.install` into a closed world is refused as unknown (the TX1 and test_control paths check this), but the supervisor's own candidate evaluation state is not affected.

## Not changed (owners)

- `docs/AGENT_RPC.md` (method table): add `world.close`. Owner: docs.
- `planning/contracts/M0_CONTRACT.md` / `planning/tasks.json`: task state for world.close.
- `bench/reload/bench_reload.py`: `--share-serve` (lines ~392, 688, 939) still reuses one serve process for all warm epochs and is documented as failing at the world cap. Once bench can close worlds between epochs it can drop that flag. Left alone, as instructed.
- `CMakeLists.txt`: no new test target needed, as the C tests were added to existing executables.

## Next work

1. Owner review of `internal.stats` (deviation above).
2. Update `docs/AGENT_RPC.md` and the M0 contract table.
3. Bench owner: remove the `--share-serve` world-cap workaround.
4. Clang and sanitizer checks on a host with the runtimes.
