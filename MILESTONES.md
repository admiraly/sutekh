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
