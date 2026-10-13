# T-RTS-LOAD-1 test plan (M3-05)

Source: `planning/contracts/M3_RTS_CONTRACT.md`, section 10.3. Byte bound: 925 bytes per maximum row, 948,225 bytes for 1,024 rows. Request cap: 1,048,576 bytes, checked on the raw request line before row parsing.

| case | claim | where it runs now | status |
|---|---|---|---|
| (a) | accepted near-maximum 1,024-row request, one revision bump, one recovery entry, one `SU_EVENT_SPAWNED` per row | engine (native) | not run: needs the spawn_batch handler in `worker.c` and forwarding in `su_agent.c` |
| (b) | a padded 1,048,577-byte request is refused by the size check before row parsing | engine, `su_proto.c` | not run: size check exists on main (#21); needs the engine request path |
| (c) | exactly 1,048,576 bytes passes the size check | engine | not run, as (b) |
| (d) | 1,024 maximum-form rows are at most 948,225 bytes | Python (`tests/rts/replay/test_replay.py`, `test_worst_case_request_fits_the_cap`) | run, passes |
| (e) | 1,000,000-row load is 977 calls, last call short | Python loader (not written) | not run: the loader is outside this slice |
| (f) | high-water precheck at 838,860 | Python (`spawn_batch_limits.check_prechecks`) | run, passes; engine path not run |
| (g) | last row over free capacity refuses with no state change, no record, no revision bump; a domain-invalid last row refuses with `SU_E_TYPE` | Python model (`test_spawn_batch_is_atomic`, `test_spawn_batch_refuses_when_free_slots_are_short`) | run, passes; engine worker handler run (`tests/rts/control/test_world_spawn_batch.c`, passes); capacity and domain refusals through the supervisor run (`tests/recovery/test_recovery.c` `scenario_control_batches`, passes) |

Rules for the engine versions:
- Each engine case goes through the agent protocol (`su_agent.c`), not a direct worker call.
- A refusal leaves `world.inspect` state and the revision unchanged, checked by digest.
- The Python model and the engine must agree on each case before the engine result counts.
