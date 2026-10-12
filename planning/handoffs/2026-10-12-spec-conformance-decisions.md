# Handoff: spec-conformance owner items, decisions and queue

Date: 2026-10-12. Source: `planning/audit/spec-conformance-audit.md` sections 2 and 5, and
`planning/handoffs/2026-10-12-spec-conformance-fixes.md` (items 1-9 and proposals A1-A9).
Owner instruction: "you decide" on open owner items; prefer the conservative reading of the spec.
No normative document (spec pack, `LIVE_IR_SPEC.md`, `docs/*.md`, schemas) was edited here.
Amendment text below is proposed, not applied.

## State of the earlier handoff

Commits `58931f5` (audit 2), `41c68e5` (audit 4), `4172c40` (audit 6) and `16bf514` (audit 7) are ancestors of
`HEAD` on `claude/project-thread-azotaa`. Nothing from that handoff needed re-implementation.

## Decisions

| Item | Decision | Reasoning | Status |
|---|---|---|---|
| A1 capability grants | Keep reading (b): a capsule without `capabilities` is not checked. The spec's "absent means none" wording in the schema is not implemented for IR 0.2 capsules without the key. | Making the key mandatory changes the accept/refuse verdicts of about 40 existing fixtures and inline capsules. The owner asked that existing verdicts stay. The conservative choice is to keep behaviour and record the gap. | Behaviour unchanged. Proposed amendment: state the "not checked until declared" rule in LIVE_IR section 1 (owner to ratify). Migration of the fixtures is queued as its own change. |
| A2 capability vocabulary | Keep operation names (`despawn_self`, `emit_event`). | Implemented and tested (`tests/ir/test_ir.c` `test_caps`). Renaming to `rts.damage` would break the verified IR 0.2 surface. | Proposed amendment: M3 contract section 5.5 to name operation grants. Owner to ratify. |
| A3 `system_rev` | Keep it as declared metadata, not a checked value. Host records do not surface it. | Matches current behaviour, which is tested (`sys_a_rev`). Checking it against an engine counter is new semantics. | Host-record surfacing queued (touches `su_host_load`, a separate loader). Owner to confirm the metadata reading. |
| A4 request-id idempotence | Scope the rule to requests that carry an explicit client `idempotency_key`. Requests without one keep current behaviour. | The rule as written would break existing M0 clients that reuse ids (`tests/reload/test_reload.c`, `tests/harness/run_m0.py` with `q-1`, `x`). The conservative reading does not change them. | Implementation needs `src/agent/su_agent.c`, which another thread owns. Queued. |
| A5 `world.run` / `world.pause` | Keep `SU_E_UNSUPPORTED_METHOD`. Do not stub. Defer to a later gate that defines pacing. | A free-running loop needs a pacing policy the spec does not define. A stub would be a placeholder success, which AGENTS.md forbids. | Proposed amendment: move both to a later gate (owner to ratify). |
| A6 jobs | Record the M0 behaviour: `tests.run` and `job.status` complete synchronously; `candidate.evaluate` is synchronous. | Already implemented and tested (`test_jobs`). The asynchronous form is a later addition. | Proposed amendment to AGENT_RPC section 2 (owner to ratify). |
| A7 `replay.compare` | Not implemented in this change. Proposed definition: compare canonical state digests of two replay logs at the same tick, returning the first differing tick. | The comparison semantics are not defined in the spec, so any implementation would be an invented contract. Whether M2 closure needs it is an owner call; M2 is recorded as accepted, so this remains an open caveat. | Open. Owner to confirm the definition and whether it is a waiver. |
| A8 error detail | Accept the `error.detail` members as implemented (`json_pointer`, `source_path`, `source_sha256`, `capsule_sha256`, `system_id`, `expected`, `actual`, `hint`). | Already implemented additively; protocol 0.1 is additive. | Proposed amendment to AGENT_RPC section 5 (owner to ratify). |
| A9 status codes 18-21 | Keep as implemented. | They are needed by the implemented paths. | Proposed amendment to AGENT_RPC section 5 (owner to ratify). |

## Audit items resolved here

- Item 5 (status records): already consistent in `planning/tasks.json`. M1-00 to M1-05 read `integrated`. The
  `planning/status.json` `runtime_tests` line was stale and is updated to the runs in this handoff.
- Harness regression (not an audit item, found while checking the baseline): the M0 harness test TX9 expected the
  pre-OD-20 sizing, which applies the largest per-row cap to every system. Since `0dcc4c1` (M3-05 slice 1) each system
  is sized by its own cap, so the per-class totals changed. The engine is the authority here: `su_ent_budget` in
  `src/runtime/entity_tick.c` sizes each system by its own cap, and this matches the allocation in `su_cmdq_create_ex`.
  TX9 now uses the per-system arithmetic and the new boundary: 838,860 rows accepted (4,194,300 entries per class),
  838,861 refused (4,194,305), 1,000,000 refused (5,000,000). Harness self-test updated to match.

## Queued (not done here)

These touch files the coordinator reserved for other threads (`src/runtime/worker.c`, `src/agent/su_agent.c`), or need
owner approval.

1. A4 request-id idempotence, scoped to `idempotency_key`. Files: `src/agent/su_agent.c`.
2. Item 22: report `runtime_mode` as the spec mode name. Site: `src/agent/su_agent.c:2389`.
3. Audit 6 remainder: `error.detail` for `world.create`, `candidate.install` and migration errors. Needs a worker
   envelope change. Files: `src/runtime/worker.c`, `src/agent/su_agent.c`.
4. A3 host-record `system_rev` surfacing. Files: `src/runtime/rts/host_decl.c` and its loader.
5. A1 fixture migration (make `capabilities` mandatory). Owner approval first, since verdicts would change.
6. Item 26: regenerate `SHA256SUMS` with `tools/assemble_handoff.py`. Deferred so the regeneration is reviewed on its own.
7. Item 25: licence statement. `docs/BACKENDS_AND_DEPENDENCIES.md` section 7 says MIT; `planning/status.json` and
   `LICENSE` say PolyForm Perimeter 1.0.1. AGENTS.md forbids choosing a licence. Owner must confirm which text stands.
8. Items 13, 15, 16, 17, 19, 20, 21: waivers, scope or performance work, each needing an owner statement.
9. Item 3 and OD-5 (host operators and the C04 exception): owner confirmation in writing.

## Commands and results

| Check | Command | Result |
|---|---|---|
| Release build, `-Werror` | `cmake -S . -B <scratch> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror`, then build | clean, 126/126 |
| Release ctest | `ctest -j4` in scratch tree (before the harness edit; no C source changed) | 32/32 passed |
| M0 harness, release, before the TX9 fix | `run_m0.py --binary <release>/sutekh --no-build` | `passed=22, failed=1 (TX9), not_run=1, unavailable=3` |
| TX9 after the fix | `run_m0.py --binary <release>/sutekh --no-build --only TX9` | passed=1 |
| Harness self-test | `run_m0.py --self-test` | 260 checks, 0 failures |

Native lane (`-DSUTEKH_NATIVE=ON`) was not rebuilt for this change, because the only edit is in the Python harness.

## Unrun checks

- Full M0 harness after the TX9 fix: see the final run recorded in the PR description.
- Native lane: not run in this change.
- Sanitizers, dev profile, clang, Windows: not run (as in the earlier audit).
