# T-RTS-LOAD-1 test plan (M3-05)

Source: `planning/contracts/M3_RTS_CONTRACT.md`, section 10.3. Byte bound: 925 bytes per maximum row, 948,225 bytes for 1,024 rows. Request cap: 1,048,576 bytes, checked on the raw request line before row parsing.

Engine test: `tests/rts/load/test_load_engine.py` (ctest `rts_load_engine`), a real `sutekh serve` with the full six-system RTS world built by `world.create` (`bench/rts/replay/rts_session.py`).

| case | claim | where it runs now | status |
|---|---|---|---|
| (a) | accepted near-maximum 1,024-row request, one revision bump, one recovery entry, one `SU_EVENT_SPAWNED` per row | engine test | run, passes; the recovery entry is checked by SIGKILL and digest and event-log equality with a control session |
| (b) | a padded 1,048,577-byte request is refused by the size check before row parsing | engine test | run, passes (`SU_E_RESOURCE_LIMIT`, `world.inspect` unchanged) |
| (c) | exactly 1,048,576 bytes passes the size check | engine test | run, passes (a valid request is accepted; a bad last row is `SU_E_TYPE`, not a size code) |
| (d) | 1,024 maximum-form rows are at most 948,225 bytes | Python (`tests/rts/replay/test_replay.py`, `test_worst_case_request_fits_the_cap`); engine test measures the real request | run, passes |
| (e) | 1,000,000-row load is 977 calls, last call short | Python loader (not written) | not run: the loader is outside this slice |
| (f) | high-water precheck at 838,860 | Python (`spawn_batch_limits.check_prechecks`); worker `spawn_precheck` | Python run, passes; engine path not run (needs a world above 838,860 rows) |
| (g) | last row over free capacity refuses with no state change, no record, no revision bump; a domain-invalid last row refuses with `SU_E_TYPE` | Python model (`test_spawn_batch_is_atomic`, `test_spawn_batch_refuses_when_free_slots_are_short`); worker (`tests/rts/control/test_world_spawn_batch.c`); supervisor (`test_recovery.c` `scenario_control_batches`, engine test) | run, passes |

Rules for the engine versions:
- Each engine case goes through the agent protocol (`su_agent.c`), not a direct worker call.
- A refusal leaves `world.inspect` state and the revision unchanged, checked by digest.
- The Python model and the engine must agree on each case before the engine result counts.
