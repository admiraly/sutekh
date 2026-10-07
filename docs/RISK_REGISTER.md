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
