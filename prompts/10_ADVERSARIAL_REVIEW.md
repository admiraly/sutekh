# REVIEW PROMPT — Try to falsify the engine's claims

Review the exact candidate or milestone assigned to you. Read the constitution and the relevant technical contracts, then inspect actual code and run focused tests. This is not a style-only review.

Try to break the claims that a capsule edit requires no native project build, hot reload preserves the active worker and state, invalid candidates cannot affect committed state, numeric semantics match every admitted backend, retired code is not called, and parallel effects are truly independent.

Probe malformed/oversized IR, duplicate keys, unsupported opcodes, stale schemas, undeclared access, resource exhaustion, worker crashes, cancelled jobs, repeated swaps, vector tails, and binding alias/alignment errors. Check that system-level staging and full-tick publication do not leak partial writes.

Inspect evidence provenance: actual commands, source hashes, test corpus, benchmark modes and hardware. Check for test weakening, expected-output laundering, hidden skipped checks, fabricated counters, and synthetic BFME tests labeled as original fidelity.

Inspect trust boundaries: native code in the supervisor, unrestricted child environments, path traversal, shell interpolation, public RPC exposure, secrets on untrusted runners, or private assets in artifacts. A separate process is not automatically a complete hostile-code sandbox.

Rank findings by concrete impact and provide minimal reproducers, not vague objections. Distinguish observed defects, plausible risks, and checks you could not perform. Do not demand a total rewrite when a bounded fix suffices. Return the smallest repair tasks needed for the next genuine acceptance gate.
