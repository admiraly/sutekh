Negative fixtures for world.create's schedule checks (M1 closure B). Each directory holds copies
of tests/scheduler/fixtures/pipe (advance, damp, pipe_world) with only the schedule edges changed.

conflict/: pipe.advance and pipe.damp with no ordering edge. They conflict (read/write on
  position_x and velocity_x), so world.create refuses with SU_E_SCHEMA_MISMATCH.
cycle/: pipe.advance after pipe.damp and pipe.damp after pipe.advance. world.create refuses with
  SU_E_INVALID_ARGUMENT and the message "ordering cycle: ...".
order/: pipe.advance and pipe.damp with the edge kept (damp after advance), but listed damp first.
  The world runs its systems in capsule order, so world.create refuses with SU_E_INVALID_ARGUMENT
  ("runs against the capsule order").

Used by tests/harness/run_m0.py, test TX5.
