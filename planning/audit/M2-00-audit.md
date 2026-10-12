# M2-00 independent audit (entities, chunked storage, queues, IR 0.2, snapshot 0.2, control plane, recovery)

Auditor: independent auditor, worktree `agent-acd0fcd2d51d1062d`, HEAD `e730672` at start (M2-01 contract draft r1 on top of the M2-00c3b handoff). Scope per the audit brief. No product, harness, contract or evidence file was edited. Scratch root (outside the repo): `/tmp/claude-0/-home-user-sutekh/82533bc6-e6ca-564a-ba43-39dd3091bafa/scratchpad/audit/` (199 MB).

## 1. Verdict

**M2-00 cannot be accepted.** Blocking items, exactly:

1. **Contract section 6 is not fully met.** Four items are missing or partial (table in section 2): merge equality at N=8 is not tested; dead-slot zeroing after a *native kernel* is only simulated by a direct write; "regenerated v2 canonical dumps for the M0/M1 fixtures" has no changed fixture in the tree (`git diff --numstat 20f2625..HEAD -- tests/fixtures` shows additions only); recovery of despawn history is not proven (MX18).
2. **Admission (contract B8) is not tested through the control plane.** Removing the `world.create` admission refusal (MX19, `src/runtime/worker.c:830-836`) survives every test. The 1,000,000-row refusal exists only as a manual measurement.
3. **MILESTONES.md M2 has no closure record.** The M2 section restates the closure criteria; it records no M2-00 evidence.
4. **Recovery with despawn history is not verified.** Mutant MX18 (despawn not written to the recovery log, `src/agent/su_agent.c:4216`) passes `test_recovery` and harness TX7 (60 checks, 0 failed).

Not blocking, but a decision the owner must record: with default per-tick caps (4096), a world created with 1,000,000 rows is accepted but its first despawn-heavy tick fails with `SU_E_RESOURCE_LIMIT` (section 5). This is the limit the worker reported; it is confirmed.

## 2. Acceptance-test coverage (contract r2 section 6)

| Required test | Status | Evidence (exact test / file) |
|---|---|---|
| Entity churn >= 100,000 ops vs model, lowest-slot reuse | covered | `tests/entities/test_churn.c` (100,000 ops, 150,047 checks, 0 failures, rerun); `test_entities.c` `test_churn_against_model`, `test_lowest_slot_reuse` |
| Stale handles: reuse, retired, out of range, zero, gen 0 with nonzero slot, wrong gen | covered (unit + process) | `test_entities.c` `test_stale_handles` (gen 0 live slot, gen 0 dead slot, slot 1024 and 0xFFFFFFFF out of range, wrong gen); harness TX7 (gen 0 slot 1, slot 99, wrong gen, zero) |
| Generation exhaustion retirement via fault near the limit | covered at world level; not via control plane | `test_entities.c` `test_generation_exhaustion`; `test_world_ents.c` `test_retirement`; `test_churn.c` fault seam. No process-level seam (handoff deviation 4) |
| Canonical merge equality, N = 1, 2, 4, 8 | partial | N=1,2,4: `test_entity_tick.c` `test_workers_equal`; harness TX8 (N=1,2,4). **N=8 missing** |
| Shuffled completion orders | partial | `test_cmdq.c` `test_canonical_interleavings` (seeded task permutation, buffer level only); not at full-tick level |
| Per-row / per-tick cap overflow aborts, state unchanged (table, event log, columns) | covered by name; state checks not inspected line by line | `test_entity_tick.c` `test_cap_abort`; `test_cmdq.c` `test_overflow`; harness TX7 cap overflow (checks the state after refusal through the binary) |
| Dead slots excluded and canonically zero after native kernels staged nonzero values (native build) | **partial** | `test_world_ents.c` `test_zero_dead` uses `stage_nonzero_everywhere` (direct writes, both builds). **No test runs a native kernel against a world with dead slots** |
| Snapshot 0.2 round trip incl. event log; 0.1 restore | covered | `tests/snapshot/test_snapshot.c` (round trip with events, parse, restore, `worked_0_2.sha256`); `test_world_ents.c` `test_canonical_v1` |
| Regenerated v2 canonical dumps for M0/M1 fixtures | **not evidenced** | `test_world_ents.c` `test_canonical_v2` (expected values not inspected); no fixture or evidence file changed since 20f2625 (additions only) |
| IR 0.1 semantic hashes unchanged | covered (verified by me) | `tests/ir/test_ir.c` `test_ir02_hashes` pins `b9e6bd62...`; same value in `planning/evidence/m0-harness-release.json` and `m0-demo-release.json` (checked) |
| New-op verifier negatives with stage ordering | covered by name | `tests/ir/test_ir.c` section "IR 0.2 (M2-00c2): negatives by stage" (not re-read in full) |
| Reference-VM known answers per new op | covered by name | `tests/vm/test_vm.c` (slot_u32, tick_u32, emit_event, despawn_self programs) |
| Recovery replay of spawn/despawn | **partial** | Spawn: TX7 crash scenario. Despawn: **not detected** when omitted (MX18). `test_recovery.c` has no despawn case |
| Mutation proofs for each test | covered by my own run | section 4 (21 mutants: 15 killed, 3 equivalent survivors, 3 real gaps) |

