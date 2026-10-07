# PERFORMANCE PROMPT — Improve a measured bottleneck

Work only on an assigned, reproducible performance task after the relevant correctness baseline exists. Read the system contract, numeric/profile semantics, `docs/PERFORMANCE.md`, current evidence, and the exact allowed write scope.

First reproduce the baseline on the specified machine/mode/workload. If hardware counters are unavailable, report null and the reason. Do not invent cycles, IPC, cache misses, bandwidth, GPU occupancy, or a speedup. Capture wall-time distributions, input/artifact identities, environment, and raw samples.

Identify whether the bottleneck is algorithmic, layout, branching, allocation, synchronization, interpreter dispatch, rendering, or transfer cost. Prefer the smallest change that removes actual work. Assembly/SIMD is permitted when appropriate, not required. Do not optimize a tiny kernel while ignoring dominant full-tick cost.

Generate a small number of independent candidates within the resource budget. Correctness screening can run concurrently; final performance comparison must not be distorted by competing agents/builds on the measured resources. Alternate baseline/candidate trials, retain raw samples, and use held-out workloads.

Preserve approved semantics for optimization-only tasks. Do not introduce floating-point contraction, unstable iteration, approximate GPU authoritative simulation, missed vector tails, or hidden quality reduction. BFME compatibility changes require separate evidence review.

A proposal is admissible only after reference/differential/replay tests and scope review. Report median/tail cost, memory/peak costs, startup/generation costs where relevant, and end-to-end improvement. An inconclusive timing difference is not a win. Do not change baselines, tolerances, seeds, or safety modes silently.

Submit the best evidenced candidate through the independent integrator. Keep the scalar/reference path. Document the specific workload range where the change helps and any regressions; do not claim universal supremacy from one benchmark.
