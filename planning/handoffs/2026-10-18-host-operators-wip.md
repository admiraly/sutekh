<!-- SPDX-FileCopyrightText: 2026 admiraly -->
# Handoff (WIP): host operators in host.h, rollback through the host API, native kernel quality, MSVC

Branch `feat/host-operators` (worktree `engines/sutekh-worktrees/host-operators`), based on `origin/feat/sim-runtime`
8713294; `origin/main` (sim-runtime merged as ef610d5) merged in at b4cfe7c (`PACK_VALIDATION.json` regenerated, not
merged). Draft PR: https://github.com/admiraly/sutekh/pull/34 (CI only; the integrator merges). Head at the time of
writing: 66ef14d. Stopped on an owner usage-budget request; a successor continues.

## Done (commits, in order)

| Commit | What |
|---|---|
| 5f125c5 | Host operators + execution tiers in `host.h` (records via `lir_json == NULL`; per-world operator scratch; tier API) |
| b4cfe7c | Merge origin/main |
| 755739f | Example `examples/host_space3d` (game schema `arena`, schema-relative bound space3d record, gates/bumps, rollback drills) |
| 394be8e | Native IR 0.3 register allocation; hoisted instructions once per call; identity lanes memcpy'd per column |
| 00e6036 | Fast q32 numerics: signed 128-bit accumulation, branchless RNE (shared by planned VM, space3d, native helpers) |
| 01c23a4 | Per-call overhead: env tier read once, cursor/binary binding lookups, O(n) commit_staging and native alias checks |
| 62bf0e0 | `bench/host/bench_host_rollback.c` + `run_host_rollback.py`; evidence `planning/evidence/host-operators/` |
| 3aca404 | Windows x64 unwind data for native kernels (RtlAddFunctionTable), `test_native_unwind` |
| eb4151e | `test_native_avx2`: XSAVE areas zeroed before dirtying YMM (fixes the Linux native CI failure "CHECK failed: feasible") |
| 66ef14d | MSVC cl /W4 /WX clean + passing; CI job `runtime-windows-msvc`; input record size bug (88 -> 92) fixed |

### New API surface (`include/sutekh/host.h`, additive, API version stays 1)
- `SuHostCapsule` with `lir_json = NULL, lir_len = 0`: `capsule_json` is a host operator record (exact or
  schema-relative "binds"); refused with `SU_E_CAPABILITY_DENIED` when no built-in operator has that system_id.
- `su_host_operator_count`, `su_host_operator_info` (built in: `space3d.query` / `space3d_ops_v1`).
- `SU_HOST_TIER_{REFERENCE,PLANNED,NATIVE,OPERATOR}`, `su_host_native_available`, `su_host_system_tier`,
  `su_host_system_set_tier` (native = scalar kernel admitted by differential check; entity-op systems refused;
  native registry is process-global and not thread-safe: document says use one thread for native worlds).
- Internal: `su_host_load_bound_bytes` (host_decl.h), `SuSysEntry.host_inst/host_inst_free/vm_tier`,
  `su_sched_set_host_instance`, `space3d_query_instance_create/destroy`, `su_world_find_binding(_from)`,
  `su_native_exec_publish_unwind`, `SuNativeLowered.unwind/unwind_size/func_end`, `SU_IO_COUNT` (su_posix.h),
  `sutekh_force_include()` (SutekhPlatform.cmake). Env switches: `SUTEKH_NATIVE_REGALLOC=0`,
  `SUTEKH_NATIVE_NO_UNWIND=1` (test/diagnostic seams).
- Operator results are world columns, so snapshots, digests, in-place restore and replays carry them (tested).

### Tests (Windows 11, clang 23.1.3 / MSVC 19.44, after `. .\tools\env-windows.ps1`)
| Command | Result |
|---|---|
| `ctest --test-dir build-win -j 8` (clang Debug, default) | 90/90 (path_fifo, containment skipped as on main) |
| `ctest --test-dir build-nat -j 8` (clang Debug, SUTEKH_NATIVE=ON) | 105/105 (same 2 skips) |
| same with `SUTEKH_VM_TIER=reference` (earlier commit) | passed |
| `ctest --test-dir build-msvc` (cl, `-DCMAKE_C_COMPILER=cl`, /W4 /WX, needs cl + SDK rc.exe on PATH) | 0 diagnostics, 90/90 |
| `test_host_ops` / `test_host_ops_dll <fixture>` (ctest host_ops, host_ops_dll; fixture by host_ops_fixture) | 2934 checks (2945 native), 0 failures: pinned racer digests da12f70c/c52e176c through host.h, rollback K=6/12 re-emits events, replay, identity-bound record, 2 worlds on 2 threads |
| mutant: schedule ignores per-world instance | threads test fails 3/3 |
| `arena_host --ticks 900 --rollback-check --expect-digest 3b1f7e1d...` (ctest example_arena_host, _native) | 3 gate + 8 bump events, 30 drills 0 differed, pin on VM and native |
| `ir03_diff.py --native --programs 1000 --seed 1..5`, allocation on and off | 0 mismatches (4.3M lanes); mutant (no reload after helper) 149 mismatches |
| `test_num_fast 1000000 7` | 53,002,904 checks, 0 failures |
| `test_native_unwind` (ctest native_unwind, Windows) | 24 checks 0 failures; with `SUTEKH_NATIVE_NO_UNWIND=1` lookup fails, process dies 0xC0000005 |
| ASan (clang -fsanitize=address) test_input | before fix: stack-buffer-overflow in su_input_record_encode; after: 800 checks 0 failures |

