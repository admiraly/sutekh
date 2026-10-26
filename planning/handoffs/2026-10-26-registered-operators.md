<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->
# Issue 137: game-defined (registered) host operators

## State and scope

Locally verified on `feat/issue-137-registered-operators`, based on `origin/main`
`07d06de` (merge of PR #129). CI and integration are pending.

Exact modified paths:

- `include/sutekh/host.h` (new section "registered host operators"; listing and refusal text of "host operators")
- `src/host/su_host_registered.c` (new: registry, per-world instances, the run adapter)
- `src/host/su_host_internal.h` (`HostOperator`: callbacks receive their operator; `reg`, `last_error`, `record_name`)
- `src/host/su_host_operators.c` (built-in table adapted; listing = built-in, then registered)
- `src/host/su_host.c` (record check with the explicit name, instance creation, operator-data family check,
  failure detail in step errors, refusal text)
- `src/runtime/rts/host_decl.c`, `src/runtime/rts/host_decl.h` (`su_host_decl_build_named`,
  `su_host_load_bound_bytes_named`; NULL name = the previous functions exactly)
- `cmake/SutekhHostOps.cmake` (new source; 8 new ctest entries)
- `tests/host/test_host_registered.c` (new)
- `docs/SPACE3D.md` (section 1, "Registered operators"), `docs/AGENT_RPC.md` (world.create: not visible to the
  worker), `docs/IMPLEMENTATION_STATUS.md`
- `planning/handoffs/2026-10-26-registered-operators.md`

## What was implemented

`su_host_register_operator(const SuHostOperatorDesc *, SuHostError *)`, called before `su_host_world_create`. The
descriptor carries `struct_size`, `flags`, system ID, schema ID, operator name and version, constants hash, the field
table (`SuHostOperatorField`: id, value type u32/i32/i64/q32, access, name), `scratch_size(capacity)`, `run`, optional
`parse_data`/`release_data` and a `user` pointer. Registration copies strings and the field table. Internally each
registration becomes an `SuHostOpDesc` whose `run` is an adapter, so the record loader, the schedule (union table,
`su_sched_add_host_bound`), the effect analysis and the tick path are the ones built-in operators use.

Enforcement, all in `src/host/su_host_registered.c` `reg_run` (no allocation; every buffer is created with the world):

- `run` receives only its declared lanes (`SuHostOperatorRun.columns`, descriptor IDs, also for bound records) and no
  world pointer: other columns are unreachable through the API.
- Read lanes are hashed (XXH64) before and after `run`; a change fails the tick with `SU_E_UNDECLARED_WRITE` and
  `err->message` names the lane. Opt-out: `SU_HOST_OPERATOR_TRUST_INPUTS`.
- Write-only lanes are zero-filled before `run` (no undeclared read through staging), read-write lanes start from
  their input (done by `schedule.c run_host`), so every declared write is defined for every row.
- Scratch is zeroed before each call (opt-out `SU_HOST_OPERATOR_KEEP_SCRATCH`), so no state survives a call.
- Identity: `host_semantic_digest` adds `"operator":"<name>"` for named (registered) operators only; built-in digests
  are byte-identical (name NULL). The semantic hash is part of `su_host_world_identity`.
- Errors: malformed descriptor `SU_E_INVALID_ARGUMENT` (with field index), duplicate / built-in ID / reserved prefix
  (`rts.`, `space3d.`, `sutekh.`) `SU_E_DUPLICATE_KEY`, full registry (64) or no memory `SU_E_RESOURCE_LIMIT`; an
  unknown record stays `SU_E_CAPABILITY_DENIED`.

## Design decisions

- **serve/worker:** registration is in-process `host.h` only. The worker's registry (`host_registry.c`,
  `su_host_find`) is unchanged; `sutekh serve` / `world.create` refuse a registered operator's record as unknown
  (`SU_E_CAPABILITY_DENIED`). No game-specific worker build is provided. Documented in `host.h`, `docs/SPACE3D.md`
  section 1 and `docs/AGENT_RPC.md` section 18.
- **API version:** `SU_HOST_API_VERSION` stays 1, following this header's convention for additive sections ("additive,
  API version 1" on events, rollback operations, host operators, operator data, tiers). Existing structs are
  unchanged; feature detection at compile time: `#ifdef SU_HOST_MAX_REGISTERED_OPERATORS`.
