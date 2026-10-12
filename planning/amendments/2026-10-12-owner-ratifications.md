# Owner ratifications of 2026-10-12

Statement: "accept all, keep going" (in reply to the spec-conformance and regression audit summaries).
Source of the items: planning/audit/spec-conformance-audit.md, planning/audit/regression-audit.md.

Ratified as amendments to the original specification:

1. In-place additions to normative documents made during M1-M3 are ratified as amendments:
   LIVE_IR_SPEC.md (IR 0.2 section), DATA_ABI (sections 6.1-6.2), AGENT_RPC (expanded method and containment sections),
   and other edits listed in the conformance audit section 3.10. The combined SUTEKH_FULL_SPEC.md has not been regenerated;
   SHA256SUMS differs from the edited files and records the original pack, not the ratified state.
2. OD-5: host operators (rts.spatial, rts.scatter) may have declared, tested, but not IR-verified effects. This is an explicit
   exception to C04, limited to registered host operators with descriptors checked at registration and an instrumented
   store-set test (M3 contract 4.4).
3. Technology choices accepted: in-house JSON parser (not yyjson), GCC on Linux, in-house SHA-256, AsmJit as the native
   backend fetched at bootstrap (not vendored).
4. Method renames accepted: schema.* became world.migrate_*, native.specialize became world.specialize.
   Missing spec-named methods are tracked by the conformance-fix task (implemented only where small and honest).

Not covered by this ratification: anything the audits list as a code defect; those are fixed in code or recorded as open.
