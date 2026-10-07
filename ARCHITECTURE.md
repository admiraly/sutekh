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
