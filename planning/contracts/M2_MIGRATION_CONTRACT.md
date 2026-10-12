# M2-01 contract draft r1: schema migrations (additive and explicit destructive)

Status: DRAFT r1 for independent review (contingent on M1 owner acceptance; additive, disabled by default until integrated). Builds on M2-00 (entities, snapshot 0.2, recovery log). Consumers: world migration engine, worker/control plane, supervisor recovery, snapshot, native invalidation, harness. A change after acceptance needs a new generation.

## 1. Principles
- A migration is a **transaction on one world**: validate -> plan (with resource admission) -> prepare new representation aside -> controlled commit at a quiescent boundary -> publish a new layout generation and world revision, or fail with the old world untouched (constitution C06, C09). It is never a code reload and its cost is reported separately.
- Field IDs are stable identity (names and order are not). The M0 **world schema** is the field table of the world's first installed capsule (M0_CONTRACT section 2); a migration edits that table and the column storage together.
- Noninvertible changes are not rolled back by running a guessed inverse. Rollback restores the **retained pre-migration checkpoint**.

## 2. Migration document (strict JSON, same strictness as IR: no duplicate keys, bounded size/depth)
```json
{ "migration_format": "0.1", "id": "m-001", "from_layout_generation": 1, "to_layout_generation": 2,
  "ops": [ {"op":"add_field","field":{"id":3,"name":"health","type":"u32","default":100,"read":true,"write":true}},
           {"op":"rename_field","id":1,"new_name":"x"},
           {"op":"remove_field","id":2},
           {"op":"transform","source":"transform.lir.json"} ] }
```
- Max 64 ops, 256 KiB. `from_layout_generation` must equal the world's current generation (else `SU_E_STALE_REVISION`); `to_layout_generation` must be exactly `from + 1` (`SU_E_TYPE`).
- `add_field`: new unique id (not currently used, not a previously removed id still referenced by retained checkpoints: ids are never reused within a world lineage), unique name, `type` `"u32"`, `default` u32 (required; there are no implicit defaults), `read`/`write` permission flags are **capsule** metadata only and are not stored in the world schema. New column filled with `default` for all capacity rows except dead entity slots (canonical zero).
- `rename_field`: same id, new unique name; only metadata changes; fixtures/IR referring to the old name fail verification afterwards (`SU_E_UNDECLARED_READ/WRITE`), by design.
- `remove_field`: rejected with `SU_E_UNSUPPORTED_MIGRATION` (reason `field_in_use`) if any installed capsule reads or writes the field; otherwise the column is dropped.
- `transform` (**explicit destructive path**, the only way to change values or retype): a SU-LIR 0.1 module whose loads name *source* (pre-migration) fields and whose stores name *destination* (post-migration) fields, verified against a two-schema contract: reads must exist in the old schema, writes must exist in the new schema (new or existing), each destination field written at most once; **destination fields not written by the transform keep their value (copy) if they existed before, or their `default` if added**. The transform runs through the existing VM over all alive rows into the **new** column storage (pure, total, bounded, deterministic; no entity ops). A transform may only be present when the migration also lists the schema edits it depends on; it is never optional magic.
- `from -> to` retype (`u32` -> `u32` with different meaning) is expressed as `transform` writing an existing id: the id is kept, so retyping semantics live in the transform and the migration record, and installed capsules reading that id are marked *needing review* (see section 5).

## 3. Plan and admission (`world.migrate_plan`)
`world.migrate_plan {world_id, expected_world_revision, migration_path | migration (inline, <= 256 KiB)}` -> `{plan_id, plan_hash, from_layout_generation, to_layout_generation, schema_before, schema_after, columns_added, columns_removed, columns_renamed, transform: {semantic_hash}|null, resource: {bytes_new_columns, bytes_peak, rows, checkpoint_bytes}, installed_systems: [{system_id, status: "compatible"|"needs_review"|"broken", reasons[]}], valid: bool}`.
- Reads only the project root (same path policy as other paths), captures migration bytes immutably, hashes them (`migration_hash` = SHA-256 of the bytes).
- Verifies everything in section 2, simulates the effect on every installed system's field table (compatible = all its fields still exist with same id/type; broken = uses a removed field; needs_review = reads a retyped/transformed id), computes memory need and checks the **resource budget** (new columns + staging + retained checkpoint + transform scratch) against configured limits (`SU_E_RESOURCE_LIMIT` with the exact numbers). A plan is immutable, bound to `(world_id, world_revision, from_layout_generation)`, and expires (stale) if the world revision changes.
- `valid:false` plans can still be inspected; committing one is refused.

