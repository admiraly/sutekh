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
