<!-- SPDX-FileCopyrightText: 2026 admiraly -->
# Handoff: simulation runtime for the 6DOF workload (rollback budget, planned VM tier, native IR 0.3, control plane, schema-relative space3d)

Branch `feat/sim-runtime` (worktree `sutekh-worktrees/sim-runtime`), from `feat/numeric-3d` 6670707; `origin/main`
merged twice (numeric-3d as 66cf3a7, presentation-input as 15b0dd7; `PACK_VALIDATION.json` regenerated, never
merged). Owner-authorized amendment: `planning/amendments/2026-10-18-sim-runtime.md` (outside the milestone order;
M3 gates unchanged). Draft PR #32 runs CI; the integrator merges.

## 1. Rollback resimulation budget

- **Benchmark.** `racer_microcosm --rollback 1,6,12 [--rollback-iters N] [--vm-tier reference|planned] [--native]`:
  save the world after the warm-up (canonical bytes), then per K restore it and re-step K ticks in one
  `su_sched_run_ticks` call, timing save, restore and resimulation separately; every resimulated digest must equal
  the reference digest (exit 1 otherwise). `run_bench.py --rollback-evidence <dir> [--tiers ...]` runs N = 8, 32,
  256 x tiers x K, 3 interleaved repeats, and fails if any digest differs within or across tiers.
- **Planned VM tier** (`src/vm/su_vm_plan.[ch]`, default; `SUTEKH_VM_TIER=reference` or
  `su_vm_set_default_tier`/`su_vm_context_set_tier` select the reference interpreter). Built per context when one
  static pass over the straight-line body proves the reference's per-row checks redundant (every register read
  follows a write of its type in the row; every writable lane stored once). Then: pre-decoded dispatch; row-invariant
  instructions (constants and pure ops on them whose register is defined once) evaluated at plan build; an
  identity-row guard: a symbolic pass over selects, v3/quat make/get and bool and/or/not finds boolean requirements
  under which every store writes its own loaded value and nothing is emitted; rows meeting them are copied (as
  memcpy runs of consecutive rows) instead of executed. Without a plan, or with an unbound lane, the reference runs.
  Statuses, `failed_insn` and emission order are the reference's.
- **Exact fast numerics** (`src/ir/su_num_fast.h`): inline functions returning the bits of `su_num.c` for every
  input: 128-bit multiply intrinsics, signed 128-bit accumulation for operands in [-2^62, 2^62) (reference
  otherwise), hardware 128/64 divide when the quotient fits, integer sqrt from a double estimate corrected in
  integers (roots >= 2^52 use the reference). Used by the planned tier, space3d and the native helpers.
  `SU_NUM_FAST_PORTABLE` builds the intrinsic-free variant.
- **Per-call state reuse** (`src/runtime/entity_tick.c`): command queue, job pool, VM contexts and plans cached in
  `SuEntRt`, keyed on schedule identity, module pointer + semantic hash + revision + txid, rows, workers, caps and
  VM tier; the world is rebound per call (restore replaces it); a cached module pointer is never dereferenced.
- **space3d set-up**: fast numerics; the gate's in-plane axes only after a side change; the sweep sort starts from
  the previous order when the collidable set and axis are unchanged (unique total order, so identical result;
  bounded fallback to the merge sort).

Rollback cost, observations (`planning/evidence/sim-runtime/`, source c8d7cb3, Release, 200 iterations x 3
processes; restore + resim p50, worst process; every resimulated digest identical across tiers):

