# SUTEKH — Complete engineering specification

<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->

**Licensing:** This revision is source-available under [PolyForm Perimeter License 1.0.1](LICENSE), not OSI Open Source. [NOTICE.md](NOTICE.md) governs attribution and explains historical license-decision entries; those entries do not grant a different current license. Commercial/OEM rights require a separate agreement where community terms do not cover the use.

Version 0.1 • 6 October 2026. This convenience copy combines the authoritative individual specifications. Timing values are unmeasured targets. Machine-readable schemas, examples, task DAG, checker, and specialized prompts are in the accompanying archive. Edit individual documents, then regenerate this copy. This is not an implemented engine.

## Contents
1. [Engine constitution](#section-1) — `ENGINE_CONSTITUTION.md`
2. [Architecture](#section-2) — `ARCHITECTURE.md`
3. [Live IR specification](#section-3) — `LIVE_IR_SPEC.md`
4. [Agent operating protocol](#section-4) — `AGENT_PROTOCOL.md`
5. [Milestones and first execution plan](#section-5) — `MILESTONES.md`
6. [Agent instructions — Sutekh](#section-6) — `AGENTS.md`
7. [Hot replacement, state transactions, and failure recovery](#section-7) — `docs/HOT_RELOAD_AND_STATE.md`
8. [Data, storage, and native ABI](#section-8) — `docs/DATA_ABI_AND_STORAGE.md`
9. [Determinism and BFME compatibility](#section-9) — `docs/DETERMINISM_AND_BFME.md`
10. [Build and validation policy](#section-10) — `docs/BUILD_AND_VALIDATION.md`
11. [Performance and velocity qualification](#section-11) — `docs/PERFORMANCE.md`
12. [Agent control protocol](#section-12) — `docs/AGENT_RPC.md`
13. [Backends and dependency decisions](#section-13) — `docs/BACKENDS_AND_DEPENDENCIES.md`
14. [Executable acceptance tests](#section-14) — `docs/TEST_SPEC.md`
15. [Game profiles and scope boundaries](#section-15) — `docs/GAME_PROFILES.md`
16. [Rendering, assets, and the shipping player](#section-16) — `docs/RENDERING_AND_ASSETS.md`
17. [Risk register and anti-failure policy](#section-17) — `docs/RISK_REGISTER.md`
18. [Primary sources and evidence boundaries](#section-18) — `docs/SOURCE_NOTES.md`

---

<a id="section-1"></a>

**Source document: `ENGINE_CONSTITUTION.md`**

# Engine constitution
**Normative v0.1.** MUST denotes a release or integration requirement. SHOULD denotes a default that may be overridden through a recorded, measured architecture decision. These rules supersede aspirational examples from earlier brainstorming.

## Objective and admissibility

Optimize **verified functionality throughput**, not lines of code, agent count, or the amount of assembly. Correctness, resource safety, and the selected compatibility contract are admissibility constraints. Among admissible candidates, optimize author latency, runtime tail latency, throughput, memory, energy, and distribution size according to the active workload profile. There is no single fastest implementation for every workload and machine.

An implementation is not “better” because it is lower-level. Retain a readable reference path. Accept assembly, SIMD, GPU compute, alternative data layouts, or whole-program optimization only with appropriate semantic evidence and end-to-end measurements.

## C01 — Gameplay has no native project build

A supported edit to an existing capsule MUST execute without rebuilding or relinking the resident executable. Changes to native C, assembly, platform glue, or the native backend are outside this guarantee and MUST receive targeted compilation before integration. Hiding a full build behind a watcher does not satisfy this rule.

## C02 — The live project remains available

Parsing, test execution, code generation, imports, and candidate experiments MUST NOT run as unbounded work inside the simulation tick. The previous known-good revision remains available. An ordinary valid code-only replacement MUST retain world state and the active worker PID. Native faults, incompatible core changes, and explicitly unsupported migrations may require worker replacement; never report that as same-process hot reload.

## C03 — Validation is not optional

A submitted revision is untrusted until the relevant gates pass. Agents may continue independent work while validation runs; they MUST NOT mark unchecked work as accepted. Native edits cannot be accepted solely on static inspection. Core ABI and shared contract changes require affected-dependent validation.

## C04 — Systems expose enforceable effects

Every capsule declares its stable identity, schema dependencies, field-level reads/writes, ordering edges, numeric profile, capabilities, resource bounds, and test obligations. The verifier derives effects from IR and checks them against the declaration. A `parallel: true` annotation or an agent's assurance is not evidence of race freedom.

## C05 — No escaping runtime addresses

Serialized state, capsules, RPC, replay files, and cross-capsule references MUST use stable IDs, generational handles, or validated offsets. Raw pointers are permitted only in scoped trusted native execution views and MUST NOT outlive the invocation or cross a version boundary.

## C06 — Hot replacement is transactional

Code, schedule, schema layout, and dependency generations are activated as one versioned transaction. Publish only at an allowed quiescent boundary. Retain old code until no execution can reference it. Reverting a function pointer is not state rollback; recovery requires a preserved state root or a checkpoint and event replay.

## C07 — Isolation has an explicit boundary

Unreviewed native candidates execute in disposable, least-privilege workers with bounded CPU, memory, file, and network access. Native code in a process can corrupt that process; no verifier claim changes that. An OS process separates address spaces but is not by itself a complete hostile-code sandbox. The control plane never executes uploaded native code in its own process.

## C08 — Determinism is a named, tested contract

Numeric operations, event ordering, iteration order, random state, serialization, and scheduling semantics are versioned. Cross-backend bit equality is required only for a profile that specifies and passes it. GPU approximate kernels cannot silently replace authoritative exact simulation. Replaying the engine against itself establishes regression consistency, not BFME fidelity.

## C09 — Schema changes are bounded operations

Additive changes with specified defaults are the first supported migration class. Destructive changes, relationship rewrites, and large migrations require explicit migration logic, dependency revalidation, resource admission, and rollback evidence. “All schema changes instantly hot-reload” is not a valid guarantee.

## C10 — Performance claims carry evidence

A reported result MUST identify source/artifact hashes, workload, hardware, OS/toolchain, measurement mode, sample distribution, and units. Unavailable counters are null with a reason. Proposed budgets are not measurements. Shared CI is not the authority for fine-grained hardware performance decisions.

## C11 — Allocation and side effects are controlled

Hot-path capsule allocation is forbidden in M0. Later arenas, bounded queues, and scratch storage have declared capacity and defined exhaustion behavior. Filesystem, network, audio, and other irreversible effects are not committed during speculative simulation. Effect release follows accepted tick/transaction policy.

## C12 — Assembly is available, never compulsory

Maintain a stable C ABI to native kernels. Keep baseline scalar implementations and capability-checked specialized variants. Assembly work cannot become a prerequisite for adding ordinary gameplay behavior. Raw assembly is a trusted extension lane, not an unchecked escape hatch inside sandboxed Live IR.

## C13 — Specialization follows semantics

CPU instruction dispatch, data-layout changes, batching, and GPU placement must respect numeric mode, observable order, replay identity, memory budgets, and platform capabilities. Code generation may optimize away capsule dispatch in a release bundle while preserving source-level diagnostics.

## C14 — Parallel authors do not share mutable worktrees

Use isolated worktrees or equivalent sandboxes, task-local build directories, leased write scopes, and one canonical integrator. Shared headers and schema changes have a designated owner. A patch's validation applies to its exact dependency snapshot, not an unspecified future main branch.

## C15 — Build debt is bounded

Authors use cheap relevant checks and a shared validation queue. The default cap is two pending native/core candidate batches per write scope; lower the cap when failures rise. Once the cap or a shared-interface failure is reached, repair or pick truly independent work. Never create unlimited uncompiled source on a broken foundation.

## C16 — Tooling is observable and replaceable

The engine exposes structured request/response diagnostics, dependency queries, test handles, replay differences, and performance reports. Agents and humans can use the same commands. No specific model, subscription, proprietary orchestration harness, or remote service is required to run a shipped game.

## C17 — No hidden global pipeline

Gameplay validation, asset invalidation, shader work, and incremental native builds are keyed to explicit dependencies. Cache hits are verified against full relevant fingerprints. Cold boot and offline toolchain setup are measured separately from warm iteration.

## C18 — Generality is earned

M0 is a headless integer live loop, not a renderer, full ECS, language research project, or BFME port. A new framework must unblock a current acceptance test. Preserve reusable core boundaries; keep Horde, CommandSet, and other BFME semantics in the RTS/BFME profile, not universal kernel types.

## C19 — External projects and assets are protected

Do not overwrite the user's existing projects, change their licensing, or assume authorization to publish their files. Public fixtures are synthetic or explicitly redistributable. Original BFME content stays outside the repository and public CI artifacts.

## C20 — Completion is auditable

A milestone closes only with runnable commands and stored evidence. Every handoff states implemented, tested, untested, blocked, and next work separately. No invented benchmark numbers, unsupported “all tests pass,” or unimplemented APIs presented as working features.

---

<a id="section-2"></a>

**Source document: `ARCHITECTURE.md`**

# Architecture
**Normative M0/M1 boundaries; later capabilities are explicitly staged.** Read with the constitution and specialized documents. The design is assembly-grounded, not assembly-exclusive.

## 1. Product boundary

Sutekh is a native game execution substrate plus an agent-oriented development control plane. It is not initially an all-purpose editor or an autonomous coding model. Its primary authoring object is a **system capsule**: typed behavior, effects, schema dependencies, tests, and performance metadata in a small replaceable package.

The engine must support development without a remote service. External coding agents interact through local structured APIs. Optional orchestrators distribute tasks; the engine itself does not assume infinite model access or start agents without host support and budget.

The first workload is a deterministic headless RTS-shaped simulation. The eventual core is reusable by BFME II/RotWK, Shatterfront, RED HORIZON, Slingshot, and the MMO. Game-specific concepts live in profiles. Horde-aware batching is supported through storage and scheduling primitives, not by hardcoding BFME into every game.

## 2. Process model

```text
Agent/human CLI
      │ structured local requests
      ▼
sutekh supervisor ───── artifact store + version graph + evidence index
      │                              │
      │ control only                 ├── import/build workers (later)
      │                              └── isolated candidate workers
      ▼
active simulation worker
  schema store → stable schedule → VM / admitted native capsules
      │
      └── committed snapshots + staged effects → renderer/player (later)
```

Use one small native executable with `serve` and `worker` roles initially rather than inventing a service platform. A Python standard-library CLI/build helper is permitted outside the tick. The supervisor owns control, not simulation pointers. Workers receive immutable manifests and explicit state inputs.

A normal code edit is installed **inside the existing active simulation worker** at a safe boundary; the supervisor does not respawn the world to fake hot reload. A candidate evaluation uses a separate disposable worker. A native crash is recovered by replacing the failed worker from a committed checkpoint/log, not by continuing inside possibly corrupted memory.

In M0 there are no irreversible external effects. In later development modes effects are staged until a tick is accepted. The production player may run a faster trusted mode with weaker failure recovery, but it must identify that mode and never claim untrusted native fault containment.

## 3. Technology decisions

| Area | Initial decision | Reason and boundary |
|---|---|---|
| Resident code | C17, small translation units | Explicit ABI; targeted native checks; no mandatory Rust build graph |
| Assembly | Optional x86-64 kernels, separate sources | Measured low-level escape hatch; scalar fallback remains |
| Build graph | CMake + Ninja, configure once per toolchain | Reuse dependency tracking; no custom general build system |
| Linux compiler/linker | Clang + LLD; measured mold substitution allowed | Pin versions; no timing claim imported from another project |
| Windows | Clang/clang-cl + lld-link and installed Windows SDK | Native Windows tests required; no Linux calling-convention assumptions |
| Metadata and M0 IR | Strict JSON; vendored yyjson in native code | Stable machine output; no custom syntax frontend yet |
| Control transport | Length-bounded JSON lines over local stdio | No public listener required; binary state is out-of-band |
| Gameplay execution | Checked scalar VM first | Executable semantics before native backend complexity |
| M1 native emitter | Isolated AsmJit adapter behind a C ABI | Reuse an encoder; the adapter's C++ dependency stays out of core headers |
| M1 SIMD | Optional AVX2 after scalar equivalence | Capability and OS-state checks; no AVX-512 requirement |
| Later rendering | Vulkan profile, capability-negotiated | No renderer dependency in the simulation or headless test host |

AsmJit provides machine-code generation and optional register allocation; it does not prove our IR semantics or produce a game engine [S01]. Cranelift remains an optional later reference/optimizing backend behind the same contract [S02]. We do not implement two native backends for M1. LLD supports the relevant object formats; exact native flags and SDK availability are bootstrap observations [S07].

## 4. Execution tiers

**T0: reference VM.** Parse JSON into a validated, immutable instruction sequence. Execute a bounded current-row loop with checked field access. This is the authoritative semantic reference for M0. A tiny independent Python evaluator in the specification tests checks examples only; it is not the engine runtime.

**T0b: batch VM, after profiling.** Amortize decode/dispatch across spans, fuse safe simple operations, or call reviewed kernels. Preserve the same reference semantics and diagnostics. Do not call this native specialization merely because it invokes a precompiled helper.

**T1: native scalar / AVX2.** A separate backend work queue lowers individual capsules. A result includes numeric profile, ABI, field binding/layout generations, target capability mask, source map, and content fingerprint. Differential tests precede admission. Publication occurs only at a safe boundary. Old code remains active while specialization is queued.

**T2: offline optimized bundles.** Fuse compatible systems, tune layouts, do profile-guided optimization, and prepare platform builds. These are release/batch jobs. Shipping code is identified by artifact hashes and can omit JIT entirely. The development lane must not wait for release optimization.

**GPU:** a separate execution family for explicitly eligible work. GPU placement needs total cost including transfer, synchronization, dispatch, and queue contention. Approximate cosmetic work can use relaxed arithmetic; authoritative deterministic work cannot silently move to a numerically different backend. Automatic CPU/GPU crossover selection is a later measured feature, not M0 infrastructure.

## 5. Version model

The following identities are different and must not be conflated:

- `source_revision`: author-visible behavior revision of one capsule.
- `semantic_hash`: normalized IR + effects + numeric semantics + referenced definitions.
- `layout_generation`: physical schema binding generation.
- `artifact_hash`: backend/toolchain/ISA-specific compiled artifact.
- `world_revision`: installed manifest/schedule/schema set and accepted state lineage.

A native optimization may change artifact hash without changing semantic hash. A gameplay rebalance changes semantic hash and therefore is not judged by old/new exact behavior equality. A schema migration changes world/layout generations and invalidates bound kernels. Replays carry all semantic revisions and their switch ticks.

## 6. Data ownership and scheduling

Use a deterministic, stable-row SoA table for M0. Entity IDs are stable, rows do not reorder, schemas do not mutate, and entity creation/destruction is deferred to a later milestone. That restriction eliminates unnecessary ECS complexity from the first proof.

Later storage uses chunked SoA tables, generational entity IDs, explicit relationship tables, and schema-mediated field access. Row address is not entity identity. Layout changes are build/migration transactions, not spontaneous runtime reshuffles.

The scheduler builds a graph from validated effects and explicit order edges. M0 is serial. M1 parallelizes disjoint chunks while preserving stable merge order. Structural changes and cross-entity effects are emitted into bounded per-chunk queues and committed in a canonical order. Implicit cross-capsule calls and hidden globals are forbidden.

## 7. Runtime modes

| Mode | Purpose | Safety/performance policy |
|---|---|---|
| `dev_checked` | Default interactive development | VM checks, isolated candidate testing, recoverable checkpoints; overhead is measured |
| `dev_native` | Validated native preview | Same version protocol; native worker faults still require replacement |
| `perf_trusted` | Controlled performance measurement | Explicit instrumentation and recovery settings; do not compare against another mode silently |
| `release` | Shipped player/server | No agent API or watcher; pinned content; optional AOT; production authority policy |

The no-build authoring loop is a property of development. A release artifact may be statically optimized and is not required to preserve every development indirection.

## 8. Backpressure and scheduling priorities

Keep simulation deadlines, user input, and essential validation ahead of speculative native optimization. Coalesce superseded capsule edits. Use a bounded priority queue with durable task status. Cancel outdated candidates by content hash. Under overload keep known-good code running and report queue latency instead of silently accumulating unbounded jobs.

Agent count is limited by independent ready tasks, write scopes, context/model limits, memory, and verifier capacity. M0 starts with a small number of independent workers. Increasing to 32 or 64 agents is an experiment after the acceptance pipeline works, never a first-milestone requirement.

## 9. Recovery and migration

Code-only swaps require matching schema/effects and a new validated schedule if ordering changed. Schema additions are introduced only in M2, using copy-to-new columns, defaults, revalidation, a safe commit, and rollback capacity. Destructive migrations require an explicit function and fixture tests. Large migrations may remain pending or require a controlled checkpoint replacement.

A snapshot clone is initially an ordinary bounded deep copy. Copy-on-write and page-level snapshots are optional measured upgrades. No claim of sub-millisecond cloning for arbitrarily large worlds is made.

## 10. Repository topology

```text
include/sutekh/       frozen C ABI headers (single contract owner)
src/platform/        OS memory, process, clock, worker control
src/ir/              decoder, verifier, normalized instruction form
src/vm/              reference execution, later batch execution
src/world/           columns, snapshots, handles, migration
src/runtime/         schedule, safe points, registry, version retirement
src/agent/           request handling, diagnostics, artifact/evidence index
backends/native/     optional C++ adapter and scalar/vector lowering
kernels/x86_64/      reviewed assembly with reference tests
profiles/rts/        general RTS systems, not original BFME assets
profiles/bfme/       compatibility adapter and evidence registry
render/              separate renderer, introduced after headless gate
capsules/            gameplay IR, contracts, tests, benchmarks
bench/               fixed, versioned representative workloads
tests/               core, integration, negative and differential tests
tools/               build broker, CLI, pack checker, offline helpers
planning/            task DAG, leases, handoff/status manifests
```

Do not create empty subsystem forests in the first implementation. Materialize a directory when its first real task starts.

## 11. Explicitly not promised

The architecture does not promise assembly outperforms optimizing compilers, instantaneous GPU pipelines, arbitrary crash-safe in-process assembly, exact BFME fidelity from synthetic tests, unlimited agent scaling, or one universal numeric model for every game. It creates observable, testable paths toward the user's actual goals.

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md). Details: [state/reload](docs/HOT_RELOAD_AND_STATE.md), [storage/ABI](docs/DATA_ABI_AND_STORAGE.md), [backends](docs/BACKENDS_AND_DEPENDENCIES.md).

---

<a id="section-3"></a>

**Source document: `LIVE_IR_SPEC.md`**

# Live IR specification
## SU-LIR 0.1 — narrow, executable M0 contract

The initial source format is strict UTF-8 JSON with a `.lir.json` suffix. A human-oriented `.live` language may later lower to the same semantic representation. It is not part of M0. Agents can author JSON immediately, without waiting for a new language toolchain.

## 1. Scope

SU-LIR 0.1 describes a straight-line scalar computation executed once for each row in a fixed table. It supports **u32 fields**, single-assignment virtual registers, constants, field loads/stores, modular arithmetic, comparisons, and selection. It has no user-defined loops, recursion, dynamic allocation, cross-entity addressing, native calls, floating point, strings, file access, or networking.

The row loop is owned by the runtime. A zero-row table executes no body instructions. Iteration order is ascending logical row index. Every register is per-row and freshly defined each invocation; no value survives between rows or ticks except an explicit field store.

This intentionally tiny language proves hot replacement. Navigation, full combat logic, and BFME semantics require later, versioned extensions; agents must not fake those features with undeclared host behavior.

## 2. Module structure

```json
{
  "ir_version": "0.1",
  "system_id": "demo.movement",
  "numeric_profile": "u32_mod_v1",
  "query": "demo.actor",
  "instructions": [
    {"op": "load_u32", "dst": "p", "field": "position_x"},
    {"op": "load_u32", "dst": "v", "field": "velocity_x"},
    {"op": "add_u32", "dst": "next", "a": "p", "b": "v"},
    {"op": "store_u32", "field": "position_x", "src": "next"}
  ]
}
```

The capsule contract is separate and binds `position_x` and `velocity_x` to stable schema field IDs/types. The IR and contract must name the same system/query/numeric profile. The schema file under `schemas/` validates syntax; **semantic verification remains required**.

## 3. Types and arithmetic

`u32` is the mathematical set 0 through 4,294,967,295. `bool` is exactly false or true and cannot be implicitly coerced to u32. JSON integer constants must be within the u32 range. Negative constants and fractional JSON numbers are rejected. The loader must not parse these integers through an imprecise floating representation.

`add_u32(a,b)` and `mul_u32(a,b)` return the result modulo 2^32. `sub_u32(a,b)` is also modulo 2^32, not saturating. All three are total functions. Later saturating arithmetic must have distinct opcode names. Native/C implementations must use well-defined unsigned behavior; they may not rely on signed overflow.

`lt_u32` is an unsigned comparison. `eq_u32` is exact equality. `select_u32` chooses an already-computed u32 register based on a bool register. There is no short-circuit or unexecuted branch in this version. This makes boundedness obvious without a general control-flow verifier.

## 4. Opcode table

| Opcode | Arguments | Result/effect |
|---|---|---|
| `const_u32` | `dst`, `value` | Define a u32 register |
| `load_u32` | `dst`, `field` | Read current row's named u32 field |
| `add_u32` | `dst`, `a`, `b` | u32 modular sum |
| `sub_u32` | `dst`, `a`, `b` | u32 modular difference |
| `mul_u32` | `dst`, `a`, `b` | u32 modular product |
| `eq_u32` | `dst`, `a`, `b` | Define bool equality result |
| `lt_u32` | `dst`, `a`, `b` | Define bool unsigned comparison |
| `select_u32` | `dst`, `cond`, `on_true`, `on_false` | Define selected u32 |
| `store_u32` | `field`, `src` | Stage u32 write to current row |

Unknown opcodes, unknown keys, duplicate object keys, undefined registers, duplicate register definitions, type mismatches, and undeclared field effects are errors. Reject ambiguity rather than guessing an author's intent.

## 5. Read/write semantics

Each system invocation sees the state committed by all prior systems in the schedule. Within that invocation, **loads read the invocation-entry value**, not a previous staged store. At most one store per field per row is allowed by 0.1. This avoids hidden within-row ordering dependencies and makes reference/vector behavior easier to compare.

The verifier requires every declared writable field to be stored exactly once in the instruction body. Read permissions permit zero or more loads; write permission does not imply read permission. Unchanged fields are not copied through registers unnecessarily. Commit staged writes only after the system completes successfully. A detected execution failure must not leave partially committed system output. Later native fault recovery follows the worker checkpoint protocol rather than assuming a C return code always exists.

The M0 implementation may allocate staging columns during world preparation or capsule admission, never per entity or unexpectedly mid-tick. Resource exhaustion rejects admission or aborts a candidate tick before publication.

## 6. Contract and binding

The included contract format contains an array of fields with stable numeric IDs, string names, types, and read/write permissions. Declaration order is not field identity. The prepared execution plan resolves these names once into validated bindings and records a layout generation.

Bindings are only valid for the exact query/table schema and layout generation used during verification. Schema mismatch returns `SU_E_SCHEMA_MISMATCH`; it does not look up a similarly named field or fall back to a raw offset.

M0 contains one table with fixed rows and fields. A later schema change is a transaction that regenerates bindings and invalidates native variants referencing old layouts.

## 7. Resource limits

Initial, configurable defensive limits: 256 KiB IR source, JSON nesting depth 32, 4,096 instructions, 4,096 virtual registers, 256 bound fields, and 1,000,000 rows for the supplied stress harness. These are acceptance/configuration values, not fundamental engine limits. Caps must be checked before proportional allocation and before multiplication of sizes. Oversized input yields a structured resource error.

Execution work is bounded by row count × instruction count. The supervisor also applies a wall-time watchdog to candidate workers. A wall-clock timeout is a tool failure and must never alter authoritative deterministic simulation state or select a gameplay outcome.

## 8. Diagnostics

Errors include `code`, source path, JSON pointer, optional byte span, system ID, relevant field/register, expected/actual values, and one concrete correction hint. Example:

```json
{
  "code": "SU_E_UNDECLARED_WRITE",
  "path": "capsules/movement/system.lir.json",
  "json_pointer": "/instructions/3/field",
  "system_id": "demo.movement",
  "message": "position_x is stored but not declared writable"
}
```

The exact failing source content hash belongs in the surrounding response/evidence record so stale diagnostics cannot attach to a newer edit.

## 9. Canonical semantics and hashes

Original source bytes are retained for diagnostics and source hashing. A semantic hash is computed from a normalized representation: fixed field order for module metadata, instruction order preserved, stable field IDs, canonical decimal integer encodings, opcode/type names, validated effects, numeric profile, and referenced definition hashes. Register names are normalized by definition order. Whitespace and JSON key order alone must not alter the semantic hash.

The hashing algorithm and canonical byte encoding must be pinned before first persisted artifacts. M0 uses SHA-256 for source/artifact identities through a reviewed implementation; a change to normalization or hash format increments the artifact-format version. Hash equality is an indexing mechanism, not a mathematical proof of program equivalence.

## 10. Lowering contract

The native backend must produce the reference result for every input within the supported profile and resource bounds. Arithmetic may be vectorized because 0.1 has no cross-row effects, provided row tails and unaligned data are handled correctly. Zero rows, counts not divisible by SIMD width, maximum u32 values, alias rejection, and generation mismatch are mandatory tests.

No FMA or floating-point semantic question exists in 0.1. Later floating-point support must explicitly define operation rounding, contraction, subnormals, NaNs, signed zero, conversions, and reduction order. A label such as `strict` by itself is insufficient [S06].

## 11. Versioned extension sequence

After M0, add only operations required by an accepted task: bounded conditional blocks, i32 arithmetic with explicit overflow behavior, typed entity handles, bounded event emission, tick/context inputs, deterministic PRNG state, relationship reads, and separately specified floating-point modes. Every addition requires verifier rules, reference implementation, invalid-input tests, cross-backend differential tests, and resource bounds.

Control-flow graphs, a broad optimizing SSA compiler, automatic layout exploration, and CPU/GPU multi-target synthesis are later capabilities. They are not prerequisites for the first executable capsule.

Fixtures: [movement IR](examples/movement.lir.json), [contract](examples/movement.capsule.json), [world](examples/world.json), [test](examples/movement.test.json). Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-4"></a>

**Source document: `AGENT_PROTOCOL.md`**

# Agent operating protocol
## Goal: continuous useful work with bounded verification debt

This protocol is harness-independent. It can be used by a single coding agent, a local orchestrator with subagents, or CI-backed teams. Do not claim to spawn agents or run jobs when the environment lacks that capability. A single agent executes ready tasks serially under the same contracts.

## 1. Roles

The **orchestrator** owns the task DAG, architecture decisions, work leases, resource admission, and current milestone. The **implementation worker** owns one bounded write scope and its patch. The **verifier** independently checks the exact candidate and records evidence. The **integrator** is the sole writer of the canonical branch and verifies that the candidate's dependency assumptions still hold. A **build broker** runs deduplicated, resource-limited native checks and posts durable results.

One model may perform multiple roles sequentially when resources are limited, but implementation and acceptance must remain distinct operations. Higher agent count is not intrinsically better.

## 2. Task packets

Every task packet specifies ID, goal, base snapshot, dependencies, write/read scopes, interface assumptions, required tests, change class, resource budget, and acceptance evidence. The packet identifies work that can proceed immediately versus work contingent on another interface landing.

Tasks are small coherent capabilities, not arbitrary file counts. Do not divide a correctness invariant between agents without a shared contract owner. Contract owners may publish a versioned draft for parallel implementation, but changes against it cannot integrate until the contract and dependencies are accepted.

## 3. Workspace isolation

Use one worktree per implementation worker and one separate canonical integration worktree. A lease records owner, paths, base revision, expiry/heartbeat, and task ID. Shared headers, schema formats, build files, and global schedule changes have explicit ownership. Leases prevent concurrent writes; they do not prove semantic independence.

Workers never reset, clean, stash, or overwrite another worker's changes. Stale worktrees are inspected before recovery. Do not force-push, delete branches, or publish a new remote unless the owner has authorized that specific operation.

## 4. Work states

```text
ready → leased → implementing → submitted → checking
                                       ↘ blocked/rejected
checking → verified → integration_check → integrated
                     ↘ stale/rejected
```

Runtime installation is a separate state machine: `received → prepared → evaluated → admitted → installed`, with explicit rejection/retirement states. Git integration does not automatically imply a live runtime switch, and a local preview does not imply a source commit is accepted.

Store state durably in machine-readable records. Every failure names its exact input snapshot. `not_run`, `running`, `passed`, `failed`, `skipped`, and `unavailable` are distinct statuses.

## 5. Continuous author loop

Read only the task packet, applicable AGENTS instructions, relevant contracts, and dependency context. Implement a coherent patch. Run cheap checks appropriate to the change. Submit the immutable candidate to the relevant validation lane. While the job runs, take the next independent task, author its tests, investigate a ready failure, or improve a measured bottleneck.

Do not sit in repeated `sleep`, full-build loops, or log polling when useful independent work exists. Do not invent work merely to appear busy. When every remaining task depends on a failing gate, repair the gate.

## 6. What must be checked locally

| Change class | Minimum author check | Acceptance lane |
|---|---|---|
| Gameplay IR/contract | Parse, schema/effect/type verification, small fixtures | Isolated runtime tests; affected replay/invariant suite |
| Native C/ASM | Targeted compile or assemble of affected targets and focused smoke test | Relevant dependent tests, instrumentation lane, platform/ABI gates |
| Shared ABI/schema | Updated fixtures and compatibility check | All affected consumers plus migration/reload tests |
| Docs/planning | Links, JSON, task DAG consistency | Independent scope/consistency review |
| Backend/compiler | Changed-target compile and opcode tests | Reference differential corpus, negative/fuzz tests, ABI guards |

A native check that exceeds the interactive budget moves into the broker; it is not skipped. The worker may continue only within the verification-debt cap and declared dependency constraints.

## 7. Verification debt and resources

Start with one integrator/verifier, one contract owner where necessary, and two to four nonconflicting implementation tasks when the harness permits. Add workers only when the ready queue, memory headroom, and verifier throughput justify it. Reserve resources for a live worker and verification; do not run one full-core compiler per agent.

Default cap: at most two pending native/core batches per write scope; a public ABI change permits only one pending generation until consumers validate. Reduce the cap after repeated failures. Keep queue length, p95 author wait, failure/rework rate, and memory pressure visible.

Parallel candidate performance comparisons are allowed for correctness screening. Final performance ranking runs serially or on isolated equivalent machines; contention invalidates fine-grained comparisons.

## 8. Evidence and acceptance

A result contains candidate/source tree hash, base/dependency hashes, test corpus hash, toolchain identity, commands, exit codes, timestamps/durations, runtime mode, logs/artifact paths, and explicit result status. Hardware metrics are optional and null when unavailable.

Optimization-only changes require unchanged approved semantics; gameplay changes require updated, independently justified expectations. An agent may not both redefine a golden result to match its code and call the result proof of correctness. Test modifications and budget relaxations receive explicit verifier review.

Reject or quarantine a candidate that removes assertions, suppresses failures, narrows benchmarks, skips tails, changes seeds, or changes correctness semantics merely to look faster. Benchmark selection is owned by the verifier/performance protocol, not the candidate author.

## 9. Integration protocol

The integrator checks the patch's write scope and base snapshot. If main has changed relevant dependencies, rebase/reapply and rerun affected gates. Previously passing reports do not transfer across arbitrary edits. Apply a coherent batch, run an integration smoke gate, and record the accepted tree hash.

Only the canonical integrator updates main. Remote publishing follows the engine repository's configured policy; the direct-main preference from a different BFME accelerator project is not automatically inherited here. Maintain a green accepted branch and separate pending candidates instead of blocking all work on one branch.

## 10. Continuation and interruption

At each coherent checkpoint record the accepted revision, current milestone, exact task states, leases, pending job IDs, failures, commands that worked, untested assumptions, and next ready tasks. A continuation agent inspects actual source and evidence before trusting summaries. A response ending or a model session closing is not an autonomous background worker.

Resume by reconciling durable job records and the current tree. Never start duplicate compilation for an already-running matching fingerprint. Expired leases require checking the worktree and owner process before reassignment.

## 11. Context discipline

`AGENTS.md` is short and stable. Each subsystem has a compact contract, ownership map, diagnostic codes, tests, and one maintained implementation note. Avoid repeatedly feeding a whole architecture document to each specialist. Generated source maps, dependency queries, and local contracts should replace repository-wide guesswork.

## 12. Reporting

A handoff must say what changed, which evidence passed, which checks were not run and why, what remains blocked, and the next executable action. Report accepted useful changes per elapsed time, agent-seconds, and human interventions separately. Do not combine incompatible units into an unexplained single “VFT score.”

Schemas and tasks: [task schema](schemas/task.schema.json), [planning/tasks.json](planning/tasks.json). Detailed build policy: [BUILD_AND_VALIDATION.md](docs/BUILD_AND_VALIDATION.md).

---

<a id="section-5"></a>

**Source document: `MILESTONES.md`**

# Milestones and first execution plan

These are evidence gates, not calendar promises. The long-term design may be broad; the initial critical path is intentionally narrow. Only M0 is authorized as the first implementation tranche by the bootstrap prompt. Later gates unlock successive scopes.

## M0 — Zero-build gameplay loop

**Deliverable:** one small C17 executable running a headless fixed-schema u32 world; JSON capsule loading/verification; checked VM; structured local control; isolated candidate evaluation; same-worker code replacement; snapshot restore; honest timing reports.

Implement the vertical slice in this order: freeze minimal ABI/IR/error contracts; build native skeleton; implement parser/verifier and fixed columns; implement VM and a tiny fixture; add worker/control lifecycle; add versioned safe-point installation and snapshot handling; demonstrate reload/failure behavior; measure the loop. Build the control plane as a thin implementation, not a separate service platform.

M0 begins at 1,024 rows. Use small edge fixtures for exact expected outcomes. The million-row workload is a stress experiment after the loop works, not an excuse to postpone it.

**Closure evidence:** T00–T15 from `docs/TEST_SPEC.md`, with genuine results and explicit timing target comparison; an instrumented capsule edit that invokes no native compiler/assembler/linker; preserved PIDs and state for code-only swaps; rejection without active-state change; stored commands for a new developer to reproduce the demonstration.

**Not M0:** JIT, SIMD backend, custom language frontend, dynamic entities/schemas, advanced ECS, pathfinding, networking, Vulkan, audio, editor, auto-tuning, distributed agent platform, or BFME asset import.

## M1 — Native acceleration and reliable parallel development

**Deliverable:** one optional AsmJit-backed native adapter, scalar lowering, later AVX2, exact differential testing, safe native code lifetime, code-generation cache, and bounded validation/build queues. Add disjoint chunk scheduling only after effects tests exist. Smoke-test Windows ABI/platform paths; keep Linux as the main development target.

Native compilation remains optional for immediate gameplay preview. Old admitted code stays live while native specialization runs. The reference VM remains available and is never removed to hide a backend mismatch.

**Closure evidence:** opcode/tail/alignment/ABI differential corpus, capability fallback, stale artifact rejection, safe retirement, worker-fault recovery, real native build latency measurements, and a small multi-agent experiment with independent work scopes and measured rejection/rework. Start small; do not require 64 agents.

## M2 — Real state evolution

**Deliverable:** generational entities, bounded structural/event queues, chunked schema storage, additive migrations, explicit destructive migration path, stronger snapshot/replay tooling, and the next narrowly needed IR operations. Define numeric profiles before adding authoritative floating point.

**Closure evidence:** entity churn, stale handle rejection, migration failure/rollback, layout invalidation, stable event ordering under multiple worker counts, and replay comparison across permitted backends. Large migration costs are reported separately from code reload.

## M3 — RTS microcosm

**Deliverable:** synthetic hordes/actors, formation membership, movement, targeting, weapons/projectiles, damage/death, and a spatial workload. No original assets required. Use representative mixed and congested scenes, not only arithmetic loops.

A surface language may be introduced here only if it clearly improves authoring and lowers to the existing verified IR. It must not become a prerequisite for the RTS profile. Improved navigation is a synthetic-profile design choice, not evidence of BFME compatibility.

**Closure evidence:** deterministic replay, useful agent-authored behaviors, end-to-end tick costs, stress curves, memory accounting, and independently reviewed performance optimizations. Record actual limits; do not predeclare a particular huge unit count as solved.

## M4 — BFME evidence slice and multiplayer prototype

**Deliverable:** version-labelled BFME behavior/evidence registry, local-data importer boundary, a small compatible horde/combat slice, command-stream replay, and headless multi-peer experiments. Select a documented game variant per fixture family. Keep original files out of the public repository.

**Closure evidence:** confirmed behavior fixtures for the selected slice, clear unknowns, source/data identity, regression versus original-compatibility results separated, and deterministic peers under controlled network faults. No claim of a completed BFME remake.

## M5 — Presentation and game production path

**Deliverable:** minimal Vulkan presentation, then measured instancing/culling/animation/effects, granular assets, asynchronous shader/pipeline preparation, audio event consumption, and a stripped release player. Extend the game profile based on validated needs.

**Closure evidence:** real frame captures/timings, device capability handling, no synchronous unexpected import/build work in frame-critical paths, safe resource retirement, reproducible packaged demo, and explicit startup/runtime/memory costs.

## First orchestration wave

The task DAG in `planning/tasks.json` is authoritative for dependencies. Initially one contract owner freezes M0-00. After that, run native skeleton/platform work, independent fixture/test work where contracts suffice, and parser/storage work as dependencies become ready. One verifier starts early with negative fixtures; it must not wait until thousands of source lines exist.

A typical first small team has an orchestrator/integrator, parser/VM worker, state/runtime worker, and test/tooling worker. Role count is not a required number of concurrent model sessions. A single capable agent can execute the same DAG.

## Stop expanding, keep shipping

A working small vertical slice outranks skeletons for twenty future systems. Freeze each interface only as far as its consumers need. Once M0 passes, improve the most consequential measured bottleneck and begin the next gate. Do not rewrite working architecture because a more elaborate hypothetical one sounds faster.

An owner decision on remote naming/license does not block local M0. Missing required native tools do block claiming M0 completion. Report the distinction accurately.

---

<a id="section-6"></a>

**Source document: `AGENTS.md`**

# Agent instructions — Sutekh

Build the engine described by the root specifications. Start with the **M0 headless no-build gameplay loop**. Do not restart architecture brainstorming, migrate another game repository, or build a full editor/compiler first.

Read `ENGINE_CONSTITUTION.md`, your task packet, and the relevant subsystem contract. The orchestrator additionally reads `ARCHITECTURE.md`, `LIVE_IR_SPEC.md`, `AGENT_PROTOCOL.md`, `MILESTONES.md`, and `planning/tasks.json`.

Gameplay IR edits must not require a native project build. Native C/ASM edits still require targeted compilation and relevant tests before acceptance. Use the broker for longer validation and continue independent ready work; never interpret “no waiting” as permission to claim unchecked source works.

Use isolated worktrees and leased scopes. Do not modify shared contracts without their owner's approval. One integrator owns canonical branch writes. Do not overwrite existing user projects, force-push, delete work, choose a project license, publish assets, or provision paid resources without authorization.

Implement real vertical slices. No fake backends, stubbed success responses, fabricated benchmarks, or unconditional `PASS`. All timing budgets in these specs are targets. Retain readable reference behavior. In-process native faults require checkpoint recovery in a replacement worker, not merely a pointer rollback.

M0 uses C17, strict JSON IR, u32 arithmetic, fixed columns, a checked VM, and a local structured control plane. Native generation, dynamic schemas, broader language features, full RTS, networking, and rendering follow the milestone gates. Assembly remains an optional measured kernel lane.

After each coherent patch, record exact modified paths, test commands and results, unrun checks, source/artifact hashes, blockers, and next work. Update durable task state. Keep useful independent work moving while bounded validation jobs run. Once verification debt or interface risk exceeds the protocol cap, repair instead of accumulating speculative code.

Commands mentioned in specification prose are desired interfaces until implemented. Verify their existence; do not report them as run by copying sample output.

---

<a id="section-7"></a>

**Source document: `docs/HOT_RELOAD_AND_STATE.md`**

# Hot replacement, state transactions, and failure recovery

## 1. Guarantees by change class

| Change | Supported first | Required behavior |
|---|---|---|
| Code body; identical schema | M0 | Same active worker PID; preserve state; activate at tick boundary |
| Invalid source/effects | M0 | Reject; no active code/state change |
| Restore an explicit snapshot | M0 | Restore exact schema, semantics, tick, and state lineage |
| Native implementation of identical IR | M1 | Admit after differential tests; retain old code until quiescent |
| Add field with default | M2 | Preallocate, migrate, rebind, commit atomically |
| Remove/retype field or relationship rewrite | M2+ | Explicit migration; dependency checks; reject unsupported cases |
| Resident core ABI change | Any stage | Targeted rebuild and controlled worker replacement may be necessary |
| Native crash | M1+ | Replace worker; restore checkpoint; replay committed inputs |

The entire game does not magically remain uninterrupted after arbitrary native corruption. Ordinary supported code replacement does not require a process restart; fault recovery is a different operation.

## 2. Candidate lifecycle

```text
RECEIVED
  → SOURCE_VALIDATED
  → EFFECTS_VERIFIED
  → PREPARED
  → PREVIEW_TESTED
  → ADMITTED
  → PENDING_BOUNDARY
  → INSTALLED
  → RETIRED
```

Any pre-install stage can end in `REJECTED`, `STALE`, or `CANCELLED`. An installed revision can be superseded or involved in recovery. A revision receives a unique transaction ID and immutable source/dependency fingerprints. A later save does not mutate an existing candidate in place.

A file watcher is only a convenience around `capsule.submit`. It must handle partial writes through a stable-byte capture or atomic manifest publication, coalesce bursts, and discard superseded preparation. Explicit submission is the authoritative test path and avoids file-system notification timing ambiguity.

## 3. Code-only installation

Prepare the new IR, effects, field bindings, scratch capacity, and replacement schedule outside the tick-critical section. Recheck expected active revision and layout generation before installation. At a boundary, stop dispatching the old schedule, finish already-dispatched jobs, atomically publish the complete new manifest/schedule generation, and resume.

M0 has a serial scheduler; this is an inexpensive safe point, not a generalized lock-free publication mechanism. M1 can use generation/epoch retirement once parallel jobs exist. No worker may observe half old and half new dependencies inside one semantic tick.

A behavior-changing swap is recorded in the replay log with an exact effective tick and semantic hash. A native-only optimization retaining semantic hash is an artifact change, but still requires valid bindings and numeric equivalence.

## 4. Committed versus working state

The **committed root** is the last accepted full simulation tick. A **working root** is the state being computed for the next tick. Candidate previews operate on independent roots. A system's staged writes become visible to subsequent systems in that working tick only after the system succeeds. External readers see only committed roots.

For M0, choose the simplest auditable implementation: copy committed columns into a reusable working buffer at tick start; preallocate per-system output scratch; publish a new committed root only after the tick succeeds. This has measurable copying overhead and is not the release-performance design. Optimize it into dirty chunks, undo logs, or page-level copy-on-write only after tests demonstrate equivalent behavior.

M0 has no external effects, so discarding a failed working root is enough for logical rollback. If the worker process dies, in-memory roots die with it: recover from a supervisor-held checkpoint/log, not from a pointer the process can no longer access.

## 5. Checkpoints and logs

A checkpoint contains format version, world revision, schema definitions and generations, ordered entities, canonical field values, tick, command queues, PRNG state when introduced, semantic manifest, and effect commit index. Exclude addresses, padding bytes, file handles, GPU handles, and wall-clock state.

The supervisor stores an immutable checkpoint plus accepted input and semantic-revision events after that checkpoint. Checkpoint materialization and retention are resource-bounded. Recovery recreates the worker with the checkpoint's manifest, replays inputs under the recorded semantic revisions, then resumes at the last accepted boundary. Repeated failure at a tick quarantines the offending revision instead of looping forever.

Do not claim constant-time cloning. M0 snapshots are bounded deep copies. Large-world costs are measured as bytes copied, memory overhead, and time, not hidden inside “world fork” latency.

## 6. State migration

An additive field migration allocates the new column, writes defaults, validates values, prepares new bindings/artifacts, and publishes a new schema/world generation. Existing fields keep stable IDs. A rename preserves field ID and changes only naming metadata. Removing a field that any admitted capsule still reads is rejected.

Destructive migrations declare source and destination schema versions, pure transform logic, bounds, resource reservation, and recovery policy. A noninvertible change cannot be rolled back by running a guessed inverse; retain the old checkpoint/root until acceptance and retention policy allow disposal.

Large migrations are not required to fit the small code-reload budget. The default is to leave the old world active, prepare the new representation independently where possible, and perform a controlled commit only when consistent. Background migration must reconcile intervening writes explicitly. Without that machinery, pause at a checkpoint and report the operation as a controlled migration, not zero-latency reload.

## 7. Native code lifetime

Generation-tag every entry point and binding. Never reclaim a page while an invocation, queued job, stack frame, callback, or deferred task may use it. Track retiring generations and wait for a quiescent epoch. Cranelift's API similarly requires no executing functions or live callable pointers before releasing its code memory [S02].

Prepare code in writable nonexecutable pages, finish relocations, transition to executable nonwritable permissions, handle platform instruction-cache requirements, then publish. Windows explicitly requires the application to establish instruction-cache coherency for generated executable regions [S03]. Failure at any stage rejects the artifact; do not leave RWX memory as a fallback.

## 8. Multiplayer policy

Live code editing is development-only by default. A lockstep session pins the semantic manifest and content set. Changing semantics requires a coordinated development-session barrier with all peers acknowledging the same version, or a new session. Native artifacts may differ by hardware only after profile-specific equivalence testing; semantic identity remains the same.

Do not install arbitrary local balance changes in a multiplayer match. Network packets and replays carry version identity so divergence due to code mismatch is diagnosed separately from simulation defects.

## 9. Required failure tests

Reject parse/type/effect errors, stale expected revision, stale schema generation, unavailable memory, preparation cancellation, and unsupported migration without touching active state. Kill a candidate worker and confirm the active worker and supervisor continue. Later kill a native active worker and verify checkpoint/log recovery, explicitly recording its changed PID.

Test repeated code revisions and retirement for memory leaks. Test a failed second system after a successful first system to ensure the published tick root remains unchanged. Test a schema change while an old native generation still has in-flight work; reclamation must wait.

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-8"></a>

**Source document: `docs/DATA_ABI_AND_STORAGE.md`**

# Data, storage, and native ABI

## 1. Identity is not address

M0 uses fixed rows and stable fixture entity IDs. Later entity identity is a generational handle with explicitly sized index and generation fields; zero is reserved as invalid. Exhausted generations are retired rather than silently wrapped into a potentially live stale ID. Physical rows, chunk indices, addresses, and GPU instance indices are not entity identity.

Schema and field IDs remain stable across names and layouts. A field's type, default, numeric interpretation, and persistence role belong in schema metadata. A capsule binds to stable IDs once during admission; lookup does not occur by string on every entity.

## 2. Storage progression

M0 is one fixed-row SoA table with u32 columns. Each table has a row count, stable entity ordering, immutable schema generation, and allocated column capacities. All hot-path sizes are validated before pointer arithmetic.

M1 may add chunks and parallel current-row iteration. M2 introduces generational entities, tombstones or swap/compaction policies, relationships, structural command buffers, and schema migration. Choose a single deterministic storage policy per profile before benchmarking alternatives. A physical reorder must not change an order-sensitive gameplay result.

Do not implement a general archetype engine before the live-loop gate. Do not automatically pack every horde's members into consecutive global entity IDs; deaths, replacements, attachments, and upgrades make identity and locality different problems. Use explicit membership ranges/indirection and measured packing policies in the RTS profile.

## 3. ABI boundary

The public ABI is C with explicit-width integers, opaque context handles, versioned structures, structure-size fields, status returns, and borrowed spans. Avoid compiler-dependent enums/bitfields, STL types, variadic calls, ownership across allocators, and implicit exceptions. Internal private C structs may evolve without becoming a public interface.

The native invocation receives validated column bindings and row spans. It does not own those buffers or retain their addresses. Each binding includes field ID, element type/stride, access permissions, length, and layout generation. Native code must either check supplied generations in its wrapper or be callable only through a wrapper that does.

Illustrative signature, to be finalized by the contract owner before workers implement it:

```c
typedef struct SuExecContext SuExecContext;
typedef struct SuInvocation SuInvocation;
typedef struct SuOutputs SuOutputs;
/* uint32_t status; no longjmp/exception propagation through this boundary. */
uint32_t su_invoke_v1(SuExecContext *ctx,
                      const SuInvocation *input,
                      SuOutputs *staged_output);
```

This is an interface sketch, not a completed header. M0-00 owns freezing the actual fields, alignment assertions, error codes, and lifetime rules. Changes after that point require a new generation and dependent tests.

## 4. Calling conventions

Use the host platform's C ABI, selected explicitly by the backend. Windows x64 register/stack rules differ from Unix System V conventions; the Windows ABI includes register arguments and caller-provided shadow space [S04]. Do not reuse earlier conversational `rdi/rsi` examples as a universal ABI.

Assembly kernels must preserve required nonvolatile registers, stack alignment, and floating-point control state. Add platform-specific ABI tests and unwind/debug metadata where required. Never claim Windows support solely because Linux-generated x86-64 bytes execute in one test.

## 5. Alignment, aliasing, and SIMD

Aligned fast paths require proven alignment. Provide an unaligned path or explicit rejection at a trusted internal boundary. Handle zero length, short arrays, nonmultiple vector tails, and overlapping ranges. The verifier or wrapper establishes nonaliasing assumptions; source annotations alone do not authorize out-of-bounds vector loads.

SIMD dispatch is per capability and supported OS register state, not merely CPU marketing name. A baseline scalar path always exists. AVX-512 and architecture-specific tuning are deferred until measurements justify their maintenance cost.

## 6. Serialization

Snapshots and network/replay data use a specified canonical encoding: versioned headers, explicit widths, little-endian integer values, length-prefixed byte/string data, stable entity/field ordering, and maximum lengths before allocation. Never serialize raw C structs or padding. The normalizer rejects duplicate field IDs and inconsistent counts.

M0 fixtures remain JSON for transparency. Binary snapshot encoding becomes necessary only when M1/M2 workload measurements justify it. The representation version and numeric profile are included in every state fingerprint. State equality compares canonical bytes or fields, not hash output alone when reporting a divergence.

## 7. Layout specialization

A later release optimizer may explore SoA, AoSoA, hot/cold splits, and system fusion. It works on a snapshot of the access graph and produces a new layout artifact plus validated bindings. It cannot change public field IDs or invalidate live addresses because capsules never retain them.

Layout candidates are compared on representative end-to-end workloads, not one kernel. Count migration/copy costs, rendering reads, snapshotting, networking, and memory peak. Dynamic data-layout retuning during a match is off by default.

## 8. Allocation model

Use lifetime-based arenas for prepared plans, world columns, tick scratch, and candidate state. Capacities are explicit. Growth is an out-of-tick transaction or a recorded resource failure. The first implementation favors easy-to-audit ownership over a custom allocator zoo.

Reference snapshot staging may use more memory than release mode. Report committed state bytes, reserved capacity, working copies, snapshots, retired code, candidate workers, and asset/device allocations separately.

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-9"></a>

**Source document: `docs/DETERMINISM_AND_BFME.md`**

# Determinism and BFME compatibility

## 1. Three separate claims

**Repeatability:** the same build and inputs reproduce a result. **Cross-backend determinism:** allowed VM/native/ISA/platform variants reproduce the same specified result. **BFME fidelity:** the remake matches independently established original behavior for the selected game/version.

These claims require different evidence. A million identical self-replays do not establish original BFME fidelity. Byte-perfect Open-BFME reconstruction, a compatible remake, and a performance-modified original executable are also separate projects with separate acceptance rules.

## 2. Numeric profiles

| Profile | Availability | Contract |
|---|---|---|
| `u32_mod_v1` | M0 | Fully specified modular integer operations in SU-LIR 0.1 |
| `fixed_authoritative_v1` | Later, only after written specification | Explicit scale, width, rounding, saturation/overflow, and conversions |
| `fp32_strict_v1` | Later, not yet defined | Exact op semantics, contraction, subnormal/NaN/zero behavior, conversions, reductions |
| `visual_relaxed_v1` | Rendering/cosmetics later | Documented acceptable numeric error; never silently authoritative |
| `bfme_compat_<variant>` | Profile work | Original behavior/rounding/order backed by evidence; not guessed from engine branding |

Do not rewrite BFME's simulation into fixed point merely because fixed point seems convenient for lockstep. Establish original behavior first. Likewise do not replace multiplication-plus-addition with fused operations or change summation order in a strict profile without equivalence evidence. Compiler floating-point modes distinguish contraction and other transformations [S06].

## 3. Deterministic scheduling

The semantic tick has a fixed duration and recorded command ordering; wall-clock pacing is outside simulation state. M0's demo tick rate is a harness setting, not a claim about BFME's tick frequency. Stable system ordering is resolved from explicit DAG edges with stable IDs as tie-breakers.

Parallel current-row systems may execute chunks in any physical order only when their effects are disjoint and outputs are merged identically. Event queues commit in a canonical order such as phase, system ID, source entity ID, local event sequence. The exact key is versioned and tested. Random numbers use explicit state/streams; never seed from thread ID, time, memory address, or work-stealing order.

Queries involving nearest neighbors, ties, equal priorities, and floating-point reductions need defined tie-breaks and iteration. Optimizing an order-sensitive algorithm is a semantic change unless evidence proves equivalence.

## 4. Replay evidence

A replay names the initial checkpoint, game/profile version, schema version, semantic manifest, content hashes, seed/state, tick sequence, ordered inputs, and semantic-revision events. The tool can stop at a selected tick, fork both revisions from the same checkpoint, and report the first differing system/entity/field.

Hashes are useful for localization. Store enough context to reconstruct and compare actual divergent values. A coarse per-tick hash does not automatically identify the causal instruction. Bisection and instrumented reruns may be required.

Regression replays test engine behavior. Compatibility fixtures must additionally cite an original-game trace, documented data interpretation, or reviewed reconstruction evidence. Mark evidence as confirmed, inferred, unknown, or intentionally different.

## 5. BFME scope carried forward

The intended remake profile targets **BFME II and Rise of the Witch-king**, with multiplayer skirmishes prioritized. War of the Ring is excluded. Preserve mechanics, balance, UI behavior/appearance, campaigns as part of eventual completeness, and observed original quirks where the remake contract requires them. Original assets are loaded from the user's local installation. Networking internals may differ for performance; compatibility with original network clients is not assumed.

The user's **vanilla 1.06 accelerator** is a different project. Do not infer from that project's target that every future remake fixture must use vanilla 1.06. Every compatibility run records its exact BFME II/RotWK edition, patch, data set, and enabled modifications. Select one documented baseline per fixture set rather than mixing editions.

Existing Rust remake code is not discarded. Evaluate reusable format knowledge, behavioral tests, schemas, and private importer utilities at defined adapter boundaries. Do not require wholesale language migration before proving the new substrate.

## 6. Profile boundaries

The generic core owns columns, scheduling, handles, bounded effects, replay, and execution. The RTS profile owns hordes, formations, spatial indexing, navigation patterns, visibility, and command semantics. The BFME adapter owns interpretation of original definitions and corresponding behaviors: weapons, armor, locomotors, upgrades, experience, powers, command sets, production, economy, construction, and scripts.

Original INI-like data may drive these systems, but parsing it does not reconstruct engine-side behavior automatically. Unknown keys or unresolved behaviors produce explicit diagnostics and compatibility gaps; silently ignoring them cannot pass fidelity acceptance.

Do not replace horde/member navigation with flow fields, merged weapon logic, or different collision rules and label it 1:1 without differential evidence. A separate enhanced profile may intentionally change behavior, but it must not contaminate compatibility mode.

## 7. Compatibility fixture format

A fixture records game variant, input command/state, relevant original data hashes, expected observed outputs, tolerance policy where justified, evidence reference, confidence, exclusions, and reason for any intentional difference. Public fixtures use synthetic assets/data where possible. Proprietary files and extracted assets remain private and are not uploaded by CI.

A useful first set covers one horde's orders, formation changes, target selection ties, attack timing, damage/armor interaction, experience/upgrade transition, death/member replacement, and a simple deterministic replay. These are test categories, not assertions that the original algorithms are already known.

## 8. Networking

Introduce recorded command playback before real networking. Then test multiple headless peers under controlled delay, loss, duplication, and reorder. The session pins semantic/content versions and compares state. Transport reliability, malicious input validation, authentication, command limits, and resynchronization are separate requirements from determinism.

Lockstep reduces the need to replicate all unit transforms but can couple advancement to command availability. Snapshot/server-authoritative and prediction-based profiles are available for other games. No engine-level claim that one model eliminates all latency is valid.

## 9. Acceptance rule

An optimization-only BFME patch may not change confirmed observed semantics. A fidelity correction may deliberately change previous remake results, but must carry new evidence and reviewed expectation changes. Distinguish `regression_pass`, `cross_backend_pass`, and `original_compatibility_pass` in reports; never substitute one for another.

Sources for numeric/tooling facts: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md). The BFME scope above is a user requirement, not a claim derived from current original-game implementation inspection.

---

<a id="section-10"></a>

**Source document: `docs/BUILD_AND_VALIDATION.md`**

# Build and validation policy
## No native project build for ordinary gameplay; fast, mandatory native checks when needed

## 1. Bootstrap once

The bootstrap agent inventories actual OS, compiler, linker, SDK, assembler, Python, available memory, and permitted network access. It resolves dependency versions to reviewed commits/releases and records their hashes and licenses. No speculative version numbers appear in this spec.

Create a small CMake/Ninja graph with independent native targets for IR, VM, world, runtime/control, and platform tests. Configure once per toolchain/build mode. Compile third-party libraries once per fingerprint. Do not create giant unity translation units, mandatory LTO, template-heavy public headers, or runtime-dependent code generation for every source edit.

Use C17 with warnings, debug symbols, and a fast development optimization setting established by bootstrap measurements. Keep sanitizers in separately fingerprinted lanes. Use precise floating-point flags in any future authoritative FP target rather than an accidental project-wide fast-math setting. Release optimization, LTO, PGO, and full platform matrices are batch lanes.

LLD is the initial cross-platform linker family [S07]. A Linux mold substitution may be benchmarked, but an unrelated project's published link time is not our budget. Windows uses its own ABI, object format, SDK, and tests.

## 2. Validation lanes

| Lane | Purpose | Author waits? |
|---|---|---|
| L0 | JSON/IR/effects/types, changed-file sanity | Yes, bounded immediate feedback |
| L1 | Targeted native compile/assemble; small unit fixtures | Briefly within budget; otherwise enqueue |
| L2 | Affected capsule/integration/replay/differential tests | No global author barrier; acceptance awaits result |
| L3 | Sanitizers, fuzzing, multi-platform, long runs | Independent build/CI workers |
| L4 | Isolated performance qualification and release builds | Dedicated controlled worker |

A gameplay change to a supported capsule goes through L0 and runtime tests without compiling the engine. A native change requires at least a real affected-target compilation and relevant tests. “Looks syntactically correct” is not an equivalent gate.

The default interactive native-check budget is **2 seconds**, a proposed scheduling threshold, not a promised compiler speed. Longer jobs keep running under the broker with durable records. Do not kill useful validation merely because it exceeded the author waiting budget.

## 3. Build broker

The broker deduplicates requests using source tree/subgraph hash, generated headers, compiler/linker versions, flags, target triple, dependency revisions, and build mode. Every job has a durable ID, state, log, artifact directory, and exit status. It refuses stale or underspecified requests.

Limit native compile jobs by observed memory and machine load. A sensible bootstrap default is one build pool and one verification pool, then tune from measurements. Do not multiply `-j all_cores` by the number of agents. Coalesce superseded edits, retain reusable outputs, and cancel obsolete work that cannot produce an admissible artifact.

The broker may be a small Python process over local job manifests. Do not build a distributed scheduler before the local queue works. Remote builders are optional and receive pinned, secret-free build inputs.

## 4. Planned command surface

These are desired interfaces to implement, **not commands that exist in this package**:

```sh
python3 tools/dev.py configure --profile dev
python3 tools/dev.py check --changed --json
python3 tools/dev.py submit-build --task M0-02 --json
python3 tools/dev.py job --id JOB_ID --json
python3 tools/dev.py test --suite zero_wait --json
python3 tools/dev.py benchmark --suite reload --json
```

Keep a thin human CLI and machine JSON output. Error exit codes remain meaningful. Do not hide a full dependency rebuild behind `check --changed`. A developer can run direct Ninja targets when diagnosing the broker.

## 5. Correct invalidation

A header change invalidates all actual consumers. A schema/ABI change invalidates affected bindings and compiled artifacts. A code generator change invalidates its outputs. Toolchain or flag changes produce a new cache namespace. Results from an unknown fingerprint are misses, never assumed hits.

Prefer the existing build tool's dependency graph to a second incomplete hand-maintained graph. Capsule dependencies still need their own semantic graph because they are runtime artifacts rather than C translation units.

## 6. CI policy

Use public-repository standard GitHub-hosted runners for correctness and build checks where appropriate. Standard public-repository runner usage is free under GitHub's documented policy, while larger runners are charged; concurrency, storage, execution, and other service limits still apply [S08, S09]. Therefore CI is neither unlimited parallel capacity nor a subsecond interactive loop.

Use affected-target selection, grouped matrices, dependency caches, artifact retention limits, and cancellation of superseded runs. Pin third-party actions by reviewed immutable revisions. Never expose secrets or persistent personal machines to untrusted pull-request code. A dedicated performance runner accepts only authorized trusted workloads and does not use broad privileges merely to obtain counters.

Do not provision paid larger/GPU runners, change billing settings, or assume existing IBM/Vast credits remain active without authorization and verification.

## 7. Evidence artifacts

Each run stores a machine-readable result containing exact commands, environment fingerprint, source/artifact hash, test corpus revision, actual duration, return codes, and explicit skipped/unavailable fields. CI artifacts distinguish a specification target from an observed result.

Build failure feedback identifies the smallest failing target and relevant diagnostics. The repair worker owns the failing scope; unrelated capsule work may continue. Repeated ABI failures stop dependent work until the contract owner repairs the interface.

## 8. Bootstrap escape clauses

If a compiler is missing, report that fact and continue with useful spec/example/parser work that can be validated using available tools; do not fabricate native results. If network access is missing, use existing pinned dependencies or record the missing dependency and work on independent tasks. Do not substitute an enormous new toolchain to avoid a small setup issue.

These are truthful degraded modes, not a declaration that uncompiled code is complete. M0 cannot close until the native loop actually runs.

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-11"></a>

**Source document: `docs/PERFORMANCE.md`**

# Performance and velocity qualification

## 1. Optimize profiles, not slogans

The engine targets high author throughput and efficient execution across distinct workload profiles. A candidate is admissible only after the applicable correctness, resource, and compatibility gates. Then compare runtime latency distribution, throughput, memory, energy where measurable, and development cost. Do not sacrifice simulation correctness to satisfy a frame-time target.

The reference implementation is intentionally simple. The optimizer needs a trustworthy semantic baseline, not a baseline deliberately made slow to inflate speedup.

## 2. Initial targets — all unmeasured

| Measurement | Proposed target | Conditions |
|---|---|---|
| IR prepare latency | p95 ≤ 20 ms | ≤64 KiB source, existing fixed schema, warm process, 1,024-row preview |
| Isolated preview response | p95 ≤ 100 ms | Reusable worker, small fixture; excludes long qualification suites |
| Code installation critical section | p95 ≤ 1 ms | Code-only swap; bindings/scratch already prepared; no migration |
| Warm edit → first preview effect | p95 ≤ 100 ms | Report preparation, queue, worker, and tick-boundary delays separately |
| Targeted native incremental check | Aim p95 ≤ 2 s | Small changed target, cached dependencies, reference machine |
| First-party clean dev build | Aim ≤ 10 s | Only after scope and toolchain exist; separately report dependency builds |
| Failed candidate publication | Exactly zero | Active code and committed state unchanged by rejection |
| Memory retired after repeated swaps | Bounded | Explain retained checkpoints/code rather than hiding growth |

Missing a latency target does not justify skipping correctness checks. Record the measurement, profile the bottleneck, and revise implementation or clearly scope the target. The target is not a hard real-time guarantee across operating systems and arbitrary hardware.

## 3. Reference environments

The user-reported primary development machine is Linux on an Intel i7-14700K, 48 GB RAM, Optane P5800X storage, and Arc A770. The user also has a Windows i5-14400F system with 16 GB RAM and a Quadro P600. This pack has not executed on either machine.

Record actual OS/build, compiler, power policy, core placement, thermal state, driver/device capabilities, and background load when testing. Do not assume homogeneous CPU cores or identical PMU behavior. GPU tests use capability checks and a clear fallback; headless simulation has no GPU requirement.

## 4. Latency decomposition

Measure `capture → parse → verify → prepare → queue → isolated test → admit → boundary wait → publish → first observable tick`. Native specialization has a separate timeline. File notification delay is included in watcher metrics but excluded from explicit-submit preparation metrics.

Publish separate warm and cold distributions. A tiny average does not establish acceptable p95/p99. Include first-run imports, cold caches, native backend startup, process spawn, and full restart costs where applicable. A prewarmed worker result cannot be relabeled as cold startup latency.

## 5. Runtime measurements

Use a monotonic clock for wall time. Record row/entity counts, system/tick durations, bytes of active state, allocations, and queue/backpressure data. Use PMU counters only when available and reliable. Linux performance monitoring has permission and resource controls, and event availability is hardware-dependent [S05].

`cycles`, `instructions`, cache misses, branch misses, and memory bandwidth estimates are optional fields with a reason when unavailable. Do not infer exact bytes read or a cache-miss percentage from elapsed time. Multiplexed counters need scaling/validity data; instrumentation overhead needs measurement.

## 6. Benchmark methodology

Maintain fixed versioned workloads and a held-out corpus controlled by the verifier. Record baseline and candidate source hashes, artifact hashes, numeric profile, mode, ISA dispatch, compiler flags, warmup, repetition count, and statistic definition.

Run correctness tests before timing. Alternate/randomize baseline and candidate trial order to reduce drift; use repeated trials and confidence intervals appropriate to the data. Run final comparisons without competing agent builds or other candidates on the same resources. Keep raw samples. Treat inconclusive/noisy differences as inconclusive, not wins.

Optimize-only acceptance requires evidence beyond a microkernel when it affects layout, scheduling, or memory. Measure total simulation tick and end-to-end frame/latency where applicable. Count snapshot staging, transfers, event merging, synchronization, and cold-path costs rather than timing only arithmetic.

Use held-out seeds/workload shapes to detect benchmark overfitting. The candidate author cannot redefine the population, remove tails, weaken tolerances, or compare different safety modes without an explicitly reviewed test change.

## 7. Workload ladder

**M0:** 0, 1, 7, 8, 9, 1,024 rows; overflow cases; valid/invalid/repeated edits; snapshot restore. Stress at 100,000 and 1,000,000 rows is a separate diagnostic, not proof that full game AI scales similarly.

**M1:** reference versus scalar-native versus AVX2 on awkward sizes and alias/alignment cases; stable and changing working sets; reload stress while native work is queued.

**M3 RTS microcosm:** scalable independent actor and horde counts; movement, target acquisition, projectile updates, damage/death, relationship churn, clustered congestion, and replay hashing. Include an adversarial dense contact scenario, not only separated units moving in straight lines. No unit-count/FPS claim precedes measurement.

**Later rendering:** actual skinning, visibility, material variety, shadowing, particles, overdraw, and GPU memory pressure. A million tiny compute particles is not equivalent to a million high-quality rendered effects.

## 8. Dispatch policy

Native CPU variants may dispatch by detected capability and a measured workload class. Keep a scalar baseline and account for tails and transition cost. Do not assume wider vectors always win. Runtime GPU selection considers transfer/synchronization, queue occupancy, semantic eligibility, and state residency. Start with static authored placement; automatic crossover tables are later work.

Release tuning is performed on representative hardware. Do not run an expensive autotuning tournament every player launch without explicit product policy. Store bounded tuning caches with device/driver/backend identities and a known-good fallback.

## 9. Agent-throughput metrics

Track accepted useful capability tasks per elapsed hour, total agent-seconds consumed, p50/p95 author idle time, verifier queue time, rejection rate, rework rate, stale-patch rate, integration conflicts, and human interventions. Report these separately; use a fixed task rubric to compare runs.

Increase agent count only while accepted throughput improves without unacceptable rework or resource contention. The ideal count can be lower than the maximum the harness can spawn. Avoid optimizing for LOC, PR count, or benchmark-only patches.

## 10. Performance gates

CI correctness gates are deterministic. Fine performance gates require controlled runners and a stored baseline. Establish thresholds from observed noise and product budgets; this pack deliberately does not impose fabricated cycles-per-entity values. A meaningful regression requires an explicit exception or repair, not silently widening the budget.

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-12"></a>

**Source document: `docs/AGENT_RPC.md`**

# Agent control protocol
## Local JSON-lines v0.1

These are interfaces to implement, not APIs already present. M0 exposes a local stdio session owned by one CLI/orchestrator. It must not expose a public network listener. Later multi-client transport may use a permission-controlled local socket or named pipe with authentication and per-client capabilities.

## 1. Envelope

One strict UTF-8 JSON object per line, maximum 1 MiB per message initially. Larger state/artifacts are referenced by validated immutable IDs, never pasted as uncontrolled executable content. Reject duplicate keys, malformed UTF-8, unexpected keys, oversized collections, and path escapes before allocating large buffers.

Request:

```json
{"protocol":"0.1","id":"request-17","method":"capsule.submit","params":{"capsule_path":"capsules/movement","expected_world_revision":"world-3"}}
```

Response:

```json
{"protocol":"0.1","id":"request-17","ok":true,"result":{"candidate_id":"candidate-12","status":"received"}}
```

IDs in these snippets are illustrative, not current engine state. Response `ok:false` carries an error object; stdout remains parseable and diagnostic prose goes to stderr or structured events. An accepted queue request does not mean a candidate passed.

## 2. Core M0 methods

| Method | Inputs | Result |
|---|---|---|
| `engine.info` | None | Versions, implemented capabilities, runtime mode, process identity |
| `world.create` | Fixture/artifact ID, tick settings | New world ID and revision |
| `world.inspect` | World, entity/field selection, result limit | Bounded canonical state and tick |
| `world.step` | World, exact tick count | Completed tick range or explicit failure |
| `world.run` / `world.pause` | World, pacing policy | Running/paused status |
| `world.snapshot` | World, expected revision | Immutable snapshot ID and metadata |
| `world.restore` | World, snapshot, expected revision | Restored revision or mismatch error |
| `capsule.submit` | Immutable capsule source or allowed path, expected base | Candidate ID, captured source hashes |
| `candidate.status` | Candidate ID | State, diagnostics, evidence IDs |
| `candidate.evaluate` | Candidate, fixture/test IDs | Asynchronous test job ID |
| `candidate.install` | Candidate, expected revision, allowed effective tick | Pending/installed version event |
| `events.read` | Cursor, maximum count | Bounded ordered events plus next cursor |
| `tests.run` | Suite, source/candidate ID | Test job ID |
| `job.status` | Job ID | State, exact input fingerprints, artifacts |

`world.step` is valid for a paused world; reject it during free running unless the interface explicitly pauses at a boundary. `candidate.install` requires sufficient evidence for the chosen policy. A client cannot set its own “passed” flag. The supervisor resolves installed revisions against its evidence registry.

## 3. Later methods

`schema.plan_migration`, `schema.commit_migration`, `replay.compare`, `benchmark.run`, `dependency.query`, `artifact.inspect`, `native.specialize`, and `task.lease` are added only in their milestone. Unsupported methods return `SU_E_UNSUPPORTED_METHOD`; never return placeholder success.

A human-facing CLI can wrap these operations. A future MCP-style adapter is an optional wrapper, not the core protocol and not a bootstrap requirement.

## 4. Concurrency and idempotence

Every mutation includes an expected world/source revision or is explicitly creation-only. Stale expectations return `SU_E_STALE_REVISION`. Retrying the same client request ID with the same canonical payload returns the previous outcome while retained; retrying it with different payload is an error.

Jobs are immutable inputs with a mutable status record. Cancellation is best-effort before installation; report whether work was actually cancelled. A cancelled result cannot later be installed. Events use monotonic sequence numbers independent of simulation tick.

## 5. Errors

Initial error set: `SU_E_PARSE`, `SU_E_TYPE`, `SU_E_DUPLICATE_KEY`, `SU_E_UNKNOWN_OPCODE`, `SU_E_UNDEFINED_REGISTER`, `SU_E_DUPLICATE_REGISTER`, `SU_E_UNDECLARED_READ`, `SU_E_UNDECLARED_WRITE`, `SU_E_SCHEMA_MISMATCH`, `SU_E_STALE_REVISION`, `SU_E_RESOURCE_LIMIT`, `SU_E_UNSUPPORTED_METHOD`, `SU_E_UNSUPPORTED_MIGRATION`, `SU_E_CANDIDATE_FAILED`, `SU_E_WORKER_FAILED`, `SU_E_CAPABILITY_DENIED`, and `SU_E_IO`.

Errors identify request/candidate/source hash, relevant JSON pointer, expected/actual values, and whether retry is meaningful. Do not leak secrets or arbitrary filesystem contents through diagnostics.

## 6. Trust and filesystem policy

M0 methods can read only the selected project/artifact roots. Normalize and validate paths, reject traversal/symlink escapes according to a documented policy, and capture immutable content before evaluation. Importing an artifact does not execute its metadata as shell commands.

Network and native compilation requests are not exposed to ordinary gameplay capsules. Build tools are invoked by the broker using validated arguments, not interpolated untrusted shell strings. Avoid default system-wide credentials in worker environments.

## 7. Evidence and metrics

Report host monotonic durations and simulation ticks separately. `engine.info` exposes capability presence, not imaginary counters. `job.status` says `not_run` or `unavailable` when appropriate. Results include exact workload and artifact identities so a worker can safely act on them without scraping text logs.

---

<a id="section-13"></a>

**Source document: `docs/BACKENDS_AND_DEPENDENCIES.md`**

# Backends and dependency decisions

## 1. Minimize new research on the critical path

The project's novelty should be the development/runtime contract, not simultaneously reinventing JSON, instruction encoding, a linker, a shader compiler, an operating-system sandbox, and a complete programming language. Reuse small pinned tools behind narrow adapters. A dependency compiled once is different from a dependency rebuilt on every gameplay edit.

M0 requires C17, native platform support, the build toolchain, a strict JSON library, and a reviewed hashing implementation. Python standard-library tooling is outside the hot runtime. The native JSON default is yyjson; its own documentation describes the library and APIs [S10]. Review its parser configuration and enforce duplicate-key/semantic policy at our boundary rather than assuming all defaults meet SU-LIR requirements.

## 2. Native backend decision

M1's default is **AsmJit in a narrow C++ translation-unit boundary exposing a C ABI**. It already provides machine-code emission and optional register allocation [S01]. The rest of the core remains C17; AsmJit headers do not spread through `include/sutekh/`.

Initially lower only SU-LIR 0.1. Implement a scalar backend before AVX2. Use an explicit stack/register plan rather than a general optimizer. Record source mappings from IR instruction indices to native ranges and preserve a disassembly inspection path. Do not hand-encode VEX/EVEX instructions to satisfy an “assembly purity” metric.

If a concrete platform/tooling issue blocks AsmJit, record an architecture decision with evidence and an alternative. Cranelift can emit callable code into memory and is an available reference or future optimizing backend [S02], but M1 must not grow two backends merely because both are interesting. A Rust dependency in a prebuilt optional tool is not the same as imposing Rust compilation on all game edits.

## 3. Handwritten assembly

Raw x86-64 kernels are optional trusted native components. Every kernel has an ABI, read/write/alignment contract, scalar reference, zero/tail/bounds tests, differential corpus, and a measured justification. The assembly lane uses a pinned assembler when source changes; normal Live IR does not spawn it.

Avoid handwritten replacements for vetted security primitives or general memory routines without a demonstrated bottleneck and expert-quality test coverage. Assembly is not a substitute for an algorithmic or layout fix.

## 4. Native memory and safety

Generated code pages follow writable-then-executable publication, with no permanent RWX mapping. Handle allocation/protection failures explicitly and preserve the previous implementation. Apply platform cache synchronization rules [S03]. Debug/unwind registration, return-address handling, and ABI tests are part of backend completion.

Checked code generation is still a trusted compiler implementation, not a complete malicious native-code verifier. Admit unreviewed native candidates only in isolated workers. A later optional WebAssembly sandbox can be evaluated when mod/plugin requirements justify it; it is not imposed as an extra M0 runtime.

## 5. Shader/backend tooling

Use an established shader compiler through an adapter when rendering begins. Cache outputs by source, includes, options, target capabilities, compiler version, and resource layout. Do not begin with a custom SPIR-V compiler.

The driver can still compile pipeline/shader work internally, and pipeline creation may be expensive [S11]. Schedule it off the render-critical path and keep a compatible old pipeline or safe fallback until readiness. Native CPU code generation does not remove GPU compilation costs.

## 6. Toolchain locking

Bootstrap records exact versions/commits, download origins, checksums, license texts, patches, target triples, compiler/linker flags, and cache key inputs. Unsupported platform configurations are reported rather than silently emulated through undefined ABI assumptions.

Do not fetch dependencies at runtime in a shipped game. Development builds can operate offline after approved dependencies are present. A new dependency must identify its problem, footprint/build cost, update policy, and simpler alternatives. No arbitrary “zero dependencies” rule should force years of unnecessary reimplementation.

## 7. Decision ledger

| Decision | Status | Revisit trigger |
|---|---|---|
| C17 resident core | Selected | Measured concrete blocker, not fashion |
| JSON IR before custom syntax | Selected for M0 | M0 complete; author ergonomics measured |
| Reference VM before JIT | Selected | Never remove semantic oracle |
| AsmJit C-ABI adapter | Selected for M1 | Documented unsupported need/performance evidence |
| No CPU/GPU autotuner in M0 | Selected | Representative GPU workloads and cost model exist |
| Static SoA before auto-layout | Selected | Real access traces and migration tests exist |
| Engine license | MIT, selected by owner on 7 October 2026 | Third-party components retain their own terms |
| Repository / final name | Public `admiraly/sutekh`; final branding pending | Before commercial branding |

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-14"></a>

**Source document: `docs/TEST_SPEC.md`**

# Executable acceptance tests

## 1. Evidence rules

Each test produces structured status, exact input/source/artifact identities, actual command and return code, and enough state to reproduce failure. Tests cannot declare themselves passing by reading a candidate-provided flag. Negative tests pass only when the intended rejection occurs without collateral state changes.

The specification checker validates JSON, task dependencies, links, and small reference examples. It is not a substitute for the following native runtime tests.

## 2. M0 acceptance catalog

| ID | Test | Required observation |
|---|---|---|
| T00 | Clean bootstrap | Documented command builds the small native runtime using locked dependencies |
| T01 | Reference movement | Included world/capsule produce exactly the expected fields after 1 and 3 ticks |
| T02 | Empty and awkward counts | 0, 1, 7, 8, 9, and 1,024 rows; no invalid access or skipped tail |
| T03 | Numeric edges | u32 wraparound, multiplication, subtraction, comparisons, selection match defined semantics |
| T04 | Invalid input matrix | Unknown op, bad type, duplicate key/register, undefined register, denied read/write, oversize input rejected |
| T05 | Successful code-only swap | Behavior changes at recorded tick; world state and supervisor/worker PIDs preserved |
| T06 | Failed swap | Active semantic hash and committed state unchanged by invalid candidate |
| T07 | Stale candidate | Changed expected world/schema revision rejects stale installation |
| T08 | Snapshot round trip | Restore canonical state, tick, and semantic manifest exactly |
| T09 | Working-root failure | Failure after an earlier system wrote data leaves the published full-tick root unchanged |
| T10 | Candidate isolation | Kill or time-limit a candidate worker; active simulation and supervisor remain alive |
| T11 | Repeated reload | Bounded retained memory across 1,000 successful/failed/reverted revisions; no stale references |
| T12 | No native gameplay build | Instrument child processes/build invocations; capsule-only swaps invoke no compiler, assembler, linker, or native project build |
| T13 | Latency decomposition | Real prepare, queue, boundary, publish, and first-effect distributions with target comparison |
| T14 | Deterministic fixture repeat | Same initial state, ordered inputs and semantic changes reproduce canonical state |
| T15 | Structured protocol | Requests/errors remain parseable; oversized/invalid messages and path escapes rejected |

Native core build T00 is deliberately distinct from gameplay no-build T12. Passing one by omitting the other is not allowed.

## 3. M1 backend qualification

For every supported opcode, generate bounded random programs and edge-case inputs and compare reference VM with native scalar and AVX2. The random generator itself is seeded and its corpus/hash is stored. Include independent hand-authored known-answer cases so a shared generator bug cannot define correctness alone.

Test zero rows, vector tails, unaligned columns, maximum values, layout mismatches, denied aliasing, code-cache invalidation, allocation/protection failure, concurrent retirement, and platform calling conventions. Run ABI clobber tests for required preserved registers and stack alignment. Use guarded mappings where appropriate for bounds detection.

Sanitizers improve coverage of instrumented native C/C++ paths, but handwritten/generated assembly may not receive equivalent instrumentation. Backend bounds/differential tests remain mandatory. Do not treat sanitizer silence as proof that arbitrary generated bytes are safe.

## 4. Scheduling tests

Run serial and parallel schedules under multiple worker counts and varied physical completion order. Compare canonical outputs and staged event order. Deliberately add conflicting effects and cycles; the scheduler must reject invalid parallelization and ordering cycles. A task's declared reads/writes must match derived IR effects.

## 5. Schema tests

Add a field with a default; rename without changing ID; reject removal of a field still consumed; execute an explicit retype migration; simulate insufficient memory; retain rollback roots; invalidate and regenerate bound native code. Test a noninvertible migration using snapshot recovery rather than a fabricated inverse.

## 6. RTS and BFME tests

The synthetic RTS suite exercises movement, formation membership, targeting ties, projectiles, damage/death, entity churn, crowding, and replay. It establishes engine behavior, not historical BFME fidelity.

The BFME suite labels exact game variant and evidence. It distinguishes confirmed observed behavior from inferred or unimplemented behavior. Original-game compatibility tests that cannot run in the environment are marked not run; synthetic tests do not replace them silently.

## 7. Failure minimization

When a replay or differential test fails, retain the seed, first divergent tick/system/field, command prefix, manifest, and reduced fixture where possible. A minimizer may shrink inputs only while preserving the failure. Do not discard a failure as “flaky” without diagnosing nondeterminism or environmental causes.

## 8. Acceptance versus performance

Correctness tests have exact specified outcomes. Timing tests report measurements and compare against declared machine/workload targets. Shared hosted CI may report timings, but controlled performance qualification determines small regression claims. Every gate identifies whether it is mandatory, provisional, or unavailable.

---

<a id="section-15"></a>

**Source document: `docs/GAME_PROFILES.md`**

# Game profiles and scope boundaries

The core is a reusable execution system. A profile selects numeric semantics, scheduling, authority, data families, budgets, and permitted specialization. Profiles share infrastructure without pretending all games need the same simulation model.

## BFME II / RotWK remake

Priority is exact gameplay behavior for the chosen variant, multiplayer skirmish first, then eventual campaign/UI completeness, excluding War of the Ring. Hordes, formations, commands, weapons, upgrades, original-data interpretation, and determinism are profile responsibilities. Assets come from a local installation. Original-client network compatibility and original save/replay formats are not assumed requirements.

Do not migrate or replace the existing Rust remake until the new engine has an independently useful proved slice and a deliberate integration plan. Reuse evidence and tests before rewriting already understood systems. The new engine is also separate from binary optimization of the original 32-bit game.

## Shatterfront

A large RTS profile with asymmetric factions, commanders, economy, construction, strategic AI, formations, fog of war, and large battles. It may choose improved pathfinding or modernized mechanics that would be impermissible in a BFME 1:1 compatibility profile. Share RTS infrastructure where semantics truly match, not by coupling faction-specific logic to the core.

## RED HORIZON

A large-scale co-op FPS/RTS battlefield profile: direct player control plus many AI actors, projectiles, explosions, audio, and effects. Distinguish authoritative combat actors from lower-cost visual population. Use explicit simulation/animation/AI level-of-detail policies when allowed by gameplay. Those approximations are not automatically acceptable in BFME.

The game project's GPLv3 decision remains attached to that project. Engine licensing is a separate owner decision. No specific future unit/particle count or frame rate is guaranteed by these architecture documents.

## Slingshot

A latency-focused physics/projectile FPS profile with readable high-speed movement, ricochets, prediction, and replay/rollback investigation. High tick rates are a workload/physics budget decision, not a blanket engine default. Deterministic or reconcilable collision rules need their own tests; author iteration must not force renderer or networking restarts for every gameplay edit.

## MMO

A headless server/zone profile with long-lived state, snapshot/replication policies, authority, persistence, security, and migrations across versions. This is later work. It must not impose databases, distributed consensus, cross-zone ownership, or live production migrations on M0.

## Shared contracts

All profiles use stable schema identities, bounded effects, structured diagnostics, a reference path, semantic versioned artifacts, isolated candidate validation, and resource-aware scheduling. Rendering and AI quality may vary by profile; authoritative rules cannot silently degrade under load unless the game explicitly defines that behavior.

These profile descriptions carry forward the user's project direction. They do not assert that those games or their subsystems have been implemented in Sutekh.

---

<a id="section-16"></a>

**Source document: `docs/RENDERING_AND_ASSETS.md`**

# Rendering, assets, and the shipping player
**Later-stage contracts. Do not implement these before the headless gates.**

## 1. Simulation/presentation boundary

The renderer consumes committed presentation snapshots and effect events. It cannot mutate authoritative simulation state through entity pointers or GPU readbacks. Interpolation, cosmetic particles, camera effects, and animation presentation can run at a different rate from the semantic simulation tick.

GPU scene data uses stable object/asset IDs and explicit lifetime generations. The renderer owns GPU buffers, descriptors, pipelines, fences, and retirement. Their handles never appear in canonical simulation snapshots.

## 2. Initial rendering profile

Use Vulkan with a tested baseline capability profile; Vulkan 1.3 is a candidate initial desktop baseline, subject to actual device/driver support. Record required versus optional features. Headless builds never depend on Vulkan. Introduce a minimal instanced diagnostic scene before adding a GPU-driven frame graph, compute culling, animation, shadows, or large particles.

Do not promise that every unit type collapses into one draw. Material state, mesh variants, skinning, visibility, shadows, transparency, and rendering passes determine batching. The engine should reduce per-object CPU work while measuring actual GPU and CPU cost.

## 3. Shader/pipeline edits

Shader source is an asset with explicit include dependencies and compiler/target/resource-layout fingerprints. Compile only affected variants using an established compiler adapter. Prepare driver pipelines away from the render-critical path. Keep the last compatible pipeline active until the new one is ready; use a documented fallback when an interface change makes that impossible.

Khronos documents internal shader compilation during pipeline creation and its potential frame-time cost [S11]. Pipeline cache-control features can avoid unexpectedly compiling in a guarded creation attempt on supporting devices [S12]. These mechanisms help schedule work; they do not guarantee a zero-cost first-use pipeline.

Changing descriptor/resource layouts is an interface migration, not just replacing a shader pointer. Revalidate bindings and dependent materials. Retire old GPU resources only after relevant device work completes. Pipeline caches include implementation/device compatibility metadata and are never trusted across arbitrary driver changes.

## 4. Asset pipeline

Asset identities are content- and dependency-addressed. Each importer declares source formats, dependency discovery, output version, target settings, and importer/tool version. Changing one model invalidates that model and its affected derived artifacts—not the entire project. Changes to importer logic correctly invalidate all affected outputs.

Imports run in isolated bounded workers. Malformed or hostile files cannot write outside the artifact store or execute arbitrary scripts. Large texture/mesh conversion can remain pending while the game uses the previous artifact. A viewport must expose stale/loading/error state rather than pretending an import completed.

Budget CPU staging, upload bandwidth, GPU memory peak, and resource retirement. Replacing a large texture can temporarily need both old and new allocations. A live asset edit is allowed to be rejected or deferred for insufficient resources.

## 5. BFME assets

The compatibility adapter reads original assets from a configured local installation. Importer tests use synthetic/minimal redistributable fixtures in public CI. Do not commit original archives, extracted textures/models/audio, or user-installation paths. Unknown format behavior is tracked as a compatibility gap with evidence.

Preserve semantic separation between visual upgrades and original gameplay. Changing unit mesh/particle density is not permission to change target selection, collision, attack timing, or authoritative visibility.

## 6. Audio

Audio consumes committed effect events. Use an established device/mixing backend initially; do not rewrite codecs in assembly. Real-time audio callbacks cannot block on the agent control plane, memory allocation, asset import, or GPU work. Asset updates and voice lifetime changes are prepared outside the audio callback.

Speculative candidates do not play real sounds. Development effect IDs and commit indices prevent duplicate externally visible effects after controlled recovery where supported.

## 7. Release player

The shipped player/server excludes agent RPC, source watchers, untrusted compilation, test imports, and development secrets. It loads a pinned semantic/content manifest and validated platform artifacts. JIT may be disabled where unnecessary or disallowed. Offline native optimization and shader preparation are packaging operations, not normal gameplay edit operations.

Maintain debug/source-map artifacts separately for profiling and crash diagnosis. Distribution size and startup time are measured alongside runtime performance. Dependency notices and the final license are owner-approved release requirements.

Sources: [SOURCE_NOTES.md](docs/SOURCE_NOTES.md).

---

<a id="section-17"></a>

**Source document: `docs/RISK_REGISTER.md`**

# Risk register and anti-failure policy

| Risk | Failure mode | Required mitigation / stop condition |
|---|---|---|
| Compiler project consumes engine project | Months of language/backend work before usable gameplay | M0 JSON + VM; reuse instruction encoder; postpone surface syntax |
| Assembly purity | Slower development and more defects without measured speed | Reference implementation first; ASM only with ABI/tests/measurements |
| Fake zero-wait | Skipped checks and unlimited broken source | Bounded native debt; targeted checks; independent verifier |
| Fake hot reload | Respawn world and call it a swap | Assert active PID/state preservation for ordinary code-only edits |
| Native crash corrupts state | Pointer rollback cannot undo memory writes | Candidate process isolation; committed roots; checkpoint/log recovery |
| Process mistaken for full sandbox | Candidate reads secrets or attacks host resources | Least privilege, resource bounds, controlled filesystem/network, stronger OS sandbox when threat model requires |
| Large migration stalls | Full-state rewrite inside tick boundary | Prepare out of tick; explicit bounded migration; reject or controlled checkpoint path |
| Determinism label is vague | FP, order, RNG, or scheduler differences create desyncs | Versioned numeric/effect semantics and cross-backend replay tests |
| BFME regression mistaken for fidelity | Self-consistent but historically wrong behavior | Independent original-game evidence and versioned compatibility fixtures |
| Thousands of agents bottleneck integration | Conflicts, stale patches, memory pressure | Scope leases, small tasks, one integrator, measured scale-up |
| Optimization overfits benchmark | Microkernel win hurts actual game | Held-out workloads, real end-to-end tests, verifier-owned baselines |
| GPU work assumed free | Transfer/driver compile/overdraw stalls | Full CPU/GPU cost model; pipeline preparation; bounded assets |
| Counter telemetry invented | Unavailable PMUs shown as precision metrics | Null + reason; record hardware and instrumentation |
| CI assumed unlimited | Rate limits/costs/queues stall work | Local loop, cache/dedup, bounded jobs; no unapproved paid capacity |
| Agent modifies evidence | Tests weakened to make patch pass | Separate expectation review; exact corpus hashes; prohibit silent budget changes |
| Custom infrastructure explosion | New build/orchestration/storage frameworks become prerequisites | Reuse small established tools; require a current acceptance test |
| Dependency versions drift | Nonreproducible codegen or poisoned cache | Pin versions/hashes; exact cache fingerprints; clean qualification builds |
| Existing games overwritten | User loses work or project scope changes | Separate engine checkout; explicit later migration decision |

## Threat boundary

M0 targets accidental defects in agent-produced IR and malformed inputs within a local development environment. A checked VM reduces expressible unsafe behavior but its C implementation can still contain bugs. Fuzz it and isolate candidate execution. Native candidates have a stronger threat surface and require a separate trusted/untrusted admission policy.

Do not expose the agent control plane on the public internet. Do not run untrusted repository code on a secret-bearing persistent self-hosted runner. A public contribution cannot approve its own access to credentials, network, native execution, or canonical integration.

## Review questions

Before adding a subsystem, identify the existing failing acceptance test it resolves. Before claiming a speedup, identify the exact semantic contract, machine, modes, raw samples, and held-out workload. Before increasing agents, identify ready independent tasks and verifier headroom. Before broadening BFME compatibility, identify the original evidence and variant.

A missing answer creates a concrete follow-up task, not permission to invent a result.

---

<a id="section-18"></a>

**Source document: `docs/SOURCE_NOTES.md`**

# Primary sources and evidence boundaries

Checked on **6 October 2026**. These sources support concrete tooling/platform facts; they do not validate Sutekh's proposed latency targets or engine performance. Dependency APIs and service limits may change; implementation agents must pin actual versions and recheck configuration-sensitive claims. No external benchmark numbers have been adopted as Sutekh results.

## S01 — AsmJit

Publisher: AsmJit project. Source: `https://asmjit.com/`

Supports the decision that an existing C++ library can emit machine code and provide optional register allocation behind an adapter. It does not verify our IR or establish an engine speedup.

## S02 — Cranelift JITModule

Publisher: Bytecode Alliance / crate documentation. Source: `https://docs.rs/cranelift-jit/latest/cranelift_jit/struct.JITModule.html`

Documents in-memory callable code/data, finalization, and the safety precondition that code memory must not be released while functions/pointers remain in use. It is a reference/alternative backend, not an M0 dependency.

## S03 — Windows generated-code page protection

Publisher: Microsoft. Source: `https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect`

Documents changing page protections and the caller's responsibility for instruction-cache coherency when publishing executable memory. Our W^X policy and retirement protocol are design requirements built around platform support.

## S04 — Windows x64 calling convention

Publisher: Microsoft. Source: `https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170`

Documents argument registers, stack/shadow-store conventions, register preservation, alignment, and unwind concerns relevant to a Windows native backend. The engine must separately implement/test each supported ABI.

## S05 — Linux performance-monitoring access

Publisher: Linux kernel documentation. Source: `https://cdn.kernel.org/doc/html/latest/admin-guide/perf-security.html`

Documents permission, capability, and resource controls for performance monitoring. This supports explicit unavailable/null telemetry rather than promising all counters on all machines. Do not weaken machine security solely for convenience.

## S06 — Clang numeric compilation modes

Publisher: LLVM/Clang. Source: `https://clang.llvm.org/docs/UsersManual.html`

Documents floating-point control and compilation behavior. The architecture consequently defines contraction/rounding/other arithmetic behavior explicitly before admitting a cross-backend deterministic FP profile. The source does not establish original BFME arithmetic semantics.

## S07 — LLD

Publisher: LLVM. Source: `https://lld.llvm.org/`

Documents the linker family and supported formats. We use it as the default native linker option rather than promising another application's link benchmark transfers to this project.

## S08 — GitHub Actions billing

Publisher: GitHub. Source: `https://docs.github.com/en/billing/concepts/product-billing/github-actions`

Documents free standard-runner use for public repositories and separate charging for larger runners, plus storage/billing rules. Recheck the actual account and runner configuration before enabling paid resources.

## S09 — GitHub Actions limits

Publisher: GitHub. Source: `https://docs.github.com/en/actions/reference/limits`

Documents concurrency, workflow/job, storage, and related service limits. Public repository status does not create unlimited concurrent capacity or a low-latency interactive builder.

## S10 — yyjson

Publisher: yyjson project. Source: `https://ibireme.github.io/yyjson/`

Documents the C JSON library selected for the native metadata boundary. Sutekh's stricter duplicate-key, schema, type, and resource rules require their own verification and tests.

## S11 — Vulkan pipeline management

Publisher: Khronos Vulkan Documentation Project. Source: `https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html`

Documents driver-side shader compilation during pipeline creation and the use of caching/preparation to reduce stalls. Producing SPIR-V does not make GPU pipeline creation free.

## S12 — Vulkan pipeline creation cache control

Publisher: Khronos Vulkan Documentation Project. Source: `https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pipeline_creation_cache_control.html`

Documents an API mechanism for controlling creation behavior when compilation would be required, subject to implementation support. It informs a later off-critical-path pipeline manager, not a zero-latency guarantee.

## User-derived requirements, not externally verified engine facts

The BFME II/RotWK scope, War of the Ring exclusion, user-reported hardware, named game concepts, maximum agent velocity preference, and desire for assembly-level control come from the conversation/project context. We have not inspected current game repositories or run their binaries while preparing this specification. No existing repository was modified, no engine benchmark was run, and no engine license or remote repository was created.
