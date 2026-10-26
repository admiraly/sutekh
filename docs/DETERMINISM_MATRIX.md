<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Cross-build determinism matrix (`fixed_q32_v1`, `u32_mod_v1`)

Status: qualified 2026-10-26 (issue #141). This document holds the cross-build claim, the evidence for it, the
divergence audit and its findings. It extends [DETERMINISM_AND_BFME.md](DETERMINISM_AND_BFME.md) section 1
("cross-backend determinism") and the profile text in [NUMERIC_FIXED_Q32.md](NUMERIC_FIXED_Q32.md).

The claim is a contract only as far as it is tested. Everything it covers is exercised by a matrix workload, and
everything else is named in "Not claimed".

## 1. Claim (C08)

**Statement.** Take a world created through the host API (`include/sutekh/host.h`) from the same capsule bytes,
fixture bytes, operator data and `SuHostWorldDesc` values. Give it the same command stream: per-tick input records,
plus host spawn/despawn calls at the same points between ticks. Then every build in the qualified set below produces
bit-identical committed state at every tick: columns, entity table, event log and tick. The consequences:

- `su_host_digest` and `su_host_digest_fast` are identical.
- Snapshots (`su_host_snapshot_write`) and replays (`su_replay_writer_*`) are byte-identical.
- A replay recorded by one qualified build plays back on any other with every keyframe and per-tick digest
  verified (`su_replay_play`).
- Restoring a mid-run snapshot (`su_host_snapshot_restore` or `_inplace`) and re-simulating the same commands
  reproduces every later state, also on another tier.

**What it covers, exactly.**

- **Tiers.** The tested placements are a run where every IR system is on the same tier (reference, planned, or
  native), and the `mixed` placement where adjacent systems of one world are on different tiers. The mixed cycle is
  native / reference / planned per schedule index, or reference / planned without the native backend. The placement
  each run actually got is part of the expectation (`tests/determinism/expected_tiers.txt`), so a system that falls
  back from native to planned fails the test. Other placements follow from per-system bit equality, but are not
  tested as such.
- **Native tier: which systems.** A system that the native backend lowers runs natively: `rts.goal`, `rts.combat`,
  `rts.move`, `racer.move`, `det.sixdof`, `det.s3move`, `det.s3move_c`, `det.bits04`. Systems with entity
  operations or cross-row reads stay on the VM tiers (the backend refuses them): `rts.damage`, `racer.race`,
  `det.churn`, `det.gather04`.
- **Linkage.** Both linkages: static `su_host` and shared `sutekh_host`.
- **IR.** Every operation of the IR 0.3/0.4 table (`src/ir/su_ops03.c`, 110 operations) runs in a matrix workload
  with edge operands, and its result reaches state. `determinism_op_coverage` fails when an operation in the table
  has no such use. That covers:
  - IR 0.3 (`fixed_q32_v1`) and IR 0.4: bit operations, `div_u32`/`rem_u32`, the row filter, and `gather_*` /
    `find_eq_u32`.
  - The IR 0.1/0.2 u32 and entity operations used by the RTS, racer and churn workloads.

  IR 0.4 is implemented, and qualified here on the VM tiers and on the native tier for modules without cross-row
  reads.
- **space3d host operators.** The query variants `space3d.query`, `space3d.query_prox`, `space3d.query_shape` and
  `space3d.query_compound` run with sphere movers, capsule movers and compound movers against spheres, rotating
  oriented boxes, rotating capsules, rotating static meshes from operator data, and rectangular, disc and
  elliptical gates. 10,780 to 17,234 contacts are reported per 600-tick run, and every result lane feeds the next
  tick's motion (section 2).
- **Entity runtime.** Spawn, despawn, events, slot reuse, generation growth, an evicting event log, and the
  cross-chunk command and event merge (`churn3k`: 3,072 slots, three `SU_CHUNK_SLOTS` chunks).

**Qualified set (x86-64, little-endian).**

- **Observed locally**, Linux x86-64 (glibc 2.43):
  - GCC 16.2.1 at `-O3` (Release) and `-O0` (Debug).
  - Clang 22.1.8 at `-O3` and `-O0`.
  - Clang `-O3 -march=native`, which enables FMA/AVX2 code generation.
  - GCC `-O3` with `SU_NUM_FAST_PORTABLE`, the limb arithmetic used where `__int128` is absent.
  - GCC `-O3 -ffast-math -march=native`. This is not a supported configuration; it shows that no float result
    reaches state.
  - Clang with UBSan.

  Results are in section 3.
- **CI (GitHub-hosted, section 3).** The compilers are pinned by `tests/determinism/ci_compilers.txt`: a runner image
  with another compiler stops the leg at configure time, and the list changes only deliberately.
  - Linux GCC 15.2 and Clang 21.1 Release with the native tier.
  - Linux GCC 16.2 Release (VM tiers).
  - Windows LLVM clang 20.1 (target `x86_64-pc-windows-msvc`, MSVC libraries and UCRT) Release, with and without the
    native tier.
  - Windows MSVC `cl` 19.51 Release.
  - Debug builds of Linux GCC (with and without native), Windows clang and Windows MSVC.

  A leg stays qualified only while its CI job passes.

**Not claimed.**

- **Platforms:** other architectures (ARM64), 32-bit targets, big-endian hosts (several serializers assume
  little-endian), macOS, and compilers or versions outside the list.
- **Native tier:** on MSVC `cl` (the AsmJit backend is built with clang on Windows; `cl` builds use the VM tiers),
  and for the systems named above as refused.
- **Tier placements other than uniform and the `mixed` cycle:** not tested.
- **Numeric profiles:** floating-point profiles (none exists yet).
- **RTS host operators (`rts.spatial`, `rts.scatter`).** They are not part of the host API. Their cross-build evidence
  is the engine recording of section 2.1, which is narrower than the rest of this claim:
  - one recording: a 192-unit scene plus 228 spawned units, at most 420 rows (a single 1,024-slot chunk);
  - the VM tiers of `sutekh serve` only: planned by default, and reference in the CI job that reruns the suite with
    `SUTEKH_VM_TIER=reference`; no native kernels;
  - one world per process: the two-worlds-per-process case of the operators' process-wide scratch is untested;
  - of the budget paths, only the per-query neighbour budget (`RTS_OPS_BUDGET_K`) truncation is exercised; other
    budget and limit paths are not.

  The host-API RTS workloads run the RTS IR systems with host stand-ins for the operators, which is a test of the IR
  systems, not of the operators.
- **space3d operator paths not exercised:**
  - A narrow-phase product on an `INT64_MIN` half extent cannot be reached through the operator, because the
    broadphase bounds of such a box are empty. It is covered directly by `test_det_regress` (D1).
  - Candidate and contact truncation occur (the `flags` counts of the activity lines), but the per-tick sweep
    budget (flag 4) was not checked separately.
- **Out of scope:** rendering, audio, and every host-side computation outside the engine. A host that derives its
  command stream from its own logic must make that logic deterministic too (section 5, D2).

## 2. Workloads and comparison

`tests/determinism/det_matrix.c` runs eleven workloads through the public host API on one tier mode and writes a
manifest. Per workload, the manifest holds:

- the `su_host_digest` of every keyframe (every 60 ticks, and the last tick);
- the SHA-256 of the sequence of all per-tick digests (`trace`), so a one-tick divergence anywhere is visible;
- the end state's fast digest, event cursor and dropped-write count;
- for the space3d workloads, an operator activity line: contacts, movers with two contacts, gate crossings,
  proximity hits, compound sub-shape contacts, and truncation flags;
- for replayed workloads, the size and SHA-256 of the canonical replay bytes (keyframe interval 60, per-tick digest
  records).

Every run also does three playback and rollback checks:

- It plays its own replay back on a fresh world.
- It restores the snapshot of a mid-run tick in place and re-simulates to the end, then does the same into a fresh
  world on another tier. Every per-tick digest must match. The restore tick is off a keyframe, for example 320 of
  600.
- It compares its placement with `expected_tiers.txt`.

With `--play-dir` it plays replays recorded by another build, and checks that the tick, keyframe and digest counts
are this workload's.

| Workload | Systems | Ticks | Content |
|---|---|---|---|
| `rts_duel16` | `rts.damage`, `rts.goal`, `rts.combat`, `rts.move` (capsules/rts IR, edges to the operators removed: `tests/determinism/rts/`) | 600 | locked scene `duel16` (bench/rts/scenes.lock.json, regenerated and digest-checked). Host stand-ins for the operators as input records: rts.scatter's saturating damage sum, nearest-enemy targets every 4th tick (ties to the lowest slot), formation anchors, an attack-move order every 50 ticks |
| `rts_mixed1024` | as above | 600 | locked scene `mixed-1024` (1,024 rows, formations, 558 events) |
| `racer16` | `racer.race`, `racer.move` (IR 0.3), `space3d.query` | 720 | 16 racers on the 41 km course (committed fixture `tests/determinism/fixtures/racer16.json`, equal to `racer_microcosm --dump-fixture`); top-speed commands |
| `sixdof` | `det.sixdof` (IR 0.3) | 1,200 | 48 rigid bodies plus a probe block, see below |
| `probe04` | `det.bits04` (IR 0.4), `det.gather04` (IR 0.4, row filter) | 600 | every family operation of i32/i64/q32, every bit operation, conversion and vector operation the other workloads do not use, gathers, `find_eq_u32`; new edge operands for every row every tick |
| `churn` | `det.churn` (IR 0.3 with `despawn_self`, `emit_event`) | 900 | entity churn in 96 slots (one chunk), see below |
| `churn3k` | as `churn` | 300 | the same in 3,072 slots (three chunks): about 2,800 live entities, up to 47 host spawns per tick, 32 host despawns every 5th tick, IR deaths and events |
| `s3_query`, `s3_prox`, `s3_shape`, `s3_compound` | `det.s3move` (`det.s3move_c` for compound), then the named space3d operator | 600 each | see below |

`sixdof` and `probe04`:

- **`sixdof` bodies.** Torque and thrust commands drive `quat_integrate`, `quat_rotate` and `v3_clamp_len`. Some
  quaternions start unnormalized. Some top speeds are `MAX`, so positions saturate; some are 0.
- **`sixdof` probe block.** Every tick it stores:
  - `mul`, `div`, `sqrt`, `rsqrt`;
  - `v3_len`/`len_sq`/`dot`/`normalize`/`cross`;
  - `quat_rotate`, `quat_normalize`, `quat_mul`/`conj`, and `quat_integrate` of an unnormalized quaternion;
  - i64 `add`/`mul`/`div`/`neg`/`shl`/`shr`, i32 `mul`/`div`/`sub`, and q32 `shl`/`shr`;
  - `q32_raw`, `i32_from_i64`, `q32_to_i32_round`/`_floor`, `q32_to_i64_floor`, `q32_from_i64`, `q32_from_i32`;
  - q32 `clamp`, `min`, `max`, `abs`.
- **`probe04`.** It adds the rest of the table:
  - every one of the 15 family operations for each of i32, i64 and q32, with comparison results reaching state
    through `select`;
  - `and`/`or`/`xor`/`not` for u32 and i64, `shl`/`shr_u32`, `div`/`rem_u32`;
  - `i32_from_u32`, `i64_from_i32`, `q32_from_raw`, `const_i64`, `v3_get`, `v3_neg`;
  - all four gathers (in range, out of range and dead slots) and `find_eq_u32` with spans of 64 and 4,096;
  - the row filter, with a quarter of the rows filtered out.

  Shift counts run 0..71.
- **Operands.** They are drawn from edge tables or are random raw values of random magnitude.
  - q32/i64: 0, ±1, ±ONE, ±HALF, `MAX`, `MIN`, `MIN+1`, ±2^62, the 2^52 bounds of the fast square root, √2^63,
    46,340 and 46,341 m.
  - i32: `INT32_MIN`/`MAX` and their neighbours, ±46,341, ±65,536, 31..64.
  - u32: 0, 1, `0xFFFFFFFF`, `0x80000000`, 31..65,536.

`churn`:

- The IR system drains energy and moves bodies (q32), and despawns rows whose energy is ≤ 0 or whose life is 0. It
  emits a death event and a periodic event.
- Between ticks the host spawns entities into the lowest free slots and despawns some. This exercises slot reuse,
  generation growth and an event log ring that evicts.
- Host spawns are not part of the replay format (`SUREPLAY` records input records only), so the churn workloads are
  compared by per-tick digests (and the rollback check) only.

space3d:

- Each world holds 96 movers in a 24 m arena: spheres, capsules (z0/z1) for `query_shape`, and compounds for
  `query_compound` (two compounds from operator data, one third plain capsules).
- The obstacles: 36 spheres, 30 rotating boxes, 24 rotating capsules, 8 rotating static meshes (two meshes from
  operator data), 6 gates, and a 1 km floor box.
- Three boxes have D1-style extents: two with `INT64_MIN` extents (empty bounds), and one with −1 m extents. The −1 m
  box does reach the narrow phase: 6 to 2,650 sphere sweeps per run, counted with an instrumented build on
  2026-10-26.
- `det.s3move` reflects each mover's velocity on contacts 0 and 1 of the previous tick, pushes out of contacts that
  began overlapped, keeps movers in the arena, and integrates every row's orientation. The operator's contact, normal
  and time-of-impact lanes are therefore inputs to the next tick's motion.
- Commands every 30 ticks kick a quarter of the movers.

**Expectations.**

- `tests/determinism/expected_digests.txt` is the manifest of Linux GCC 16.2.1 Release on the reference tier.
- `tests/determinism/expected_tiers.txt` is the placement of every workload in every tier mode.

Each `determinism_matrix_<mode>` ctest compares its build's manifest and placement with them; a mismatch prints the
first differing workload and keyframe. To find the exact tick:

1. Record with `--replay-dir` on one build.
2. Play the replays with `--play-dir` on the other build. `SU_REPLAY_DESYNC` names the tick.
3. Optionally, `--dump-dir` writes every keyframe snapshot so two states can be compared field by field.

**Sensitivity (mutation check).** `tests/determinism/run_mutants.py --build <dir>` applies one plausible
cross-platform difference at a time, rebuilds `det_matrix`, runs the named workloads against the expectation, and
restores the source. Results on 2026-10-26 (GCC 16.2.1 Release), every mutant caught:

| Mutant | Caught by (first differing keyframe) |
|---|---|
| libm `sqrt` (double) instead of the exact root in the sphere sweep `ray_sphere` | `s3_query`, `s3_prox`, `s3_shape`, `s3_compound` (tick 60) |
| round half up instead of half to even in `su_num_fast.h` (planned/native) | `sixdof`, `probe04`, `racer16` (tick 60) |
| round half up in the reference library `su_num.c` (reference tier) | `sixdof`, `probe04` (tick 60) |
| x86 shift-count masking in `su_u32_shl` | `probe04` (tick 60) |
| `find_eq_u32` returns the last match (reference tier) | `probe04` (tick 60) |
| command/event merge in reverse canonical order (`su_cmdq.c`) | `churn3k` (tick 60). `churn` is not affected: with one chunk there is nothing to merge |

The first mutant is the one an earlier review found undetected: only `racer16` ran `space3d.query`, which made
about 92 sphere sweeps in 720 ticks.

### 2.1 Full RTS world through the engine

The RTS native operators (`rts.spatial`, `rts.scatter`) use process-wide scratch. The host API therefore does not
carry them (`src/host/su_host_operators.c`).

`tests/determinism/rts_engine_golden.py` covers them: it replays `tests/determinism/rts_engine_golden.rtsreplay`
through a real `sutekh serve` with the full six-system RTS world. The file was recorded once by Linux GCC 16.2.1
Release. The run has:

- a 192-unit splitmix64 scene;
- orders, a reinforcement batch with anchors, a formation, a despawn, and armed units;
- a congested blob of 200 armed units within 160 x 40 coordinate units. Every one of them exceeds the per-query
  neighbour budget (`truncated_rows` 200 for 20 ticks).

Every one of the 241 boundaries is compared: engine state digest, event digest and counts. These digests are the
engine snapshot format 0.2, not `su_host_digest`. The ctest is `determinism_rts_engine`. Its limits are listed in
section 1, "Not claimed".

## 3. Results

Locally observed on 2026-10-26: Intel Core i7-14700K, Linux 7.2.9 (Nobara, Fedora 44), glibc 2.43, CMake 4.3.0,
Ninja 1.13.2, Python 3.14.7. Every cell below produced a manifest whose SHA-256 is
`3ce716273cc36c7b682847da0be8dedf803620a6efcc8368a3e1909450e03925` (191 lines). Apart from its provenance comment,
this is `expected_digests.txt` line for line.

Each cell also passed the placement comparison and the in-place and fresh-world rollback checks of all eleven
workloads. A DLL run (planned) passed on every build. "mixed" is the `mixed_vm` cycle on builds without the native
backend.

| Build | reference | planned | mixed | native |
|---|---|---|---|---|
| GCC 16.2.1 Release (`-O3`), `SUTEKH_NATIVE=ON` | equal | equal | equal | equal |
| GCC 16.2.1 Debug (`-O0`), `SUTEKH_NATIVE=ON` | equal | equal | equal | equal |
| Clang 22.1.8 Release (`-O3`), `SUTEKH_NATIVE=ON` | equal | equal | equal | equal |
| Clang 22.1.8 Debug (`-O0`) | equal | equal | equal | not built |
| Clang 22.1.8 `-O3 -march=native` (FMA, AVX2) | equal | equal | equal | not built |
| GCC 16.2.1 `-O3 -DSU_NUM_FAST_PORTABLE` (no `__int128`, no `divq`) | equal | equal | equal | not built |
| GCC 16.2.1 `-O3 -ffast-math -march=native` (unsupported flags) | equal | equal | equal | not built |
| Clang 22.1.8 RelWithDebInfo, `-fsanitize=undefined -fno-sanitize=function`, `SUTEKH_NATIVE=ON` | equal | equal | equal | equal |

Further local checks:

- **Cross-build playback.**
  - Every build played the nine replays of GCC Release on the reference and planned tiers.
  - The GCC Release DLL played every build's own replays.
  - Every keyframe and per-tick digest matched, and every build's replay files are byte-identical to GCC's.
- **Per build.** `determinism_regress`, `determinism_rts_engine` and `determinism_op_coverage` (110 of 110) passed on
  every build in the table.
- **Full ctest suites** (GCC and Clang Release with native, Clang UBSan), after merging `origin/main`: 167 tests
  each, 0 failures, 2 skipped (`native_unwind` is Windows-only; `audio_device` has no device), and no UBSan
  `runtime error`.

CI legs: observed on PR #145, run 40271629352, commit `16c99ca`. All 14 jobs passed. Details are in
`planning/handoffs/2026-10-26-determinism-matrix.md`.

| Leg | Compiler (pinned) | Tier modes | Replays played |
|---|---|---|---|
| `determinism-linux` gcc, Release, native | GNU 15.2.0 | reference, planned, mixed, native; DLL | (recorded) |
| `determinism-linux` clang, Release, native | Clang 21.1.8 | reference, planned, mixed, native; DLL | (recorded) |
| `determinism-windows` clang, Release | Clang 20.1.8, x86_64-pc-windows-msvc | reference, planned, mixed_vm; DLL | Linux GCC and Linux Clang, every mode: 54 of 54 |
| `determinism-windows` clang native, Release | Clang 20.1.8, msvc target, AsmJit | reference, planned, mixed, native; DLL | Linux GCC and Linux Clang, every mode: 72 of 72 |
| `determinism-windows` MSVC `cl`, Release | MSVC 19.51.36260.0 | reference, planned, mixed_vm; DLL | Linux GCC and Linux Clang, every mode: 54 of 54 |
| `determinism-play-windows-on-linux` | GNU 15.2.0, native | reference, planned, mixed, native | all three Windows legs: 108 of 108 |
| `determinism-compare` | | | 36 of 36 manifests (static and DLL, every mode, every leg) equal the expectation |

The existing jobs also run the determinism ctests on their builds, and all passed: `gcc16` (GCC 16.2 Release),
`runtime` (Debug), `runtime-native`, `runtime-windows` (clang Debug) and `runtime-windows-msvc` (Debug).

## 4. Divergence audit

The authoritative paths audited:

- `src/host`, `src/ir` (`su_num.c`, `su_num_fast.h`, verifier, JSON, SHA-256);
- `src/vm`, `src/world`, `src/runtime` (schedule, entity tick, jobs, command queue);
- `profiles/space3d/native`, `profiles/rts/native`;
- the native backend's 0.3 helpers and lowering.

Verdicts:

| Source | Verdict and evidence |
|---|---|
| Signed overflow and shifts | Arithmetic goes through unsigned wraparound and `su_i*_from_bits`. Shifts at or above the width are defined by the profile and handled before shifting. `shr` is computed without a signed right shift, and `MIN / -1` is a special case. **One exception was found and fixed:** D1. The UBSan build, which runs the matrix and the full suite, reports nothing else. |
| Implementation-defined behaviour | The signed right shift of `__int128` in `su_num_fast.h` is used only where `__SIZEOF_INT128__` is defined (GCC, Clang), and those compilers document it as arithmetic. Out-of-range unsigned-to-signed conversions go through `su_i64_from_bits`. Parsers read bytes as `unsigned char` or `uint8_t`, so plain `char` signedness never matters. |
| Evaluation order | No unsequenced side effects in engine code. Found in the test driver itself (D2). |
| `long` / `size_t` width (LLP64 vs LP64) | No `long` reaches state; it is used for field-table indexes only. Serializers write `size_t` lengths as `u64`, field by field. |
| Padding and uninitialized bytes | No struct is hashed or serialized whole. Bytes are written field by field with little-endian `put_u32`/`put_u64`. `SuEvent` (40 bytes) and `SuStructCmd` (32 bytes) carry size asserts and have no padding. Recordings and snapshots are byte-identical across compilers (section 3). |
| libc in tick paths | `qsort` is used at world creation and in input merging: `su_host.c` input records, `su_world.c` ids and fields, `snapshot.c`, `su_lir.c`. Every comparator is a strict total order over keys that are proven unique, so glibc's merge sort and the MSVC CRT's introsort give the same order. The space3d sweep and mesh sorts and the command-queue merge are the engine's own stable sorts over unique keys. `strcmp` order is defined as unsigned-char comparison. No `rand`, `time`, locale or `strtod` reaches state. |
| Floating point | `su_num_fast.h` uses one `sqrt` of a `double` to seed an integer square root. The result is then corrected with exact integer comparisons, or computed by the reference integer method when the estimate is not within two steps. Integer results therefore do not depend on the FP unit, contraction (GCC's C default is `-ffp-contract=fast`), FMA, MXCSR or the CRT's `sqrt`. Evidence: the `-march=native` and `-ffast-math` rows, the Windows legs, and `num_fast` (fast against reference, run on every CI leg). A libm square root in the space3d narrow phase would be caught (section 2, mutation check). |
| `__int128`, `_umul128`/`_udiv128`, `divq` | Three implementations of the same exact results. GCC and Clang on Linux and clang on windows-msvc take `__int128` plus `divq`. clang on windows-msvc defines `__SIZEOF_INT128__` and needs no compiler-rt helper: `llvm-nm -u` of a probe of the `su_num_fast.h` operations, cross-compiled at `-O0` and `-O2` with `--target=x86_64-pc-windows-msvc` (with a stub `math.h`, since this machine has no MSVC headers), lists only `sqrt`, `_fltused` and `su_num` symbols: no `__divti3`, `__udivti3` or `__muloti4`. MSVC `cl` takes `_umul128`/`_udiv128`. `SU_NUM_FAST_PORTABLE` covers the limb code. `num_fast` and `num_fast_portable` compare each against `su_num.c`; `num_model` compares `su_num.c` with an independent Python model. |
| Native tier vs VM | Kernels are admitted only after a seeded differential check against the VM. The matrix compares native manifests and placements with the expectations (section 3), and `ir03_native_diff` / `ir04_native_diff` compare kernels with the VM on random programs. Win64 lowering is exercised only by the Windows clang native leg. |
| Threading and scheduling | Chunking depends on `SU_CHUNK_SLOTS` and effective chunk rows, not on the worker count. The command and event merge is keyed by (system, slot, seq) with ties broken by buffer index, and is exercised across chunks by `churn3k`; the mutation check shows the matrix detects a change to it. Failures pick the lowest failing chunk. The VM's default-tier globals (`su_vm.c`) are plain globals read by workers: a formal data race, but idempotent and tier-neutral (D4). |
| Endianness | The serializers write little-endian. Two places assume a little-endian host: `snapshot.c` writes one byte from an `unsigned`, and `su_world.c` v1 restore `memcpy`s column bytes. Out of scope: big-endian is not claimed. |

## 5. Findings

**D1. Signed overflow in the space3d box sign products (fixed; regression fixture).**

- *Where:* `profiles/space3d/native/space3d_geom.c`. The edge/vertex region of `s3_sweep_sphere_obb`
  (`out[i] * half.x`) and `box_corner`, which `s3_sweep_capsule_obb` and `s3_dist_seg_obb` reach through
  `box_edge`, multiplied a sign by a box half extent read from a world lane.
- *Trigger:* a half extent of `INT64_MIN`, which is a legal q32 input, for example from a command, makes
  `-1 * INT64_MIN` a signed overflow. That is undefined behaviour, which an optimizer may compile differently from
  one compiler or version to the next. UBSan reported it: `space3d_geom.c:269: signed integer overflow:
  -1 * -9223372036854775808`.
- *Fix:* `sign_mul` computes the wrapping product. That is the value GCC 16.2 `-O3` and Clang 22.1 `-O3` produced
  before the fix, so no observed result changed.
- *Half extents are not validated.* A negative extent describes no box, and two negations of it meet deliberately.
  The slab and clamp tests use the profile's saturating `su_q32_neg` (−`INT64_MIN` = `INT64_MAX`), while the corner
  products use the wrapping `sign_mul`. Both are fixed functions of the input bits, as documented at `sign_mul`.
- *Reachability through the operator.* An `INT64_MIN` extent never reaches the product, because its bounds are empty.
  A small negative extent does reach the narrow phase deterministically (section 2, space3d).
- *Fixture:* `tests/determinism/test_det_regress.c` (ctest `determinism_regress`). It checks 108 answers taken from
  the pre-fix code of both compilers (identical) for six half extents, two orientations and three axes, over sphere
  and capsule sweeps and distance.

**D2. Unspecified evaluation order in host-side command generation (fixed in the driver; guidance).**

- *Where:* the first version of `det_matrix.c` drew two random numbers inside one expression, for example
  `(int64_t)rng() * ONE + rng()`. C leaves the order of those calls unspecified (C17 6.5p3).
- *Effect:* GCC and Clang happened to agree, so the local matrix could not show the problem, but another compiler
  may legally reverse the order and produce a different command stream.
- *Fix:* the draws are now separate statements (`rng_q32`).
- *Guidance:* lockstep hosts must sequence every stateful call that feeds a command stream the same way. The engine
  cannot protect a host from this.

**D3. UBSan `-fsanitize=function` is incompatible with the native tier (tooling; no engine defect).**

- *Where:* clang's function-type check reads a signature word in front of every indirect call target.
- *Effect:* a JIT kernel at the start of a fresh page has no readable bytes in front of it, so a
  `-fsanitize=undefined` build faults in `su_native_run_checked`, in `native_scalar`, `ir03_native_diff` and
  `determinism_matrix_native`.
- *Workaround:* build sanitizer runs with `-fno-sanitize=function`; the suite then passes on every tier.

**D4. `su_vm.c` default-tier globals are a formal data race (documented, not changed).** The tiers are
bit-identical, so whichever value a worker reads yields the same results. Making them atomic is left to the VM
owner.

**D5. Earlier overclaims of this document (corrected in the fix round of 2026-10-26).** The first version
overclaimed in four places:

- It said "every combination of execution tiers", but only uniform per-run tiers were tested.
- It implied that every IR 0.3 operation and every conversion ran, but 45 of the 110 table operations never did.
- It covered "the space3d host operators" when only `space3d.query`, at low volume, ran.
- It did not compare where systems ran, so a native fallback would have passed.

Section 1 now states only what the matrix tests, and tests were added for the rest: `probe04`, the four space3d
workloads, `churn3k`, the rollback check, the mixed tier mode, the placement expectation, op coverage and the
mutation check.

**Limitations relevant to lockstep hosts.**

- Host spawns and despawns between ticks are not recorded by `SUREPLAY`. A lockstep session must replay them from
  its own command log. The churn workloads, including their rollback check, show that doing so is deterministic.
- The RTS operators are not available through the host API, and their engine evidence is limited (section 1, "Not
  claimed").
- `su_host_digest_fast` is an XXH64 and is identical across builds. It suits per-tick peer comparison between
  cooperating peers, not trust boundaries (`host.h`).

## 6. Running it

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=ON   # NATIVE optional
cmake --build build --target det_matrix det_matrix_dll det_op_list test_det_regress racer_microcosm sutekh
ctest --test-dir build -R '^determinism_' --output-on-failure
# A manifest, replays, and playback of another build's replays (tier: reference, planned, native or mixed):
build/det_matrix --fixtures build/det_fixtures --tier planned --out m.txt --replay-dir r/ \
    --expect tests/determinism/expected_digests.txt --expect-tiers tests/determinism/expected_tiers.txt
build/det_matrix --fixtures build/det_fixtures --tier native --play-dir other-build/r/
python3 -I tests/determinism/compare_manifests.py tests/determinism/expected_digests.txt m.txt other.txt
# Sensitivity: every mutant must be caught (edits and restores sources; maintainers only)
python3 -I tests/determinism/run_mutants.py --build build
```

The fixtures directory holds the RTS scenes, written by the ctest setup test (`gen_workloads.py fixtures`); the
racer scene is committed. Any change that is meant to alter simulation results must update `expected_digests.txt`,
plus `expected_tiers.txt` when placements change and `rts_engine_golden.rtsreplay` when the RTS engine path
changes, in the same commit, with the reason. An update made only to make the test pass hides a divergence. The
same holds for `ci_compilers.txt`.