| N | VM tier | K | restore p50 us | resim p50 us (min..max of 3 runs) | per tick p50 us | total p50 / 8.33 ms | total p99 / 8.33 ms | save p50 us |
|---|---|---|---|---|---|---|---|---|
| 8 | reference | 1 | 26.8 | 179.8..182.4 | 182.4 | 2.5% | 10.2% | 2.2 |
| 8 | reference | 6 | 30.5 | 1062.0..1122.2 | 187.0 | 13.9% | 27.4% | 2.2 |
| 8 | reference | 12 | 30.7 | 2111.0..2193.8 | 182.8 | 26.7% | 51.1% | 2.2 |
| 8 | planned | 1 | 38.0 | 25.5..37.7 | 37.7 | 0.9% | 1.3% | 2.1 |
| 8 | planned | 6 | 46.3 | 138.3..261.2 | 43.5 | 3.7% | 9.1% | 2.1 |
| 8 | planned | 12 | 26.9 | 274.3..276.6 | 23.1 | 3.6% | 6.7% | 2.1 |
| 8 | native | 1 | 24.4 | 24.0..25.6 | 25.6 | 0.6% | 1.2% | 3.2 |
| 8 | native | 6 | 26.0 | 121.1..124.7 | 20.8 | 1.8% | 3.1% | 3.2 |
| 8 | native | 12 | 27.3 | 234.5..244.8 | 20.4 | 3.3% | 4.5% | 3.2 |
| 32 | reference | 1 | 29.3 | 242.7..246.3 | 246.3 | 3.3% | 6.1% | 3.1 |
| 32 | reference | 6 | 34.0 | 1507.9..1631.7 | 271.9 | 20.0% | 32.5% | 3.1 |
| 32 | reference | 12 | 32.5 | 2982.0..2999.8 | 250.0 | 36.4% | 83.4% | 3.1 |
| 32 | planned | 1 | 27.9 | 45.8..47.9 | 47.9 | 0.9% | 1.3% | 2.7 |
| 32 | planned | 6 | 31.1 | 302.7..316.6 | 52.8 | 4.2% | 7.3% | 2.7 |
| 32 | planned | 12 | 31.2 | 613.3..621.2 | 51.8 | 7.8% | 14.3% | 2.7 |
| 32 | native | 1 | 27.5 | 36.7..37.8 | 37.8 | 0.8% | 2.2% | 4.1 |
| 32 | native | 6 | 30.7 | 217.9..228.6 | 38.1 | 3.1% | 5.7% | 4.1 |
| 32 | native | 12 | 31.0 | 446.4..459.9 | 38.3 | 5.9% | 10.2% | 4.1 |
| 256 | reference | 1 | 55.7 | 837.3..863.5 | 863.5 | 11.1% | 21.1% | 10.0 |
| 256 | reference | 6 | 59.5 | 5048.8..5228.7 | 871.5 | 63.5% | 97.8% | 10.0 |
| 256 | reference | 12 | 60.2 | 10115.6..10304.9 | 858.7 | 124.4% | 173.7% | 10.0 |
| 256 | planned | 1 | 53.0 | 286.9..289.1 | 289.1 | 4.1% | 8.8% | 8.4 |
| 256 | planned | 6 | 56.3 | 1812.9..1816.2 | 302.7 | 22.5% | 40.5% | 8.4 |
| 256 | planned | 12 | 57.6 | 3626.8..3630.1 | 302.5 | 44.3% | 81.9% | 8.4 |
| 256 | native | 1 | 54.2 | 182.1..183.6 | 183.6 | 2.9% | 4.7% | 8.1 |
| 256 | native | 6 | 57.5 | 1181.6..1183.2 | 197.2 | 14.9% | 26.3% | 8.1 |
| 256 | native | 12 | 58.2 | 2360.4..2363.3 | 196.9 | 29.1% | 45.2% | 8.1 |

Per-tick timing, planned tier, one tick per call (before this work N = 8 / 32 / 256 with the query: 173 / 252 /
904 us p50):

| N | query | rows | p50 us (min..max of 3 runs) | p99 us | max us | worst p99 / 8.33 ms | gates | collision ticks | events | digests equal across runs | replay start/ckpt |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | yes | 72 | 24.0..24.5 | 27.5..50.0 | 55.2..189.8 | 0.60% | 24 | 1 | 25 | True | True/True |
| 8 | no | 72 | 15.5..15.7 | 22.0..29.0 | 31.3..92.1 | 0.35% | 0 | 0 | 0 | True | True/True |
| 32 | yes | 96 | 52.4..53.3 | 91.0..112.6 | 154.6..187.9 | 1.35% | 97 | 7 | 104 | True | True/True |
| 32 | no | 96 | 39.8..41.3 | 69.5..79.6 | 76.0..1339.0 | 0.96% | 0 | 0 | 0 | True | True/True |
| 256 | yes | 320 | 320.8..337.7 | 483.0..545.4 | 587.5..640.8 | 6.54% | 574 | 800 | 1374 | True | True/True |
| 256 | no | 320 | 260.4..260.9 | 449.2..458.7 | 501.4..953.2 | 5.50% | 0 | 0 | 0 | True | True/True |

## 2. Native tier on Windows and IR 0.3 lowering

- AsmJit pin `0d7f010` fetched by `tools/bootstrap_asmjit.py`; fixed for Git for Windows (`core.autocrlf=false`,
  LF lock file): the pinned license hash now matches. AsmJit headers are a SYSTEM include.
- Windows ports: `tests/native/native_test_mem.h` (guard pages, mappings, W^X, "store into code faults" via SEH,
  OS AVX2 oracle, monotonic clock) for `native_scalar`, `native_avx2`, `native_versions`; the four control-plane
  qualification drivers (`specialize_admission`, `native_fault`, `specialize_inflight`, `specialize_churn`) on
  `tests/common/su_test_os.h`. Two seam fixes found on the way: the native crash seam raises a real access
  violation on Windows (raise(SIGSEGV) only exits with code 3 there), and `SU_TEST_INTERLEAVE` is one-shot per
  supervisor instead of per process (identical on POSIX).
