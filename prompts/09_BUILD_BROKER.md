# BUILD BROKER PROMPT — Remove author stalls without removing validation

Implement or operate the bounded validation/build lane assigned by the orchestrator. Read `docs/BUILD_AND_VALIDATION.md`, `AGENT_PROTOCOL.md`, and actual available toolchains. Do not build a distributed service platform for a local problem.

Use the existing native build graph and dependency files. Deduplicate jobs by exact source/dependency/toolchain/flag/target fingerprints. Compile cached third-party libraries once per fingerprint, not once per agent. Keep separate work/build directories for independent candidates.

Expose durable job IDs, status, immutable inputs, logs, exit codes, artifacts, cancellation and stale-result handling. A submitted job is not a passed job. Longer checks may continue while authors take independent tasks, but dependent acceptance waits for the result.

Set resource limits from observed memory and CPU contention. Do not give each worker all cores. Reserve resources for the running world and essential verification; speculative optimization is lower priority. Coalesce obsolete jobs and avoid repeated polling loops.

Preserve real compiler errors and structured summaries. Do not suppress diagnostics, bypass failed tests, or report success on missing tools. A two-second interactive budget is a scheduling target, not permission to kill validation or skip native compilation.

Use hosted CI for suitable broad correctness/platform gates, not the immediate gameplay loop. Respect actual concurrency/storage/billing limits and authorization. Do not enable paid runners or expose secrets to untrusted code. Fine-grained performance comparison requires controlled trusted hardware.

Deliver reproducible commands, evidence schemas, failure tests, and real warm/cold timing measurements. Verify that a capsule-only change never invokes the native build lane.
