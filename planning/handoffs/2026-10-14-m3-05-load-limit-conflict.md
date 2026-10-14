# M3-05 — load/capture acceptance conflicts with retained limits

T-RTS-LOAD-1(e) asks for 1,000,000 accepted rows while (f) and the production spawn guard refuse high_water >838,860. Default 512 MiB spool admission binds even earlier at 588,630. These are incompatible success conditions without a contract decision; no production bound is raised here.

The (f) successful capture at 838,860 is independently impossible under snapshot 0.2's 64 MiB limit. Minimum row payload, ignoring all headers/events and allowing one-digit zero values, is 39*(2*N-1) column bytes + (2*N-1) entity-ID bytes + (8*N-1) slot bytes = 88*N-41. At N=838,860 this is 73,819,639 bytes >67,108,864. Real entity IDs and legal RTS sentinels only increase it.

New `tests/entities/test_rts_snapshot_limit.c` reproduces the existing snapshot refusal on a structurally valid 39-field world with 838,860 alive rows and zero columns, and verifies a 65,536-row control captures successfully (6,271,779 bytes). It has 11 checks/0 failures. This test preserves current policy; a passing refusal regression does **not** close the requested scale-success gates. CMake registers it serially. Native test compiled with GCC15.2 Debug; evidence and source/artifact hashes in `planning/evidence/m3-load-limits/`.

Modified implementation paths: `tests/entities/test_rts_snapshot_limit.c`, `CMakeLists.txt`. No architecture/specification or production code changed. Independent review, sanitizers and native Windows unrun. Million-row supervisor load and capture-at-838860 remain unachieved. OD-15 remains unchanged, recovery claim N<=65,536.

Resolution requires the snapshot/control owners to reconcile (e)(f) with the accepted limits, or explicitly authorize a scoped larger/binary snapshot design. Record this as a gate conflict rather than fabricate success. Continue independent M3-06 fixtures, targeting metrics and mutation checks while the gate remains open; do not declare M3-05 or M3 complete.