- **Public run interface instead of the internal one:** `SuHostOpDesc.run` takes internal `SuWorld`/`SuInvocation`
  types that are not installed; the public `SuHostOperatorRun` exposes only declared columns, alive/generation views,
  tick, scratch and parsed data.
- **Registry:** process-wide, append-only, lock-protected (static `pthread_mutex`/SRWLOCK), no unregister: an entry
  can never disappear under a live world. Listing (`su_host_operator_count/info`) shows built-ins first, then
  registered operators in registration order.
- **Operator data:** the existing `su_host_set_operator_data` path; one parse per call, refcounted per world, released
  with the last instance. The family check now also requires the same registration (registered operators share one
  callback but not one parser).
- **Forward compatibility for issue 142 (deterministic multithreaded tick):** `struct_size` on the descriptor and on
  `SuHostOperatorRun` (append-only growth; later libraries accept earlier descriptor sizes), unknown flag bits refused,
  and `SuHostOperatorRun` already carries `row_begin`/`row_end` and `chunk_index`/`chunk_count` (always 0..rows and
  0 of 1 here). Issue 142 would add an opt-in flag; only operators that set it would be called per chunk with one
  scratch per worker. No threading is implemented here.

## Local validation

Linux 7.2.9 (Fedora 44 based), Intel Core i7-14700K (20 CPUs available), GCC 16.2.1, Clang 22.1.8, CMake 4.3.0,
Ninja 1.13.2, GNU ld 2.46.1. Renderer and glTF importer were skipped by configure (no Vulkan headers / asset deps
bootstrapped); the same holds for the baseline. Builds used `-j 6`. `SUTEKH_WERROR` is ON by default
(`-Wall -Wextra -pedantic -Werror`).

```sh
cmake -S . -B /tmp/wt-137-build/debug   -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B /tmp/wt-137-build/release -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake -S . -B /tmp/wt-137-build/asan    -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"
python3 tools/bootstrap_asmjit.py   # pinned commit 0d7f010, lock unchanged
cmake -S . -B /tmp/wt-137-build/native  -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug -DSUTEKH_NATIVE=ON
cmake --build <dir> -j 6 && (cd <dir> && ctest -j 6 --output-on-failure)
```

| Configuration | Build | ctest |
|---|---|---|
| GCC Debug | 0 warnings | **133/133 passed**, 1 skipped (`audio_device`: no audio backend on Linux) |
| GCC Release | 0 warnings | **133/133 passed**, same skip |
| GCC Debug `SUTEKH_NATIVE=ON` | 0 warnings | **150/150 passed**, 2 skipped (`native_unwind` Windows only, `audio_device`) |
| Clang Debug ASan+UBSan | 0 warnings | 21 of 133 failed, 1 skipped: **the identical set fails on the baseline** (below) |

The 8 new tests pass in every configuration, including ASan+UBSan: `host_registered` (551 checks, static),
`host_registered_dll` (547 checks; the allocation counter is static-only), `host_registered_record`, and
`host_registered_verify_{same,version,constants,name,none}`.

ASan+UBSan baseline: `origin/main` 07d06de extracted with `git archive` to `/tmp/wt-137-base-src` and built with the
same flags: 125 tests, the same 21 failures and the same skip (failure sets compared with `diff`: identical). The
sanitizer reports in both logs are the same: 4 LeakSanitizer reports, one heap-use-after-free in `su_replay_open`
(`src/host/su_replay.c:696`, error path hit by `test_replay` `test_tamper`: `su_replay_close(r)` and then a read
of `in->start_tick`), and two AddressSanitizer allocation failures (15,392,894,357,504 bytes). These failures are
pre-existing, are not touched here and were not diagnosed one by one. The 21 tests: `containment`, `control`, `host_replay`, `host_rollback_dll`, `host_rollback`, `rts_b1_hidden`, `rts_b1_install`, `rts_b2_hidden`, `rts_b2_install`, `rts_b3_hidden`, `rts_b3_install`, `rts_caps_engine`, `rts_ctrl_engine`, `rts_e2e_edges`, `rts_load_engine`, `rts_model_digest`, `rts_projectile_mutant`, `rts_replay_engine`, `rts_world_set_fields`, `rts_world_spawn_batch`, `space3d_serve`.

