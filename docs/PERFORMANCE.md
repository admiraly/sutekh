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

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
