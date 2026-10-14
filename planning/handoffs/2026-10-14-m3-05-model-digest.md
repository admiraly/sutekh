# M3-05 Q8 — independent model/engine digest comparison

The SnapshotModel adapter encodes the semantic Python model in engine snapshot 0.2's fixed key order. Only creation-time semantic manifest and acknowledged revision are pinned from the engine; every column, slot identity, event and count comes from the independent model. The standalone model replay format remains intact.

Exact implementation paths: `bench/rts/ref/model.py`, `bench/rts/replay/model_snapshot.py`, `bench/rts/replay/engine_backend.py`, `tests/rts/replay/test_model_snapshot.py`, `CMakeLists.txt`. Evidence: `planning/evidence/m3-model-digest/`.

Comparison found and fixed three reference-model errors against existing contract rules: reusable despawns incorrectly incremented retired count; spatial query wrote NONE into dead rows that must remain zero; truncated counts included rows killed at the tick boundary. Generation 0xffffffff is retired and never reusable. The harness stages initial formation references through set_fields after all anchors exist, preserving generator slot order and all 39 initial values without weakening Q2.

Checks: reference unittest 22/22; model replay unittest 26/26; seven locked committed scenes (16..65,536 rows) through four ticks; duel/mixed/congested small scenes through 64 ticks. Full snapshot SHA-256, canonical retained event SHA-256, and counts equal at every checked boundary. Event log cap 7 exercises eviction; duel and congested scenes exercise deaths. Generator hashes equal the existing lock; no golden expectations rewritten. Three reverted model defects independently detected. Source/artifact hashes and exact commands in result.json. New bounded CTest regression covers <=1024 rows through four ticks.

No native implementation changed; targeted native compilation is unnecessary. Independent reviewer, migration/schema history, native specialization, recovery at scale and performance qualification are not run in this slice. Snapshot encoding is 0.2 only. M3-05 remains implementing: near-million-row load/capture acceptance conflicts with existing high-water/spool/snapshot limits and needs honest resolution. Continue with that gate, then M3-06.

Integration: `cmake -S . -B build`; `ctest --test-dir build --output-on-failure -j 2`: 46/46 passed.
