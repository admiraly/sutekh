<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Cross-build determinism matrix (`fixed_q32_v1`, `u32_mod_v1`)

Status: qualified 2026-10-26 (issue #141). This document holds the cross-build claim, the evidence for it, the
divergence audit and its findings. It extends [DETERMINISM_AND_BFME.md](DETERMINISM_AND_BFME.md) section 1
("cross-backend determinism") and the profile text in [NUMERIC_FIXED_Q32.md](NUMERIC_FIXED_Q32.md). The claim
covers only the builds and tiers named below.

## 1. Claim (C08)

**Statement.** Take a world created through the host API (`include/sutekh/host.h`) from the same capsule bytes,
fixture bytes, operator data and `SuHostWorldDesc` values. Give it the same command stream: per-tick input records,
plus host spawn/despawn calls at the same points between ticks. Then every build in the qualified set below produces
bit-identical committed state at every tick: columns, entity table, event log and tick. The consequences:

- `su_host_digest` and `su_host_digest_fast` are identical.
- Snapshots (`su_host_snapshot_write`) and replays (`su_replay_writer_*`) are byte-identical.
- A replay recorded by one qualified build plays back on any other with every keyframe and per-tick digest
  verified (`su_replay_play`).

The claim holds for every combination of execution tiers (`SU_HOST_TIER_REFERENCE`, `SU_HOST_TIER_PLANNED`,
`SU_HOST_TIER_NATIVE`) and for both linkages (static `su_host`, shared `sutekh_host`). It covers the numeric
profiles `fixed_q32_v1` (IR 0.3) and `u32_mod_v1` (IR 0.1/0.2), the entity operations, the event log, and the
space3d host operators.

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
- **CI (GitHub-hosted, observed, section 3):**
  - Linux GCC 15.2 and Clang 21.1 Release with the native tier.
  - Linux GCC 16.2 Release (VM tiers).
  - Windows LLVM clang 20.1 (target `x86_64-pc-windows-msvc`, MSVC libraries and UCRT) Release, with and without the
    native tier.
  - Windows MSVC `cl` 19.51 Release.
  - Debug builds of Linux GCC (with and without native), Windows clang and Windows MSVC.

  A leg stays qualified only while its CI job passes.

**Not claimed.**

- Other architectures (ARM64), 32-bit targets, big-endian hosts (several serializers assume little-endian),
  macOS, and compilers or versions outside the list.
- The native tier on MSVC `cl`. The AsmJit backend is built with clang on Windows; `cl` builds use the VM tiers.
- Floating-point numeric profiles: none exists yet.
- RTS host operators through the host API: they are not part of it (section 2.1); the engine path is covered
  separately.
- Rendering, audio and every host-side computation outside the engine.

A host that derives its command stream from its own logic must make that logic deterministic too (section 5, D2).

## 2. Workloads and comparison

`tests/determinism/det_matrix.c` runs five workloads through the public host API on one tier and writes a manifest.
Per workload, the manifest holds:

- the `su_host_digest` of every keyframe (every 60 ticks, and the last tick);
- the SHA-256 of the sequence of all per-tick digests (`trace`), so a one-tick divergence anywhere is visible;
- the end state's fast digest, event cursor and dropped-write count;
- for replayed workloads, the size and SHA-256 of the canonical replay bytes (keyframe interval 60, per-tick digest
  records).

Every run also plays its own replay back on a fresh world (same build). With `--play-dir` it plays replays
recorded by another build.

| Workload | Systems | Ticks | Content |
|---|---|---|---|
| `rts_duel16` | `rts.damage`, `rts.goal`, `rts.combat`, `rts.move` (capsules/rts IR, edges to the operators removed: `tests/determinism/rts/`) | 600 | locked scene `duel16` (bench/rts/scenes.lock.json, regenerated and digest-checked). Host stand-ins for the operators as input records: rts.scatter's saturating damage sum, nearest-enemy targets every 4th tick (ties to the lowest slot), formation anchors, an attack-move order every 50 ticks |
| `rts_mixed1024` | as above | 600 | locked scene `mixed-1024` (1,024 rows, formations, 558 events) |
| `racer16` | `racer.race`, `racer.move` (IR 0.3), `space3d.query` (host operator) | 720 | 16 racers on the 41 km course, obstacles, gates, collisions; top-speed commands |
| `sixdof` | `det.sixdof` (IR 0.3, `tests/determinism/sixdof.*`) | 1,200 | 48 rigid bodies, see below |
| `churn` | `det.churn` (IR 0.3 with `despawn_self`, `emit_event`) | 900 | entity churn, see below |

`sixdof` bodies:

- Torque and thrust commands drive `quat_integrate`, `quat_rotate` and `v3_clamp_len`. Some quaternions start
  unnormalized. Some top speeds are `MAX`, so positions saturate; some are 0.
- Every tick a probe block stores `mul`, `div`, `sqrt`, `rsqrt`, `v3_len`/`len_sq`/`dot`/`normalize`/`cross`,
  `quat_rotate`, `quat_normalize`, `quat_mul`/`conj`, and `quat_integrate` of an unnormalized quaternion. It also
  stores i64/i32 wrapping `mul`/`div`/`neg`, shifts by 0..71, every conversion, `clamp`, `min` and `max`.
- The operands are drawn from an edge table (0, ±1, ±ONE, ±HALF, `MAX`, `MIN`, `MIN+1`, ±2^62, the 2^52 bounds of
  the fast square root, √2^63, 46,340 and 46,341 m) or are random raw values of random magnitude.

`churn`:

- The IR system drains energy and moves bodies (q32), and despawns rows whose energy is ≤ 0 or whose life is 0. It
  emits a death event and a periodic event.
- Between ticks the host spawns 0..3 entities into the lowest free slots and despawns one every 5th tick. This
  exercises slot reuse, generation growth and a 128-event log ring that evicts.
- Host spawns are not part of the replay format (`SUREPLAY` records input records only), so `churn` is compared by
  per-tick digests only.

**Expectation.** `tests/determinism/expected_digests.txt` is the manifest of Linux GCC 16.2.1 Release on the
reference tier. Each `determinism_matrix_<tier>` ctest compares its build's manifest with it line by line; a
mismatch prints the first differing workload and keyframe. To find the exact tick:

1. Record with `--replay-dir` on one build.
2. Play the replays with `--play-dir` on the other build. `SU_REPLAY_DESYNC` names the tick.
3. Optionally, `--dump-dir` writes every keyframe snapshot so two states can be compared field by field.

### 2.1 Full RTS world through the engine

The RTS native operators (`rts.spatial`, `rts.scatter`) use process-wide scratch. The host API therefore does not
carry them (`src/host/su_host_operators.c`).

`tests/determinism/rts_engine_golden.py` covers them: it replays `tests/determinism/rts_engine_golden.rtsreplay`
through a real `sutekh serve` with the full six-system RTS world. The file was recorded once by Linux GCC 16.2.1
Release. The run has:

- a 192-unit splitmix64 scene;
- orders, a reinforcement batch with anchors, a formation, a despawn, and armed units;
- 27 deaths.

Every one of the 241 boundaries is compared: engine state digest, event digest and counts. These digests are the
engine snapshot format 0.2, not `su_host_digest`. The ctest is `determinism_rts_engine`.

## 3. Results

Locally observed on 2026-10-26: Intel Core i7-14700K, Linux 7.2.9 (Nobara, Fedora 44), glibc 2.43, CMake 4.3.0,
Ninja 1.13.2, Python 3.14.7. Every row below produced a manifest whose SHA-256 is
`4dee8c734aa29da4986aef324d4dd2992371cede603f303b9d887609b1a245cc` (97 lines). Apart from its provenance comment,
this is `expected_digests.txt` line for line. "native" means every system the backend lowers ran as a native kernel; the entity-operation systems
(`rts.damage`, `det.churn`) and `racer.race` stayed on the planned tier, and host operators always run as host code.

| Build | reference | planned | native |
|---|---|---|---|
| GCC 16.2.1 Release (`-O3`), `SUTEKH_NATIVE=ON` | equal | equal | equal |
| GCC 16.2.1 Debug (`-O0`), `SUTEKH_NATIVE=ON` | equal | equal | equal |
| Clang 22.1.8 Release (`-O3`), `SUTEKH_NATIVE=ON` | equal | equal | equal |
| Clang 22.1.8 Debug (`-O0`) | equal | equal | not built |
| Clang 22.1.8 `-O3 -march=native` (FMA, AVX2) | equal | equal | not built |
| GCC 16.2.1 `-O3 -DSU_NUM_FAST_PORTABLE` (no `__int128`, no `divq`) | equal | equal | not built |
| GCC 16.2.1 `-O3 -ffast-math -march=native` (unsupported flags) | equal | equal | not built |
| Clang 22.1.8 RelWithDebInfo, `-fsanitize=undefined -fno-sanitize=function`, `SUTEKH_NATIVE=ON` | equal | equal | equal |

Further local checks:

- **Cross-compiler playback.** Replays recorded by GCC Release were played by the Clang Release static library on
  the reference and native tiers. Clang's replays were played by the GCC Release DLL. Every keyframe and per-tick
  digest matched. The four replay files of the two compilers are byte-identical (`cmp`).
- **`determinism_rts_engine` and `determinism_regress`.** Passed on every build in the table.
- **Full ctest suites** (152 tests each), on GCC Release with native, Clang Release with native, and the UBSan build
  (halt on error): 0 failures, 2 skipped (`native_unwind` is Windows-only; `audio_device` has no device). The UBSan
  test log contains no `runtime error`.

CI legs: GitHub-hosted runners, observed on PR #145, run 40262502027, commit `bd24204`. Every job of the run
passed. The manifests of all 13 leg/tier combinations equal the expectation (`determinism-compare`: "13 of 13
manifests equal").

| Leg | Compiler | Tiers compared with the expectation | Linux GCC replays played |
|---|---|---|---|
| `determinism-linux` gcc, Release, native | GCC 15.2.0 (Ubuntu) | reference, planned, native, DLL | (records them) |
| `determinism-linux` clang, Release, native | Ubuntu clang 21.1.8 | reference, planned, native, DLL | n/a |
| `determinism-windows` clang, Release | clang 20.1.8, x86_64-pc-windows-msvc | reference, planned, DLL | reference, planned: every digest matches |
| `determinism-windows` clang native, Release | clang 20.1.8, x86_64-pc-windows-msvc, AsmJit | reference, planned, native, DLL | reference, planned, native: every digest matches |
| `determinism-windows` MSVC `cl`, Release | MSVC 19.51.36260 | reference, planned, DLL | reference, planned: every digest matches |
| `gcc16` (existing job; Release, native off, full ctest) | GCC 16.2.0 (container) | reference, planned, DLL | n/a |
| `runtime` (Linux checked, Debug; full ctest twice, once with the reference VM default) | Ubuntu GCC | reference, planned, DLL | n/a |
| `runtime-native` (Linux Debug, native; full ctest) | Ubuntu GCC | reference, planned, native, DLL | n/a |
| `runtime-windows` (Debug, full ctest) | clang, x86_64-pc-windows-msvc | reference, planned, DLL | n/a |
| `runtime-windows-msvc` (Debug `/W4 /WX`, full ctest) | MSVC `cl` | reference, planned, DLL | n/a |

Every leg also passed `determinism_regress` and `determinism_rts_engine`, including on Windows. Later commits of the
PR re-run the same jobs; see the handoff for the run on the final head.

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
| Floating point | `su_num_fast.h` uses one `sqrt` of a `double` to seed an integer square root. The result is then corrected with exact integer comparisons, or computed by the reference integer method when the estimate is not within two steps. Integer results therefore do not depend on the FP unit, contraction (GCC's C default is `-ffp-contract=fast`), FMA, MXCSR or the CRT's `sqrt`. Evidence: the `-march=native` and `-ffast-math` rows, the Windows legs, and `num_fast` (fast against reference, run on every CI leg). |
| `__int128`, `_umul128`/`_udiv128`, `divq` | Three implementations of the same exact results. GCC and Clang on Linux and clang on windows-msvc take `__int128` plus `divq`. clang on windows-msvc defines `__SIZEOF_INT128__` and needs no compiler-rt helper: `llvm-nm -u` of a probe of the `su_num_fast.h` operations, cross-compiled at `-O0` and `-O2` with `--target=x86_64-pc-windows-msvc` (with a stub `math.h`, since this machine has no MSVC headers), lists only `sqrt`, `_fltused` and `su_num` symbols: no `__divti3`, `__udivti3` or `__muloti4`. MSVC `cl` takes `_umul128`/`_udiv128`. `SU_NUM_FAST_PORTABLE` covers the limb code. `num_fast` and `num_fast_portable` compare each against `su_num.c`; `num_model` compares `su_num.c` with an independent Python model. |
| Native tier vs VM | Kernels are admitted only after a seeded differential check against the VM. The matrix compares native manifests with the expectation (section 3), and `ir03_native_diff` / `ir04_native_diff` compare kernels with the VM on random programs. Win64 lowering is exercised only by the Windows clang native leg. |
| Threading and scheduling | Chunking depends on `SU_CHUNK_SLOTS` and effective chunk rows, not on the worker count. The command and event merge is keyed by (system, slot, seq) with ties broken by buffer index. Failures pick the lowest failing chunk. The VM's default-tier globals (`su_vm.c`) are plain globals read by workers: a formal data race, but idempotent and tier-neutral (D4). |
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

**Limitations relevant to lockstep hosts.**

- Host spawns and despawns between ticks are not recorded by `SUREPLAY`. A lockstep session must replay them from
  its own command log; the matrix's `churn` workload shows that doing so is deterministic.
- The RTS operators are not available through the host API (section 2.1).
- `su_host_digest_fast` is an XXH64 and is identical across builds. It suits per-tick peer comparison between
  cooperating peers, not trust boundaries (`host.h`).

## 6. Running it

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUTEKH_NATIVE=ON   # NATIVE optional
cmake --build build --target det_matrix det_matrix_dll test_det_regress racer_microcosm sutekh
ctest --test-dir build -R '^determinism_' --output-on-failure
# A manifest, replays, and playback of another build's replays:
build/det_matrix --fixtures build/det_fixtures --tier planned --out m.txt --replay-dir r/
build/det_matrix --fixtures build/det_fixtures --tier native --play-dir other-build/r/
python3 -I tests/determinism/compare_manifests.py tests/determinism/expected_digests.txt m.txt other.txt
```

The fixtures directory is written by the ctest setup tests: `gen_workloads.py fixtures` for the RTS scenes, and
`racer_microcosm --dump-fixture` for the racers. Any change that is meant to alter simulation results must update
`expected_digests.txt` (and `rts_engine_golden.rtsreplay` when the RTS engine path changes) in the same commit, with
the reason. An update made only to make the test pass hides a divergence.
