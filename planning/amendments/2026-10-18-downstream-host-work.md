# Downstream-driven host and platform work — owner-authorized amendment

Authorization, 18 October 2026: the owner authorized work driven by a downstream game outside the current
milestone order (M3, the RTS microcosm, remains in progress and no M3 gate is closed by this work). The downstream
workload is a single-player-first, later server-authoritative, fixed-step 6DOF racing simulation on Windows: a
120 Hz fixed simulation, a presentation host that renders with interpolation, per-tick canonical input records,
replays, state checksums, and snapshot/restore for prediction and reconciliation. This amendment records the scope;
it does not rewrite the root specifications, the IR contract or the milestone table.

## Scope, in the owner's priority order

1. Native Windows build and default test suite with Clang and the MSVC libraries, including the supervisor/worker
   control plane (`serve`, `worker`, process spawning, pipes, fault recovery in a replacement worker) ported through
   the platform layer with Win32 primitives. The POSIX path is kept and not weakened. Tests that are inherently
   POSIX are registered as explicitly skipped on Windows with the reason; none is reported as passed.
2. An in-process host embedding API: a small C ABI (`include/sutekh/host.h`, static library and DLL) that a game host
   links and drives from one thread without the supervisor: world create/destroy from a schema and capsule set,
   fixed tick rate, per-tick generic input records, stepping, zero-copy and copying reads, snapshot capture/restore
   to memory, a canonical state digest, structured error codes. The out-of-process control plane remains the
   agent/development path.
3. A fixed-step host clock helper (accumulator, ticks to run, interpolation alpha, spiral-of-death clamp).
4. Command-stream replay: header (engine and format versions, schema and capsule semantic hashes, numeric profile,
   seed, initial snapshot digest), per-tick input records and sparse keyframe snapshots, with digest-verified playback
   and seek by keyframe.

## Boundaries

- The IR/opcode contract (`LIVE_IR_SPEC.md`, the opcode and type tables of `include/sutekh/ir.h`), the verifier and
  VM semantics are owned by the parallel numeric/3D work and are not changed here except for minimal build fixes.
- Determinism claims are limited to what tests show: identical digests when the same runtime build re-simulates the
  same inputs. Cross-platform or cross-compiler bit identity is not claimed by this amendment.
- Timing figures recorded under this amendment are observations on the development machine, not qualification.
- Windows containment differs from Linux and is reported as such (see `src/agent/su_proc.h`): a job object (no process
  creation, commit limit, user-time limit, kill on supervisor exit, no crash dialogs) and a low-integrity token for
  read-only disposable workers; there is no network denial and no file-size or handle-count limit on Windows.