## 4. Commit (`world.migrate_commit {world_id, plan_id, expected_world_revision, allow_needs_review?: bool}`)
1. Revalidate the plan against the current revision and layout generation (`SU_E_STALE_REVISION`); refuse when any installed system is `broken` (`SU_E_UNSUPPORTED_MIGRATION`) or `needs_review` without `allow_needs_review`.
2. **Pin a checkpoint** of the current world (snapshot 0.2 bytes, spool) as `pre_migration` before touching anything.
3. Prepare aside: allocate the complete new column storage (live columns are untouched); fill; run the transform over alive rows into it; run self-checks (row counts, entity table untouched, dead rows zero). Any failure, including allocation failure (injected in tests) -> discard the new storage; the old world is bit-identical and still active; error `SU_E_RESOURCE_LIMIT` / `SU_E_CANDIDATE_FAILED` with the failing op.
4. Commit at a tick boundary (never mid-tick): swap column storage and schema, set `layout_generation = to_layout_generation`, bump the world revision, rebind every installed system whose field table is still valid (semantic hash is unchanged by layout generation; the VM context and bindings are regenerated for the new layout), **retire every native artifact of the world** (execution tier falls back to `vm`; re-specialization goes through normal admission; artifacts lowered for the old layout can never run: constitution C06, M1 contract section 3), invalidate in-flight specializations, emit a `SU_EVENT_MIGRATED` event (a = from generation, b = to generation) into the world's event log.
5. Reply `{world_revision, layout_generation, migration_hash, timings: {plan_ns, prepare_ns, transform_ns, swap_ns, total_ns}, bytes: {new_columns, peak}, systems: [...], rollback_available: true, rollback_until_revision: ...}`. Migration timings are **never** merged into code-reload latency reports (TEST_SPEC / PERFORMANCE separation).

## 5. Installed capsules after a migration
Systems marked compatible keep running without resubmission. A *newly submitted* capsule must declare the world's **current** `layout_generation` (existing rule; a stale value is `SU_E_SCHEMA_MISMATCH`). Systems marked needs_review run (only with `allow_needs_review`), are flagged in `world.inspect`, and the flag clears only when a candidate for that system is installed.

## 6. Rollback (`world.migrate_rollback {world_id, expected_world_revision}`)
Restores the pinned pre-migration checkpoint, schema and layout generation included (a *new* world revision is assigned; `restored_from` records the migration), drops native artifacts, and re-installs the pre-migration systems from the content store. Available while the pinned checkpoint is retained (default: until the next migration commit or `world.close`; bounded by the spool budget; an evicted checkpoint -> `SU_E_STALE_REVISION` reason `rollback_unavailable`). State changes made after the migration are **lost on rollback** (it is a restore, not an undo) and the reply says so explicitly.

## 7. Snapshots, recovery, replay
- Snapshot 0.2 already carries the field table; restoring a snapshot whose schema differs from the world's is refused (`SU_E_SCHEMA_MISMATCH`) except through `world.migrate_rollback` (internal restore with schema). A new snapshot field `layout_generation` and `migration_lineage` (list of `migration_hash`) are added (snapshot 0.3 is NOT introduced: they go into the 0.2 `world` header as optional keys, absent = generation 1, empty lineage; the key order is fixed in the worked example fixture).
- `world.migrate_commit` and `world.migrate_rollback` are recovery-log entries: the supervisor stores the migration document bytes (content-addressed, bounded) and replays commit/rollback after checkpoint restore with a digest check; the checkpoint taken immediately **after** a commit is mandatory.
- Replay logs record the migration hash and the effective tick; a replay on a build that cannot reproduce the same schema fails closed.

## 8. Limits and errors
Rows <= `SU_LIMIT_ROWS`, ops <= 64, fields per schema <= `SU_LIMIT_FIELDS` (256), migration <= 256 KiB, transform <= 4,096 instructions. Errors reuse existing codes: `SU_E_PARSE/TYPE/DUPLICATE_KEY` (document), `SU_E_STALE_REVISION`, `SU_E_SCHEMA_MISMATCH` (generation/field mismatch), `SU_E_UNSUPPORTED_MIGRATION` (field_in_use, broken systems, unsupported op), `SU_E_RESOURCE_LIMIT`, `SU_E_CANDIDATE_FAILED` (transform failed/verification). Verifier stage order for the migration document follows M0_CONTRACT section 1 (limits, parse, duplicate keys, shape, schema semantics, ops in order).

## 9. Tests required for acceptance
Additive default fill (incl. dead entity slots zero), rename keeps id and rejects old-name capsules, remove rejected while used and allowed when unused, transform correctness vs independent Python model (wrap/overflow, copy semantics), memory failure injected at each allocation point leaving the world bit-identical and active, rollback restores schema+state exactly (and loses post-migration work, as documented), layout invalidation (native artifact retired, stale artifact refused after a migration, bindings regenerated, compatible systems keep running bit-identically to a control world, needs_review gating), plan staleness, resource-admission failure numbers, crash recovery during/after migration (kill before checkpoint, after commit before checkpoint: replay), snapshot with migration lineage round trip, large migration timing separated from reload timing (1,000,000 rows), mutation proofs for each.

## 10. Open points for the owner
Retyping beyond u32, background/online migration with write reconciliation (this draft always pauses at a tick boundary), multi-world migrations, destructive entity-table changes, schema downgrade tooling. Deferred.