Closure evidence in MILESTONES.md M2 for M2-00: none recorded (criteria text only).

## 3. Lanes (commands, return codes)

Build tree root: `scratchpad/audit/` (five CMake trees, `rel`, `dev`, `nat`, `asan`, `mutb`, rather than one; disclosed).

| Lane | Command (abbreviated) | rc / result |
|---|---|---|
| AsmJit bootstrap | `python3 tools/bootstrap_asmjit.py` | 0 |
| Default Release build | `cmake -S . -B .../rel -DCMAKE_BUILD_TYPE=Release`, `cmake --build` | 0 |
| Default Debug (dev) build | same, `Debug` | 0 |
| Native Release build | `-DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release` | 0 |
| ASan+UBSan build (`sutekh` target) | `-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"` | 0 |
| ctest release | `ctest --test-dir .../rel -j4` | 0 (17/17) |
| ctest dev | `ctest --test-dir .../dev -j4` | 0 (17/17) |
| ctest native | `ctest --test-dir .../nat -j4` | 0 (29/29) |
| `run_m0.py --self-test` | | 0 (241 checks, 0 failures) |
| Full harness, release binary | `run_m0.py --binary .../rel/sutekh --report-dir .../h_rel` (T00 builds) | 0: passed 22, failed 0, not_run 0, unavailable 2 (TX3, TX4: no native build) |
| Full harness, native binary | `run_m0.py --binary .../nat/sutekh --report-dir .../h_nat` | 0: passed 24, failed 0, not_run 0, unavailable 0 (TX7 62 checks, TX8 38) |
| Zero-wait demo (release) | `tools/demo_zero_wait.py --binary .../rel/sutekh --out ...` | 0 |
| strace probe (demo) | `strace -f -c` and `strace -f -e trace=socket` | 0. 45 execve calls (38 failed, PATH lookups). Two `socket(AF_INET)` calls, both `EPERM` (worker seccomp, M1 C07) |
| 1M-row IR 0.1 movement | `test_tick_workers --bench`, 5 runs (section 6) | 0 |
| Mutation baseline (Debug copy) | `mutate.py baseline` | all 10 targets pass |

## 4. Mutation testing (independent; scratch copy of `src include tests examples schemas third_party`, one edit at a time, restored after each)

