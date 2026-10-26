<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 144: schedule system cap and capsule-independent world schemas

## State and scope

Locally verified on `feat/issue-144-schedule-schema`, based on `origin/main` `31192d9` (merge of PR #147). CI and
integration are pending.

Exact modified paths:

- `src/runtime/schedule.h`, `src/runtime/schedule.c`: `SU_SCHED_MAX_SYSTEMS` 64 (array size),
  `SU_SCHED_DEFAULT_SYSTEMS` 8; per-schedule admission cap (`max_systems`, `su_sched_max_systems`,
  `su_sched_set_max_systems`); `su_sched_declare_schema`; `su_sched_finalize` starting from the declared lanes and
  refusing fields outside them; `su_sched_explain_mismatch_ex` (names the system that disagrees with the declaration).
- `src/runtime/jobs/su_effects.c`: effect conflicts by a merge over ID-sorted field tables (same result).
- `src/runtime/worker.c`: every `SU_SCHED_MAX_SYSTEMS` became `SU_SCHED_DEFAULT_SYSTEMS` (mechanical rename; the
  control plane's arrays and admission stay at 8).
- `include/sutekh/host.h`: `SU_HOST_MAX_CAPSULES` 64; `SuHostWorldDesc.schema` appended,
  `SU_HOST_WORLD_DESC_V1_SIZE`; new section "declared world schemas" (`SuHostSchema`, `SuHostSchemaField`,
  `SU_HOST_SCHEMA_MAX_LANES`); model, creation and field-info comments.
- `src/host/su_host.c`, `src/host/su_host_internal.h`: v1/v2 descriptor sizes, capsule-count refusal, the schedule cap,
  heap-allocated capsule facts, schema declaration and lane expansion, declared-schema checks and messages, identity
  fold, schema-only lanes as read-only world columns, 64-bit operator-data mask, failing-system bound.
- `tests/host/test_host_schedule.c` (new; static with internal checks, and DLL), `tests/host/test_host_compat.c` (new,
  first-release API only), `tests/host/sched_fixtures.h` (new), `CMakeLists.txt` (3 tests).
- `docs/IMPLEMENTATION_STATUS.md`, `docs/AGENT_RPC.md` (section 18: the worker keeps 8 systems).
- `planning/handoffs/2026-10-26-schedule-schema.md` (this file).

## What was implemented

### 1. Schedule size

- A host world takes 1..64 schedule entries (IR capsules and host operators, `SU_HOST_MAX_CAPSULES`). 65 or more is
  `SU_E_RESOURCE_LIMIT` with `err->index` 64 (the first capsule that does not fit) and the message "N capsules: a world
  schedules at most 64 systems (capsule 64 does not fit)". 0 stays `SU_E_INVALID_ARGUMENT`. Before, 9..N were
  `SU_E_INVALID_ARGUMENT` ("capsules (1..8)").
- The cap is a per-schedule admission value, not only an array size. `su_sched_init` gives 8, so every control-plane
  schedule (worker `world.create`, restore, migration, replace) admits 8 as before. `su_host_world_create` raises its
  schedule to `SU_HOST_MAX_CAPSULES`. `su_sched_add*` refuses at the schedule's own cap (`SU_E_RESOURCE_LIMIT`).
- Audit of everything sized by or encoding the system count or index:

| Place | Before | Now |
|---|---|---|
| `SuSchedule.sys[]` | 8 entries | 64 entries (about 27 KB per schedule instead of 3.4 KB; one per world plus a migration slot in the worker) |
| `SuHostWorld.ops[]` | `SU_SCHED_MAX_SYSTEMS` | same macro (64) |
| `SuHostWorld.opdata_mask` | `uint32_t`, `1u << i` | `uint64_t`, `UINT64_C(1) << i`; `_Static_assert(SU_HOST_MAX_CAPSULES <= 64)`. With 32 bits, entry 32 and later were undefined shifts (test: operator data at index 40) |
| `CapsuleFacts facts[]` in world creation | 8 on the stack | `calloc(capsule_count)` (each holds 2 x 64 edge pointers) |
| `check_effects` `SuEffectInput in[]` | 8 | 64 on the stack (about 5 KB) |
| `su_effects_build` | 256 systems max, conflicts O(n^2 * lanes^2) | unchanged limit; conflicts O(n^2 * lanes) by merge (below) |
| entity tick cache (`entity_tick.c`, per-system arrays and `ps/pe`) | `SU_SCHED_MAX_SYSTEMS` | 64; heap struct, `ps/pe` 512 bytes on the stack |
| `su_cmdq` buffers | `nsystems * nchunks` | unchanged; `su_ent_budget` sums every system |
| step failure detail | `failed < SU_SCHED_MAX_SYSTEMS` | `failed < w->sched.count` |
| worker arrays (`RevPaths` 8 KB each, `CkptSys`, manifests, migration, txids, drops) | `SU_SCHED_MAX_SYSTEMS` (8) | `SU_SCHED_DEFAULT_SYSTEMS` (8): unchanged sizes; raising them to 64 would put about 1 MB of `RevPaths`/`CkptSys` on some worker stacks |
| agent (`AGENT_MAX_SYSTEMS` 8) | 8 | unchanged |
| event `system_index` | u32 | unchanged |
| host snapshot `SUHSNAP` | no system count | unchanged |
| world identity | u32 count + per-system records | unchanged encoding |
| replay header | u32 count; reader refused count > 8 | reader refuses count > 64 (`SU_HOST_MAX_CAPSULES`); writer unchanged |
| worker snapshot manifests | count from the schedule (<= 8) | unchanged |
| native kernels | 32 live per process | unchanged: at 64 systems the 33rd `SU_HOST_TIER_NATIVE` request is refused (`SU_E_CAPABILITY_DENIED`, "native artifact registry full (32 live)"); the system keeps its VM tier |

- Effect analysis: `conf[i][j]` is "some field ID bound by both systems conflicts", so it does not depend on the order
  in which fields are compared. With unique IDs per table (verified modules and host declarations), a merge over
  ID-sorted tables gives the same matrix. Sorting is an insertion sort over an index array (linear on the ascending
  tables the verifier produces). A table with duplicate IDs, or no memory for the index array, falls back to the old
  exhaustive loop. The message loop that names the first conflicting field is unchanged (it runs once, on failure).
- Edge lists: `schedule.after/before` items are read up to `SU_SCHED_MAX_SYSTEMS` per capsule (64; was 8). Only a
  capsule listing more than 8 items, which in an 8-system world must repeat names or name unknown systems, sees a
  difference: items 9..64 are now checked instead of being dropped silently.

### 2. Declared world schemas

- `SuHostWorldDesc.schema` (appended; `struct_size` may be `SU_HOST_WORLD_DESC_V1_SIZE` (80 bytes LP64/LLP64, the
  first release; only those bytes are read) or `sizeof(SuHostWorldDesc)`). NULL keeps the union schema exactly.
- `SuHostSchema {struct_size, field_count, fields, schema_id, schema_version, layout_generation}`, `SuHostSchemaField
  {id, type, name}`. Types u32/i32/i64/q32. Lane rule as IR 0.3: an i64/q32 field "x" at J is lanes J "x.lo" and J + 1
  "x.hi" with the field's value type, so an IR 0.3 capsule declaring i64 "x" binds exactly those lanes and a host
  operator record lists both. Lane IDs and names must be unique; at most 1,024 lanes; names 1..255 bytes (252 for
  i64/q32). Malformed: `SU_E_INVALID_ARGUMENT`, `err->index` = field index.
- Binding checks: every capsule and host record must have the declared schema ID, version and layout generation
  (`SU_E_SCHEMA_MISMATCH`, capsule index, both triples in the message), and every lane it binds must be a declared lane
  with the same ID, name, storage and value type (`SU_E_SCHEMA_MISMATCH`, capsule index, "the declared world schema
  has id I "a" (t), S has id J "b" (u)" or "S binds field id I "a" (t), which the declared world schema does not have",
  with the lane hint). These are the union-table checks with the declaration as a closed starting table.
- Schema-only lanes (access 0 in the union table, reported as access 0 by `su_host_field_info`) are read-only world
  columns: no staging, written by the host between ticks (input records, `su_host_input_push_i64`, spawn values). World
  access is in neither the schema digest nor the canonical state.
- Identity: `SHA-256("SUHSCHEMA\0" || identity as without a schema || u32 len || schema ID || u32 version || u32
  generation || u32 lane count || per lane, ascending ID: u32 id | u32 value type | u32 len | name)`, folded before
  operator data. The value type matters because `su_world_schema_digest` does not cover it (a schema-only lane declared
  i32 instead of u32 changes nothing else). Consequence: worlds with and without a declaration never exchange
  snapshots or replays, even with identical columns; this is deliberate and documented in `host.h`.
- Not done: field defaults (columns start at 0, as without a schema), a world with zero systems (`capsule_count` stays
  1..64), declared schemas in the worker (`world.create`), migrations of a declared schema.

## Design decisions

- **Cap as an admission value plus a raised array maximum**, instead of a world-creation parameter in the public
  descriptor: one documented maximum (`SU_HOST_MAX_CAPSULES` 64) is simpler for hosts. The per-schedule cap keeps the
  control plane at exactly 8 without touching its formats or stack sizes.
- **Control plane unchanged.** The worker, agent, checkpoint and migration paths keep 8 systems per world (their
  arrays and RPC limits are unchanged; `docs/AGENT_RPC.md` says so). Raising them is a separate change: it touches
  agent protocol limits, the agent snapshot bound (`AGENT_MAX_SYSTEMS * 512`) and about 1 MB of worker stack arrays.
- **Descriptor versioning**: `SuHostWorldDesc` grows by one trailing pointer; the old size stays accepted and only
  `struct_size` bytes are read, as for `SuHostOperatorDesc`. API version stays 1 (additive section, as before).
  Compile-time detection: `#ifdef SU_HOST_SCHEMA_MAX_LANES`.
- **Logical fields, not lanes**, in `SuHostSchemaField`: the same shape capsules declare. Lane names follow the
  verifier's expansion.
- **Error codes**: malformed declaration `SU_E_INVALID_ARGUMENT` (as malformed operator descriptors); disagreement
  `SU_E_SCHEMA_MISMATCH` (as union conflicts), with the capsule index (union conflicts without a declaration keep
  `UINT32_MAX`, as before).

## Local validation

Linux 7.2.9 (Fedora 44 based), Intel Core i7-14700K, GCC 16.2.1, Clang 22.1.8, CMake 4.3.0 (Unix Makefiles), `-j 6`.
`SUTEKH_WERROR` ON (`-Wall -Wextra -pedantic -Werror`). Renderer and glTF importer skipped by configure, as on the
baseline. Build trees outside the repository.

```sh
cmake -S . -B <dir>/dbg -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B <dir>/rel -DCMAKE_BUILD_TYPE=Release
python3 tools/bootstrap_asmjit.py   # pinned commit, lock unchanged
cmake -S . -B <dir>/nat -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=ON
cmake -S . -B <dir>/asan -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build <dir> -j 6 && (cd <dir> && ctest -j 6)
```

| Configuration | Build | ctest |
|---|---|---|
| GCC Debug | 0 warnings | **139/139 passed**, 1 skipped (`audio_device`: no Linux audio backend) |
| GCC Release | 0 warnings | **139/139 passed**, same skip |
| GCC Debug `SUTEKH_NATIVE=ON` | 0 warnings | **156/156 passed**, 2 skipped (`native_unwind` Windows only, `audio_device`) |
| Clang Debug ASan+UBSan | 0 warnings | 21 of 139 failed, 1 skipped: **the identical set fails on the baseline** (below); the 3 new tests pass |

New tests: `host_schedule` (185 checks static, with the internal schedule checks; 191 with the native backend),
`host_schedule_dll` (174), `host_compat` (12), 0 failures in every configuration. What they cover:

- 64-system chain (system k reads v_k, v_{k+1}, stores v_{k+1} = 3 v_k + v_{k+1} + k + 1, after system k - 1): order
  of `su_host_system_info`, every column against a reference model after each of 10 ticks (the result depends on the
  order), two worlds digest-identical every tick, a pinned digest at tick 10, allocating and in-place restore with
  re-simulation, replay record (with an input), seek and play; the v1 descriptor size gives the same identity.
- Admission: 65 capsules (code, index, message); 0 capsules; a missing edge at 64 systems (conflict named: systems
  `chain.s39`/`chain.s40`, field 41); a back edge (ordering cycle); entity budget: 8 event-emitting systems fit at
  100,000 rows, 64 are refused ("entity tick buffers"); 64 emitting systems at 4 rows: 256 events in (system, slot)
  order, system indices 0..63; native: 32 of 64 kernels install, system 32 refused with its message, mixed tiers
  give the VM digest; operator data on entry 40 (data used, second call names entry 40, identity changes); the
  schedule itself (static only): default cap 8, 9th refused, cap bounds, 64 admitted, 65th refused, declaration only
  on an empty schedule, cap reset by `su_sched_free`.
- Declared schema `t.sch` (8 fields, 10 lanes): `target` (host-written, read by `sch.follow` in the same tick),
  `wide` i64 (host-written with `su_host_input_push_i64`, read by the IR 0.3 system `sch.wide`), `opin` (host-written,
  read only by the registered operator `test.sq`), `opout` (operator result read by `sch.use` after it), `kind` (bound
  by nothing: spawn values and inputs, read back); a model checks every lane of every entity for 10 ticks; field info
  and value types; snapshot (allocating and in place) and replay round trips; identity differs from the same capsules
  without a declaration and with a schema-only lane of another value type, and a snapshot does not cross.
- Refusals: capsule name, undeclared field, declared name at another ID, i64 bound as q32, schema ID, version and
  generation, an operator lane of another value type (each `SU_E_SCHEMA_MISMATCH` with the capsule index and a
  message check); malformed declarations (duplicate lane ID, duplicate name, i64 high lane on another field's ID, a
  field named like another's lane, unknown type, empty or NULL name, q32 at `UINT32_MAX`, no fields, struct_size, NULL
  schema ID); a fixture without a schema-only column (index = its field ID), and one with it.

ASan+UBSan baseline: `origin/main` `31192d9` extracted with `git archive` (plus `test_host_compat`) and built with the
same flags: 136 tests, the same 21 failures (sets compared with `diff`: identical) and the same skip. Report counts in
both `LastTest.log`s are equal: 4 LeakSanitizer reports, 2 heap-use-after-free lines (the pre-existing
`su_replay_open` error path named in the issue-137 handoff), 0 UBSan runtime errors. The 21 tests are the ones listed
in the issue-137 handoff: `containment`, `control`, `host_replay`, `host_rollback`, `host_rollback_dll`,
`rts_b1_hidden`, `rts_b1_install`, `rts_b2_hidden`, `rts_b2_install`, `rts_b3_hidden`, `rts_b3_install`,
`rts_caps_engine`, `rts_ctrl_engine`, `rts_e2e_edges`, `rts_load_engine`, `rts_model_digest`,
`rts_projectile_mutant`, `rts_replay_engine`, `rts_world_set_fields`, `rts_world_spawn_batch`, `space3d_serve`. Not
diagnosed here.

**Worlds without a declared schema are unchanged.**
1. The whole existing suite passes unchanged, with its pinned digests.
2. `host_compat` (first-release API only) pins the world identity and the tick-12 digest of 1-, 3- and 8-system chain
   worlds with inputs; the values were taken from `origin/main` `31192d9` (the same source built against it).
3. Cross-build comparison (scratch script, not committed): the same commands on the baseline Release build
   (`origin/main` `31192d9`, `git archive`, plus `test_host_compat`) and on this branch's Release build. Identical
   files: `test_host_compat dump` (replay bytes, final snapshot bytes, identity and every tick's SHA-256 and fast digest
   for the three worlds; 9 files), the canonical replay of `test_replay record` (SHA-256 prefix `232e8f2a5ccf4613`, as in
   the issue-137 handoff), `arena_host --ticks 900 --rollback-check` with and without `--respond`, `minimal_host`. The
   `racer_microcosm --n 8 --ticks 720 --digest-every 120` output differs only in its timing fields; its state digests,
   final state and event-log digests are identical.

**Mutation check** (scratch script, not committed; each mutant built and both new tests run, then the source restored):
12 mutants, 11 killed, 1 equivalent.
- Killed: the declared table not closed (undeclared field accepted); no identity fold; no value type in the fold;
  32-bit operator-data mask (two variants); conflict matrix not set by the merge; 65 capsules with the wrong code;
  system index of a declared mismatch not reported; high lane at the wrong ID; value types not compared in the union;
  host cap 63 instead of 64.
- Equivalent: schema-only lanes created read-write instead of read-only in the world (only staging memory differs; no
  observable result).

Repository checks: `python3 tools/validate_pack.py --check-report` passed; `python3 tools/check_repository_license.py`
passed; `git diff --check` clean.

## Observations (C10)

Not budgets. i7-14700K, Release, `taskset -c 3`, scratch program `bench144.c` (not committed, SHA-256
`ef696d39e46408271af144a7dd2b2b2893c01907677b2928e809d7e9bc58f9c2`), linked to `libsutekh_host.so`; chain worlds;
world creation 21 samples, ticks 400 after 40 warm-up; median (p95), microseconds.

| Systems | Rows | Create | Tick |
|---|---|---|---|
| 8 | 1,000 | 314 (333) | 84 (89) |
| 32 | 1,000 | 962 (1,302) | 333 (355) |
| 64 | 1,000 | 2,438 (2,550) | 675 (705) |
| 8 | 10,000 | 2,149 (2,403) | 862 (891) |
| 32 | 10,000 | 9,988 (10,576) | 3,463 (3,546) |
| 64 | 10,000 | 19,625 (20,338) | 7,000 (7,214) |

The baseline library with 8 systems: 345 (370) / 87 (92) at 1,000 rows, 2,163 (2,371) / 886 (914) at 10,000 rows, the
same as this branch within noise. The chain systems are tiny (8 instructions, 2 lanes); a wide-table worst case
(64 systems x 1,024 lanes) was not measured.

## SHA-256 evidence

Sources as committed (the handoff itself excluded):

- `include/sutekh/host.h` `ccca26e4b5a180414c6e1aa8cf2ec8b39b1bec16fba1bc85d852a5a6ed18b1d1`
- `src/host/su_host.c` `c31f065e0c8c9271f8cdaad31da8e826d41918990c5f448d97b4a93df00764b7`
- `src/host/su_host_internal.h` `eae23f6dd38a3b193fd10d8fdec3bc66fd3a9f4cc4cb246030993bbd71854937`
- `src/runtime/schedule.c` `d9d3f34b001493fbedc2a26e470100d12bce17db7d80e3ec4521f34e77a92a93`
- `src/runtime/schedule.h` `0d22bf58eb820ae60e3fdad2ef5a201feeb90b03928460f87d62c607867d3c7a`
- `src/runtime/jobs/su_effects.c` `5d30739b0f7d76ec05924edb32a32d470b4cd707241794517edd171bb88e9ad4`
- `src/runtime/worker.c` `ffad124aa907819608870faf40098f14406d09fc7482d3f00bcd954aaa1883f0`
- `tests/host/test_host_schedule.c` `4a6d0ec07a162c567a9bed44ffdc8f73f224fa8be627e7408a2eee36a274b186`
- `tests/host/test_host_compat.c` `ed822f3999a210de8f2967fa709c6eb7c2ef54ad93e939099e56761768472eff`
- `tests/host/sched_fixtures.h` `90a72a86c0214ee8a91451c12a236bbb333eff0d347ae7b0cf2f06aa4f6f57ff`
- Release `libsutekh_host.so` `45ecc479627a83c77cb5f7c05b82bb5671dc5d8bf45f27e6b2a63ee42f42643f`

## Not run / open

- MSVC, clang-cl, macOS, ILP32 (`SU_HOST_WORLD_DESC_V1_SIZE` is `offsetof(seed) + 8`; `_Static_assert`s pin it to
  the offset of `schema` and, with 8-byte pointers, to 80 bytes; 32-bit layouts were not compiled), ThreadSanitizer,
  CI.
- The worker and `sutekh serve` do not get 64 systems or declared schemas (see design decisions).
- Effect-analysis timing at 1,024 lanes per system; no wide-table benchmark.

## Next work

- Optional: raise the control plane's limit (agent, worker arrays on the heap, RPC limits) if the out-of-process path
  needs more than 8 systems.
- Optional: field defaults in `SuHostSchemaField` (the world supports defaults through its schema extension).