**No-registration guarantee.** (1) The whole existing suite passes unchanged, including pinned digests
(`example_arena_host*` `--expect-digest`, `host_ops` racer digests from `bench/space3d/expected.json`, replay
record/verify across processes). (2) An explicit cross-build comparison (scratch script, not committed) ran the same
digest-producing commands on the baseline Release build and on this branch's Release build: `arena_host --ticks 900
--rollback-check` with and without `--respond` (final digests `3b1f7e1d...` and `fab7f110...`), the SHA-256 of the
canonical replay file written by `test_replay record` (`232e8f2a5ccf4613...`), every `racer_microcosm --n 8 --ticks 720
--digest-every 120` state digest, and the `minimal_host` digest lines. Output files identical; SHA-256 of both
`02c978ede2dc7baac17b44ba151d27f979962e27e810df63907a8395191ec702`. Run on the final sources too.

**Mutation check** (scratch script, not committed; each mutant built and run, then the source restored and
`cmp`-verified): 8 engine mutants, 8 killed. Removed input re-hash (5 failures); no zero prefill of write-only lanes
(contract check); no scratch zeroing; a `malloc` in the tick (50 allocations counted); operator name not in the
semantic hash (killed only by the cross-process `verify name`, as intended); record name not checked; duplicate ID not
refused; `run->tick` off by one (440 cells differ from the model).

Repository checks: `python3 tools/validate_pack.py --check-report` passed; `python3 tools/check_repository_license.py`
passed; `git diff --check` clean.

## SHA-256 evidence (final sources)

- `include/sutekh/host.h` `75256325cdc1d930922a7a604ede84fb1660e984dbee0fb13b269aebbeaa3609`
- `src/host/su_host_registered.c` `05f48674e44968b8b3a5fe6e75a178888cb3eac96d77e5732156a7d0229b81b1`
- `src/host/su_host.c` `8508ad53b1fb1392c086e6db3ba69051742d9be8ce9da0679f0339c8b93354d9`
- `src/host/su_host_internal.h` `9b452d3eae274a5994684bc5d0bbd6e1c896ab2bb6d443cbdf22978071011f92`
- `src/host/su_host_operators.c` `4580d401a5916cb788806db01604648b0e008c14c9d590d70e082be1275af682`
- `src/runtime/rts/host_decl.c` `868cb8354ccb8174811c0e6c99116f84376bc849272fcaa56ac78a62e37fec04`
- `src/runtime/rts/host_decl.h` `d76db14df94c423c305ddf7a5d121a71571f3a9a454257f9d2c6afd7df07e3cb`
- `tests/host/test_host_registered.c` `b16ea506c82bab01db42a7d5c20856a92bfc1cca71fcecbb257249ae2b485074`
- `cmake/SutekhHostOps.cmake` `0362848ca601cc8affcc7449b8e29588a1950d40afcec1378978f6931567301a`
- Release `libsutekh_host.so` `24596b6506c5cc323447216d82159a36e655a770ba243792718e387470d5dbe8`
- Release `test_host_registered` `90f0007ae5c8d1656b37e0575c713d8384e29b361601f9d602ccd89285450cb8`

## Unrun checks and limitations

- Not run: Windows (MSVC `/W4 /WX`, clang-cl), macOS, the renderer-on configuration, ThreadSanitizer (the
  concurrent-registration test ran without it), CI. No CI result is claimed.
- No timing measured: the cost of the input re-hash (XXH64 over rows x readable lanes per tick) and of the scratch
  zeroing is not quantified; both have documented opt-outs. No performance claim is made.
- Detection limits: the input check is a 64-bit hash (a collision, about 2^-64, is missed) and covers writes through
  the views `run` receives; in-process native code can still write anywhere (C07). With `TRUST_INPUTS` an undeclared
  write is not detected (tested as such).
- The tick allocates nothing: proven by counting `malloc/calloc/realloc` (`-Wl,--wrap`) on the static Linux build
  only; the DLL and Windows builds are not counted.
- Not in scope: operator-owned variable-size state in snapshots (separate request), parallel execution (issue 142),
  unregistering, registered operators in a worker.
- `SU_E_CAPABILITY_DENIED` text for an unknown host record changed ("built into or registered with this library");
  code, index and behaviour are unchanged.

Next: push the branch, open the PR closing #137, integrate after review and CI.