- **Win64 ABI.** `test_native_scalar` has a Win64 trampoline poisoning rbx rbp rdi rsi r12-r15 and xmm6-xmm15 and
  checking them after a call with shadow space, for the scalar and AVX2 kernels: both ok. ABI probe
  (`bootstrap_asmjit.py --build-probe --cxx clang++`): the Win64 call (rcx, rdx) returns the right values;
  **finding**: AsmJit's own `JitRuntime` maps code RWX on Windows. Sutekh never uses `JitRuntime` (artifacts are
  RW then RX, checked by the W^X tests).
- **IR 0.3 lowering** (`backends/native/su_native_lower03.cpp`, helpers `su_native_ops03.c`): every operation of a
  0.3 module without entity operations, scalar caps. Inline: constants, loads/stores (64-bit lanes), u32/bool,
  i32/i64 arithmetic, compares, min/max/clamp, selects, saturating q32 add/sub/neg/abs, the q32 multiply
  (branchless RNE, saturation), v3 add/sub/neg/scale, moves. Helpers (Win64 shadow space, aligned stack): div,
  shifts, sqrt/rsqrt, two conversions, dot, cross, len, normalize, clamp_len, quaternion ops. Row-invariant
  instructions run once before the loop; the VM plan's identity-row guard is reused. AVX2 for 0.3 is refused
  (`ir_0_3_avx2_not_lowered`, `world.specialize auto` picks scalar); 0.3 with entity ops is refused like 0.2.

## 3. Control plane

`tests/space3d/test_space3d_serve.py` (ctest `space3d_serve`): a real `sutekh serve` builds the racer world from a
fixture the driver dumps (`--dump-fixture`), with `space3d.query` as a host operator; `world.step`,
`world.inspect` (every lane of every row against the in-process state at T/2 and T), `world.snapshot`,
`world.restore` + re-step, `events.read`; on native builds `world.specialize racer.move` (admission 42 cases, 0
mismatches) with identical lanes and snapshot sha256. **Bug found and fixed:** `world.create` refused every IR 0.3
module (`su_effects_derive` knew only u32 loads/stores).

## 4. Schema-relative space3d binding

A host record may bind the operator to the game's schema: every `host.fields` entry carries the game lane's id,
name, type, access and `"binds": "<operator lane>"`; `schema.id` names the game schema. `su_host_load_bound`
verifies the bijection, types, access, ascending IDs, unique names and 64-bit lanes on game IDs J, J+1; the
declaration carries the game's lanes and a semantic hash covering the binding; `su_sched_add_host_bound` stores
it and `run_host` hands the operator the game columns under its own IDs (operator code unchanged). `docs/SPACE3D.md`.

## Tests (exact commands; Windows 11, clang 23.1.3; after `. .\tools\env-windows.ps1`)

