<!-- SPDX-FileCopyrightText: 2026 admiraly -->
# Simulation runtime for the downstream 6DOF workload — owner-authorized amendment (extends 2026-10-18-numeric-3d)

Authorization, 18 October 2026: the owner extended the downstream amendments (`2026-10-18-downstream-host-work.md`,
`2026-10-18-numeric-3d.md`) to the simulation runtime the downstream 6DOF racing workload needs, outside the
milestone order. M3 (RTS microcosm) remains in progress; this amendment does not close, reorder or waive any M3
gate.

## Scope

1. Rollback resimulation budget: a benchmark that restores a saved state and re-steps K ticks (K = 1, 6, 12) at
   N = 8, 32, 256 racers on the racer microcosm, and semantics-preserving speed-ups of the reference execution path
   (a planned VM tier, exact fast numerics, identity-row skipping, reuse of per-call tick state, cheaper space3d
   query set-up), each proven bit-identical against the unoptimized reference.
2. The native tier (`SUTEKH_NATIVE=ON`) building and passing its tests on Windows x64, and lowering of IR 0.3
   operations with differential tests against the VM; operations that are not lowered keep the VM fallback.
3. Control-plane coverage of an IR 0.3 world with `space3d.query` through `sutekh serve`.
4. Schema-relative binding of `space3d` fields, verifier-checked, if it can be done cleanly.

## Constraints kept

- Additive. IR 0.1, 0.2 and 0.3 modules keep their semantic hashes; world storage, canonical state, snapshot
  formats and the control plane are unchanged unless a shared contract requires it, and each such change is
  recorded in the handoff.
- The reference interpreter and `src/ir/su_num.c` stay the readable reference behaviour. Every faster path is
  selectable off (`SUTEKH_VM_TIER=reference`, `SU_NUM_FAST_PORTABLE`) and differential-tested against it.
- Host event reading, in-place snapshot restore, the fast digest, the native input layer and the presenter belong
  to the presentation work stream and are not changed here.
- Evidence rules (C10, C20): timing figures are observations on the named machine, not qualification.