### Rollback through the public host API (observations, Release, i5-12400F, source 01c23a4)
`run_host_rollback.py --n 8,16,32,256 --tiers reference,planned,native --k 6,12 --repeat 3 --iters 200`; total =
restore_inplace + step(K) + digest_fast, p50 / 8.33 ms (full table in `planning/evidence/host-operators/host-rollback-bench.json`):

| N | planned K=6 | planned K=12 | native K=6 | native K=12 | restore p50 us |
|---|---|---|---|---|---|
| 8 | 1.52% | 3.02% | 1.48% | 2.25% | 4-5 |
| 16 | 3.41% | 3.95% | 1.78% | 2.92% | 5-8 |
| 32 | 3.10% | 6.00% | 2.05% | 4.12% | 6-7 |
| 256 | 16.88% | 33.81% (was 44.3%) | 11.11% | 22.10% (was 29.1%) | 18-22 |

Register allocation on vs off: within noise (`regalloc-off/`); the kernel is helper- and multiply-bound, the
gains came from the numerics and per-call overhead (found with an in-process sampling profiler, see below).

## Exactly where I stopped
- Last push 66ef14d (MSVC job added to `.github/workflows/repository-checks.yml`; CI for it NOT yet observed).
- Check PR #34 CI: the Linux native job should now pass (AVX2 probe fix); the new `runtime-windows-msvc` job is
  unverified on GitHub (vcvars import step written, not run there). If the push token cannot change workflows,
  the job may need the owner to apply it.
- Not yet done: the dated final handoff (this WIP file stands in), extending the downstream amendment
  (`planning/amendments/2026-10-18-sim-runtime.md` or `-downstream-host-work.md`) with this work's scope,
  docs for the tier API beyond host.h comments, SysV unwind registration (not required), Linux ASan lane.
- Scratch profilers (not committed): `build-prof/prof_sample.c` (SuspendThread/RIP sampler + llvm-symbolizer),
  `build-rel/prof_subsets.c`, `prof_sys.c`, `bench_helpers.c`, `dump_move.c`. Remaining hot spots at N=8 (about 18
  us/tick native): space3d.query fixed cost (~85 ns per static obstacle: bounds recomputed each tick), racer.move
  per-call fixed ~3.5 us, memcpy in commit/identity copies.