| Command | Result |
|---|---|
| `cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang; ninja -C build-win; ctest --test-dir build-win -j 6` (after the main merge) | 84/84 passed (path_fifo, containment skipped as on main) |
| same with `$env:SUTEKH_VM_TIER='reference'` | 84/84 passed |
| `cmake -S . -B build-nat ... -DCMAKE_CXX_COMPILER=clang++ -DSUTEKH_NATIVE=ON; ninja -C build-nat; ctest --test-dir build-nat -j 6` | 97/97 passed (same two skips; the four qualification drivers now run) |
| same with `SUTEKH_VM_TIER=reference` | 97/97 passed |
| `test_num_fast 200000 1` / `test_num_fast_portable 100000 2` (ctest num_fast, num_fast_portable) | 15,902,904 / 5,302,904 checks, 0 failures; rounding mutant: 5,826 failures |
| `test_vm_tiers` (ctest vm_tiers) | 235 checks, 0 failures |
| `ir03_diff.py --programs 300 --rows 48 --seed 1` (ctest ir03_diff); extra seeds 7 (1500 programs) and 3 (600) | 0 mismatches on both tiers; clean guarded programs all guarded; 2 guard-proof mutants detected (8 mismatches; guard not exercised) |
| `ir03_diff.py --native --programs 600 --seed 11` (ctest ir03_native_diff); seeds 1-5 x 1000 programs | every kernel's lanes identical to the VM; mutants detected: tie rounding (5 programs), add saturation (10) |
| `run_bench.py --check` (ctest space3d_racer_replay) | pinned digests da12f70c.../c52e176c..., replay ok |
| `racer_microcosm --n {8,32,256} --ticks 1200 --replay` with `--vm-tier reference`, planned, `--native` | final, periodic and event digests and race totals identical |
| `test_space3d_bind` (ctest space3d_bind) | 263 checks, 0 failures; 10 refusals each for its intended rule |
| `test_space3d_serve.py` (ctest space3d_serve) | default build 10 checks, native build 16 checks, 0 failures (game schema VM and native ok) |
| `test_native_scalar` / `test_native_avx2` (ctest native_scalar, native_avx2) | 188 checks incl. 5000-program corpus and Win64 clobber checks for scalar and AVX2 kernels / 245,497 three-engine runs, PASSED |
| `specialize_admission`, `native_fault`, `specialize_inflight`, `specialize_churn` (Windows) | pass (272, 219 checks; inflight; churn 1000 cycles) |
| `bootstrap_asmjit.py --build-probe --cxx clang++` | Win64 call correct; FINDING: JitRuntime maps RWX on Windows (not used by Sutekh) |
| MSVC 19.44 `cl /O2 /W3 /std:c17` scratch builds of test_num_fast and test_vm_tiers | 10,602,904 and 235 checks, 0 failures |
| `tools/validate_pack.py --check-report`, `tools/check_repository_license.py` | pass |
| Linux CI (PR #32) | run 39133125627 at c8d7cb3: Linux checked runtime 83/83, and 83/83 again with `SUTEKH_VM_TIER=reference`; Linux native tier (AsmJit fetched and verified, `SUTEKH_NATIVE=ON`) 95/95; Windows checked runtime 87/87; validate ubuntu and windows pass |

## Not run / open

- Linux results come only from CI (PR #32: Linux checked runtime, the new Linux native-tier job, and the new
  reference-tier re-run). MSVC is not in CI; scratch MSVC 19.44 `/O2 /W3` builds of `test_num_fast` (10.6M checks,
  the `_umul128`/`_udiv128` path) and `test_vm_tiers` (235 checks) passed.
- Native kernels have no Windows unwind tables (`RtlAddFunctionTable`); IR 0.3 kernels call helpers, so a fault
  inside a helper cannot be unwound through the kernel frame. Recovery is process-level (worker replacement), so
  nothing relies on it today.
- The in-process host API (`su_host`, presentation stream) accepts IR capsules only: a game embedding Sutekh cannot
  yet use `space3d.query` there. Proposal: accept a host record in `SuHostCapsule` (`lir_json` NULL), resolve it with
  `su_host_find` and a byte-based `su_host_load_bound`; needs the host registry (and the RTS operators it names) in
  the host library.
- Native IR 0.3 kernels keep every register in a stack slot (no register allocation); AVX2 lowering of 0.3 not done.
- Timing figures are observations on a shared machine, not qualification.

## Contract changes for the other stream and the integrator

- `src/vm/su_vm.h`: tier API (`SU_VM_TIER_*`, `su_vm_set_default_tier`, `su_vm_default_tier`,
  `su_vm_context_set_tier`, `su_vm_context_tier_info`); environment `SUTEKH_VM_TIER=reference`. Default behaviour is
  bit-identical; anything that asserted reference-only internals should select the reference tier.
- `src/runtime/schedule.h`: `SuSysEntry.host_bind` (owned, NULL = descriptor IDs), `su_sched_add_host_bound`.
  `src/runtime/rts/host_decl.h`: `su_host_load_bound`; `su_host_load` now refuses bound records.
- `src/runtime/entity_tick.c`: `SuEntRt` caches per-call state; `su_ent_rt_destroy` frees it.
- Native: `SuNativeLowerInput` gained helper, guard and pre-loop fields (internal); `su_native` links `su_vm`;
  0.3 + SCALAR compiles (was `ir_0_3_not_lowered`).
- `src/runtime/jobs/su_effects.c`: IR 0.3 field operands are derived (fixes `world.create` of 0.3 modules).
- `src/agent/su_agent.c`: `SU_TEST_INTERLEAVE` one-shot per supervisor. `src/runtime/native_versions.c`: Windows
  crash seam. `su_ir` links `m` on UNIX.
- CI: Linux native-tier job; Linux suite re-run with `SUTEKH_VM_TIER=reference`.

## Next steps

1. Host API support for host operators (proposal above), so the downstream game can embed `space3d.query`.
2. Native IR 0.3 register allocation for the hot body (values live across a row in registers), then measure K = 12
   at N = 256 against the planned tier.
3. Windows unwind info for native kernels.
4. If rollback restore dominates at small N, use the presentation stream's in-place restore in the rollback bench.
