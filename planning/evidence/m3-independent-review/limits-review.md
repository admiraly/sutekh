# Independent review: runtime limits v2

Reviewer: independent review_limits_v2 agent; implementation author: root integrator. Base: 5de97822056ee37b1411706bd75be6b2a0ff12d7. Audit performed 2026-10-14 against the source hashes below. Source was not modified. This review is limited to versioned limits and restore memory repair, not M3-wide acceptance or performance qualification.

## Result

No blocking correctness defect found in the reviewed changes. Conditional approval for the code-level changes, subject to full replacement-worker recovery and integrator dependent-suite verification on these exact sources. Owner authorization permits K and scale-limit changes; the separate amendment identifies superseded requirements rather than rewriting root specifications.

The snapshot repair owns a bounded copy of schema ID and field names before freeing the header tree. header_desc validates lengths and field count; parse_core and restore run while the copied descriptors remain alive. su_world_create deep-copies names, and su_json_doc_free nulls its arena/root, making final cleanup safe. The added early length/null checks also protect the schema-changing restore entry point. No aliases into the freed header document remain in the descriptor.

Exact admission uses high_water - alive - retired reusable holes and free_count excludes retired slots. The entity table's invariant places all allocated/retired slots below high_water; allocation fills lowest reusable slots. The formula therefore preserves exact growth and allows ceiling reuse without treating retired slots as reusable.

K160 is a semantic change explicitly versioned as rts_ops_v2. Production descriptors, capsule names/versions/constants hashes, model budget and model identity change together. Scatter receives a new version even though arithmetic is unchanged, coherently preventing cross-version manifest equivalence. Exact K/K+1 and clipping fixtures retain assertions; clipping density increases so the same defect still exceeds the new budget. Sampling uses all eligible queriers when fewer than 1024 exist, without replacement; unchanged committed scene/seed evidence is retained. These measurements establish the selected fixtures, not arbitrary-scene targeting guarantees.

The binary 60,000ms request timeout is bounded by the supported agent maximum; library defaults stay unchanged. It permits longer control-plane restore operations and does not establish a latency guarantee. The 512MiB snapshot cap does not prove all legal schemas fit the 4GiB worker limit.

## Checks independently run

- Release CTest snapshot, rts_replay: passed. An initial filtered migration run failed because migration_expected.json had not been generated; this was a test prerequisite selection error, not suppressed.
- Correct prerequisite-inclusive CTest selection: snapshot, host_restore, rts_native_ops, rts_native_records, snapshot_cp, world_ext_retired, world_restore_null, migration_model, migration, migration_runtime: 10/10 passed, exit 0 (1.19 seconds).
- Read direct restore log: 20 checks, zero failures, exit 0; 1M-row direct library restore and recapture, 4GiB address-space limit, peak RSS 2,456,420KiB, total command 9.84 seconds. This is implementation-authored evidence inspected by reviewer, not a newly run independent scale experiment.

## Findings and limitations

- Advisory (test coverage): add a bounded regression for the newly introduced early oversize guard in the allow_schema_change path, and an AddressSanitizer schema-change restore/migration test to protect copied-name lifetime. Existing snapshot/migration checks pass, but this review did not run instrumentation or allocation-failure injection for the new allocation.
- Acceptance blocker outside code review: the previous full supervisor 1M-row replacement-worker restore failed. The memory repair and timeout change require a new real RPC recovery run; direct library restoration is insufficient to close it.
- Performance review remains open: no independent M3-08 timings/stress curves were assessed here. The old mutation M3-M31 is superseded/redundant under the fixed 1M capacity and must not be reported as detected unchanged.
- Hashes below bind static review. Later edits require review of their delta and affected checks.

## Reviewed SHA-256 values

```json
{
  "bench/rts/ref/model.py": "09f50deff4a246f715d0ff26098d3cacde71ed4a9ca4f983e7731d3d7110108e",
  "bench/rts/replay/engine_backend.py": "6cc9737a0862884761b6d2076b834d253fed1a307619b448e4576e664e4ec9f0",
  "bench/rts/replay/rts_replay.py": "2a09f1983da363da3c88f171b313aaf4ba141a1ffe3b1a10ee25cdbfb69fa404",
  "bench/rts/replay/spawn_batch_limits.py": "3ee3d6f704bc85e3f465af25da4126a8004e6c473e8adb24bfe01f7167e5d1f4",
  "capsules/rts/host/scatter.capsule.json": "ae02432a5f048c2c9d41a9fe7342e3cd5f3579a39508c2e282171d2f9c71d003",
  "capsules/rts/host/spatial.capsule.json": "dd28d5623ad731fb897ac2daedd34ca5c216c72fd9d8a23b94254fd09bfee16e",
  "profiles/rts/native/rts_ops.h": "87bbd9713f16433b3396b0904557c3a22e1724230b7ca0dfbc297fec084723e5",
  "profiles/rts/native/rts_ops_common.c": "23cf9ee47f5f4c1f0acc7b5b67261f123b81265143af2efa07312ad96ba754ca",
  "profiles/rts/native/rts_scatter.c": "787d77c6dbf6e225c5711fffb0116ffb9d3ff4b54c1f55221a00da053ac100db",
  "profiles/rts/native/rts_spatial.c": "60d08baa0dc304ccb2a042fe56846b71c86479ffd862a4f5d08f3e29d16205a6",
  "schemas/capsule.schema.json": "f83ce55d0869f9af2f9a0022ef0e38c2b4c99d7001e78629dd148c6dcd4cfe59",
  "src/platform/sutekh_main.c": "a1526d622bd60aa33f30593294db405101fa6fb59dd11b052026b230e252cb8f",
  "src/runtime/rts/host_decl.c": "7cacd69373a5c4c373988870b7cdf88e828d86465abd098122851fb06389baec",
  "src/runtime/rts/host_decl.h": "8646920544e30d62015334ced1d1c3b20d2e9958091c862c86740a7622214904",
  "src/runtime/worker.c": "5c6c3e7a0e34191f00b8d0158e943bde62747b8ebe6bb73c1dd985336953d4e1",
  "src/world/snapshot.c": "5a97c4b1aaed96ba3dada5c5489d80cb35bee7ad8fdb50f5cb682b2944397e7b",
  "src/world/snapshot.h": "4ee3a11e81c69a3638ab90c87f6c065a25311ae293e94794510bc6cd8daff842",
  "tests/entities/test_rts_snapshot_limit.c": "33331533d410ddc9881cc56bde210e12920ad63d8e09f32ea948641d33e96ae4",
  "tests/rts/e2e/targeting_gates.py": "5df55b560b82b985b3f2dd8d9abd2a8b8160370fb16c0f65bd9ac7f5ecc743f6",
  "tests/rts/native/test_rts_ops.c": "da801081da971d326d6a74afce6d8c5b91c44d70789a65be1f07e179cbcdb476",
  "tests/rts/ref/checks.py": "f20000e6add1f2bdee2ee9d1f81e45cafb8bd0993c724bcaa3526d720b88cf4b",
  "tests/rts/replay/test_replay.py": "15a8824662c192f60ce406b9fe0567ee25748ba274c15f53d0bba41eb0c332cf",
  "profiles/rts/runtime_limits_v2.json": "6aecd14fda66178b6f7ab33695db0aa4384087d445c7db6edf6227fc2186b668",
  "planning/amendments/2026-10-14-m3-runtime-limits-v2.md": "7140240c32fc5d6851d119c57ce3e369c5a7fa67b0334fb0bca0f229ea40ac3d"
}
```