| ID | Mutant | Result | Catching test(s) |
|---|---|---|---|
| MX01 | alloc picks highest free slot in its 64-slot word (not global) | KILLED | test_entities, test_churn |
| MX02 | despawn does not bump generation | KILLED | test_entities, test_churn |
| MX03 | retirement threshold off by one | KILLED | test_entities, test_world_ents, test_churn |
| MX04 | validate accepts generation mismatch | KILLED | test_entities, test_world_ents, test_churn |
| MX05 | generation 0 accepted (`SU_ENTITY_GEN(h)==0` check removed) | SURVIVED, **equivalent** | Stored generations are always >= 1 (`SU_GEN_FIRST`), so the mismatch check already rejects gen 0 |
| MX06 | zero_dead skipped for chunk 0 | KILLED | test_world_ents, test_entity_tick |
| MX07 | zero_dead chunk base offset +1 row | KILLED | test_world_ents, test_entity_tick |
| MX08 | cmdq merge tie-break by seq reversed | KILLED | test_cmdq, test_entity_tick |
| MX09 | event ring evicts newest instead of oldest | KILLED | test_cmdq, test_entity_tick (test_world_ents passes) |
| MX10 | events.read skipped counter off by one | KILLED | test_cmdq |
| MX11 | events.read accepts cursor ahead of next_seq | KILLED | test_cmdq |
| MX12 | per-row despawn cap check off by one (`>=`) | KILLED | test_ir (first run had a bad anchor; rerun after fix) |
| MX13 | IR 0.2 semantic hash omits ir_version string | KILLED | test_ir |
| MX14 | emit_event reserved-kind bound off by one | KILLED | test_ir |
| MX15 | tick_u32 returns committed tick, not tick being computed | KILLED | test_vm (test_entity_tick and test_ir pass) |
| MX16 | snapshot 0.2 key list order swapped (parser array only) | SURVIVED, **equivalent as written** | The parser's `check_keys` is order-insensitive and re-encode compares bytes; a writer-order change would be caught by `worked_0_2.sha256` (writer not mutated) |
| MX17 | restore ignores explicit `retired_count` check | SURVIVED, **equivalent** | `build_table` still rejects 1->0 and 1->2 ("entity table is not canonical"), measured with `snapfuzz` |
| MX18 | recovery log omits despawn (`su_agent.c:4216` LOG_NONE) | **SURVIVED (gap)** | Not caught by test_recovery or TX7 (60 checks pass). Reason not established |
| MX19 | admission budget refusal removed (`worker.c:830`) | **SURVIVED (gap)** | Not caught by test_entity_tick (budget unit test only) or TX7 |
| MX20 | world spawn allowed during tick (`su_world.c:675`, second phase guard) | KILLED | test_world_ents (test_entities passes) |
| MX21 | world.close does not release the world (`worker.c:2588` world_drop removed) | **SURVIVED (gap)** | Not caught by TX7; memory leak not observed (leak check off) |

Count: 21 mutants, 15 killed (MX01-04, MX06-15, MX20), 3 equivalent survivors (MX05, MX16 as written, MX17), 3 real survivors (MX18, MX19, MX21). Results: `scratchpad/audit/mutation_results_all.json` and `mutation_results_mutants.json`.

## 5. Defects and findings, ranked

1. **Recovery despawn replay not verified (MX18).** `src/agent/su_agent.c:4216` appends the despawn; with the append removed the harness still matches the model. The claim "recovery with spawn/despawn history" is unproven for despawn. Severity: high for acceptance, since it is an explicit contract item.
2. **Admission not tested through the control plane (MX19).** `src/runtime/worker.c:830-836` has no worker-level test; the 1M-row case was measured by hand (handoff section 6). Contract B8 says admission fails with `SU_E_RESOURCE_LIMIT`. Severity: medium.
3. **1M-row world is creatable but cannot tick under default caps (confirmed).** `world.create` with 1,000,000 rows and default caps returns ok; `world.step` returns `SU_E_RESOURCE_LIMIT` ("system 0 failed; tick discarded, committed state unchanged"); `world.inspect` afterwards reports alive 1,000,000. Severity: medium, a product decision (contract B8 buffer sizing), not a correctness bug.
4. **world.close release untested (MX21).** `src/runtime/worker.c:2588` (`world_drop`). Severity: low.
5. **Native dead-slot requirement untested with a real native kernel.** `tests/entities/test_world_ents.c:360-384` stages values by direct writes. Severity: low for M2-00, required by section 6.
6. **N=8 merge equality untested.** `test_entity_tick.c` and TX8 use N=1,2,4 only. Severity: low.
7. **Snapshot 0.2 accepts a self-consistent altered counter (informational).** `event_dropped` 7->0 in `worked_0_2.json` parses and restores (`snapfuzz`). Snapshots carry no integrity tag, so this is by design; note only.
8. **Snapshot parser robustness: no memory-safety defect found.** Driver `snapfuzz` (ASan+UBSan, links the ASan libraries; counts cumulative over one run): 918 truncation prefixes, all rejected; 16 targeted count/key/overflow cases, 15 rejected with diagnostics (the accepted one is item 7); a duplicate key rejected; 20,000 seeded byte-level mutations; whole run: 643 parses ok, 20,293 rejected (statuses 1, 2, 3, 9, 11); no sanitizer report. The `slots->type` access at `snapshot.c` (about line 1184) is reachable only if a required key is absent; `check_keys` rejects that earlier, and I did not test it separately.
9. **Event ring, cap check, merge comparator: read, no defect.** `su_cmdq.c:768-775` (eviction keeps the oldest-first ring; MX09 killed), `su_cmdq.c:851` (cursor check), `ir/su_lir.c:991-1000` (per-row cap, `>` correct; MX12 killed).
10. **Shuffled completion orders are tested at buffer level only** (`test_cmdq.c`), not through a full tick.