- Contract notes for the integrator / other agent: `SU_INPUT_RECORD_BYTES` 88 -> 92 (input.h; presentation
  stream's input layer: a buffer overflow fix); `test_host_rollback.c` one CHECK became a _Static_assert;
  `su_vm_default_tier` reads the environment once; AVX2 kernel frame is now MSVC style (rbp set after the
  allocation); `SuNativeLowered` grew unwind fields.

## Original brief (verbatim, remaining work for the successor)

You are a dedicated engineer on **Sutekh**, the owner's (GitHub: admiraly) custom C17 headless, agent-native game engine. Develop it in exactly the ways a downstream game needs. You work autonomously over a long session and have standing permission to commit and push. Never idle waiting on CI: push, keep working, and check later.

## Where you work
- Worktree: `C:\Users\dev\Documents\trinity\engines\sutekh-worktrees\host-operators`, branch `feat/host-operators`, based on `origin/feat/sim-runtime` (8713294).
  - That base is PR #32, which the parent is about to merge to main. When it lands, merge `origin/main` into your branch; never rebase or force-push.
  - Regenerate `PACK_VALIDATION.json` with `tools/validate_pack.py --write-report` instead of merging it.
- The canonical checkout `engines\sutekh` belongs to the parent; don't edit it.
- `local\` (LLVM 23.1.3, and AsmJit if bootstrapped there) and `.venv\` are junctions. Dot-source `tools\env-windows.ps1`.
- Shell: PowerShell 5.1 (Bash also available). Git: `C:\Program Files\Git\cmd\git.exe`. gh: `C:\Program Files\GitHub CLI\gh.exe`.
- Machine: i5-12400F (12 threads), 16 GB RAM, no WSL. Linux results come from CI.
- If something needs UAC elevation, don't start it; nobody is there to approve it.

**Parallel agent:** another agent works in `...\sutekh-worktrees\presentation-assets` (branch `feat/presentation-assets`). It owns render, assets (glTF), materials, effects, HUD text and audio. Don't touch those. You own the simulation runtime: IR/VM/native, scheduler, space3d, and the `host.h` simulation-side API. Keep edits to shared files small.

## Read first
- `planning/handoffs/2026-10-18-sim-runtime.md` (the previous agent's report). It includes a proposal for host-operator support in `su_host`.
- Plus the handoffs `2026-10-18-presentation-input.md`, `-numeric-3d.md` and `-windows-native-host.md`.
- `docs/SPACE3D.md`, `docs/NUMERIC_FIXED_Q32.md`, `AGENTS.md`, `ENGINE_CONSTITUTION.md`, and the downstream-work amendment in `planning/amendments/` (extend it).

Follow Sutekh's process rules:
- real tests, no fake PASS;
- exact commands and results;
- a dated handoff in `planning/handoffs/` (today is 2026-10-18);
- timings labelled as observations;
- digest-identical differential checks against the reference tier for every optimisation.

## What the downstream game needs (priority order)
A 6DOF racer embeds Sutekh in-process through `host.h`: a 120 Hz authoritative sim of 8–16 racers plus projectiles and pickups, with client-side rollback that re-simulates K = 6–12 ticks per rendered frame.

1. **Host operators in the in-process `su_host` API.** Today a game embedding Sutekh via `host.h` cannot use `space3d.query` or any host operator. That blocks the game.
   - Implement the previous agent's proposal or a better one: load bound host records (`su_host_load_bound` / schema-relative space3d binding) into a `host.h` world. Include operator results/events in snapshots, digests, in-place restore and replays, so rollback re-simulation reproduces them bit-identically.
   - Test it end to end through the in-process API, including the DLL build, against the `sutekh serve` path and the existing racer digests.
   - Update `examples/presentation/space_demo` (coordinate minimally; it's the other agent's area) or add a separate small example showing an embedded world with space3d collisions and gate events.
2. **Rollback path through the host API.**
   - Rebase the rollback benchmark on `su_host_snapshot_restore_inplace`, plus the fast digest where appropriate.
   - Measure restore + K = 6/12 re-step at N = 8/16/32/256 through the public host API on the planned VM and native tiers.
   - Optimise the remaining hot spots; the 256-racer native case is about 29% of the budget at K = 12, but the game's realistic N is 8–16.
3. **Native kernel quality.**
   - Register allocation for IR 0.3 native kernels (at least keeping hot values in registers across a row loop instead of spilling every op).
   - Windows x64 unwind info (RtlAddFunctionTable or equivalent) for native kernels, so crashes and debuggers behave. Test it with the existing crash seam.
4. **MSVC in CI** if cheap: a Windows job building with cl/clang-cl at the repo's warning level. Fix warnings properly rather than relaxing them, but if older code has many MSVC `/W4` issues, scope the job honestly and note it.

Ship coherent, tested commits, push regularly, and open a draft PR to main for CI after item 1. If verification debt grows, stop and repair.

## Hard rules
- Sutekh is a **PUBLIC** repo (PolyForm Perimeter; SPDX headers on new files).
  - No third-party game assets or names (redacted from the brief here: the repository is public).
  - Don't mention the downstream game's name or specifics; use generic terms only.
- No force-push, no history rewrites, no deleting others' work, no licence changes.
- Commit with `-c user.name=admiraly -c user.email=249692417+admiraly@users.noreply.github.com`. End each message with:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Don't merge to main; the parent integrates.

## Final report
Include:
- what is done, with exact test commands and results;
- the new API surface;
- benchmark tables (observations);
- what is unrun and why;
- commits and the PR URL;
- contract changes for the integrator or the other agent;
- next steps.

### Status against the brief
- Item 1: done (API, per-world scratch, records exact/bound, snapshots/digests/restore/replay, static + DLL tests,
  pinned racer digests, new example). The `sutekh serve` comparison is indirect: the same pinned in-process digests
  that `space3d_serve` compares lanes against; no new serve test was added.
- Item 2: done (host-API bench, evidence table above, hot spots optimised); further small-N work listed above.
- Item 3: register allocation done (measured: no gain on this workload, reported honestly); unwind info done and
  tested with an in-kernel fault (the worker crash seam faults outside the kernel, so a direct fault test was used).
- Item 4: cl /W4 /WX clean locally, job added; CI result pending.
- Remaining: confirm CI (Linux native, new MSVC job); extend the amendment; final dated handoff with the final
  report sections above; optional further small-N optimisation (space3d static bounds, per-call fixed cost).
