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