Not reviewed line by line (scope limit): `src/runtime/worker.c` entity handlers beyond the sites above, `src/runtime/entity_tick.c`, `src/world/su_world.c` entity parts, full IR verifier 0.2 additions. Findings above come from mutation, fuzzing and targeted reading, not a complete code review.

## 6. Performance (re-measured; release, non-native, this shared host)

Movement, IR 0.1, 1,000,000 rows, `test_tick_workers --bench` (5 ticks per line; 5 runs; this audit's medians):

| N | this audit: median [min, max] ticks/s | handoff c3b "after" median [min, max] | M0-13 stress (RPC, release, 1M rows) |
|---|---|---|---|
| 1 | 64.7 [57.8, 77.5] | 58.8 [47.4, 69.6] | release world.step median 12.49 ms, p95 22.41 ms (about 80 ticks/s at median) |
| 2 | 105.7 [80.4, 116.2] | 91.5 [83.2, 98.3] | n/a |
| 4 | 119.6 [112.0, 140.7] | 127.0 [96.5, 138.8] | n/a |

The handoff's "within the spread" statement holds. The M0-13 RPC stress figure is consistent with the in-process rate.

IR 0.2 entity system, one system (`despawn_move`: `despawn_self` + `emit_event`), 1,000,000 rows, caps raised to 1,048,576, client wall time per `world.step` ticks=1 (`scratchpad/audit/bench_ir02.py`, not committed):

| Config | ticks/s (median of 6 steps) | Note |
|---|---|---|
| N=1, run 1 | 4.25 | 727,273 rows alive after the first tick |
| N=1, run 2 | 3.94 | same workload |
| N=4 | 4.39 | same workload |

Handoff claim: N=1 3.57 to 3.98, N=4 3.35 to 3.55. My N=1 figure is slightly higher and N=4 slightly above the range, so the claim is host-dependent but not contradicted. The handoff's method (its benchmark script) is not committed; this audit's client is not in the repo, so the IR 0.2 rate is not reproducible from the repository.

Not re-measured: worker peak RSS (handoff 294 MB) and two-system admission refusal at 1M rows.

## 7. Fuzzing (ASan+UBSan `sutekh` binary; `ASAN_OPTIONS=quarantine_size_mb=1:detect_leaks=0`, `SUTEKH_WORKER_AS_BYTES=0`, `SUTEKH_WORKER_SANDBOX=0`)

3 seeds (21, 22, 23), 8 sessions each (24 total), `SUTEKH_TICK_WORKERS` 1 or 2 chosen per session at random. Mutation: one field per request (about 35%), plus malformed wire bytes (truncation, duplicate keys, bad UTF-8, deep nesting, bad protocol), unknown methods. Totals: 9,377 requests sent. Transport failures, nonzero exits, hangs, non-JSON replies, sanitizer reports: **0**. Worker kills were not injected.

| Method | sent | ok | err | mutated | mutated ok |
|---|---|---|---|---|---|
| world.create | 345 | 333 | 12 | 0 | 0 |
| world.close | 321 | 321 | 0 | 0 | 0 |
| world.spawn | 1,921 | 1,127 | 794 | 666 | 31 |
| world.despawn | 1,267 | 490 | 777 | 470 | 17 |
| world.step | 1,510 | 1,003 | 507 | 544 | 37 |
| events.read | 1,133 | 459 | 674 | 384 | 13 |
| world.snapshot | 664 | 432 | 232 | 232 | 0 |
| world.restore | 309 | 183 | 126 | 98 | 0 |
| world.inspect | 1,405 | 739 | 666 | 671 | 54 |
| unknown method | 209 | 0 | 209 | 209 | 0 |
| raw malformed bytes | 293 | 0 | 0 | 0 | 0 |

Every valid method succeeded at least once (no method was idle). The 12 `world.create` errors are `SU_E_SCHEMA_MISMATCH` from the scheduler's conflict rule (two parallel systems that write and read one field without an ordering edge; the intended rule, checked by hand). Raw malformed replies are counted under `world.inspect` error codes (`SU_E_PARSE`, `SU_E_DUPLICATE_KEY`). Requests sent are below the loop budget because iterations that send nothing (for example `restore` with no snapshot) were not separately counted.

Snapshot bytes: `snapfuzz` (section 5, item 8), with no sanitizer hits.

## 8. Hygiene

- Commits after `20f2625`: 142, all authored by `admiraly <249692417+admiraly@users.noreply.github.com>`, the same identity as the base commit.
- `Co-Authored-By` in those commit messages: 0. `Claude-Session` trailers: 122.
- License files (`LICENSE`, `LICENSES/`, `COMMERCIAL_LICENSE.md`, `NOTICE.md`): unchanged since `20f2625`. `THIRD_PARTY_NOTICES.md` gained one AsmJit row (zlib, commit pinned).
- Evidence and fixture files: no modification since `20f2625` (additions only, 147,455 lines). Notes that the "regenerated dumps" claim has no changed file behind it.
- Docs vs evidence spot checks (6): IR 0.1 hash (match); 241-check self-test (match); 17/17 and 29/29 ctest (match); `test_churn` 150,047 checks and 24,821 stale refusals (match); TX7 60 release / 62 native and TX8 38 (match); default-cap refusal at 1M rows (match, section 5).
- Working tree left clean: `build/` (created by the demo), caches, and `third_party/asmjit` (bootstrap) removed. All remaining scratch is outside the repo.

## 9. Unrun checks and deviations

- Not run: ASan or TSan ctest of the entity unit tests (the handoff claims these; I built and ran only the `sutekh` target under ASan); full harness on the ASan binary; native ASan; any test with worker kills in the fuzz; `SUTEKH_WORKER_SANDBOX=1` fuzzing; the snapshot writer-order mutant (MX16 as written is equivalent; the writer variant was not run); RSS re-measurement; the two-system 1M admission refusal.
- Deviations from the brief: one build tree per lane (five trees) rather than one; the mutation source copy was restored after each mutant, not deleted (it is 17 MB and remains in scratch); the M2-01 commit `e730672` (contract draft) was not reviewed beyond its diffstat (56 lines, documentation only).
- Observed, not verified against contract: a failed `world.step` reply reports `world_revision now world-1.r1`; whether a failed tick should change the revision was not checked against M0 semantics.

## 10. Commands

```
cmake -S . -B <scratch>/rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build <scratch>/rel
cmake -S . -B <scratch>/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build <scratch>/dev
cmake -S . -B <scratch>/nat -G Ninja -DSUTEKH_NATIVE=ON -DCMAKE_BUILD_TYPE=Release && cmake --build <scratch>/nat
cmake -S . -B <scratch>/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build <scratch>/asan --target sutekh
ctest --test-dir <scratch>/{rel,dev,nat} -j4 --timeout 900
python3 tests/harness/run_m0.py --self-test
python3 tests/harness/run_m0.py --binary <scratch>/{rel,nat}/sutekh --report-dir <scratch>/h_{rel,nat}
python3 tools/demo_zero_wait.py --binary <scratch>/rel/sutekh --out <scratch>/demo_rel.json
<scratch>/rel/test_tick_workers --bench
python3 <scratch>/mutate.py {baseline|all|mutants MX12}   # mutation runner (scratch)
<scratch>/fuzz.py <scratch>/asan/sutekh <worktree> <seed> 6000 <out.json> 1   # seeds 21-23
<scratch>/snapfuzz tests/fixtures/snapshot02/worked_0_2.json 20000 7          # ASan driver
python3 <scratch>/bench_ir02.py <scratch>/rel/sutekh <scratch>/benchroot <N> 2 3
```
