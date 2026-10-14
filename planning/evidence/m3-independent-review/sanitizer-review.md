# Independent sanitizer follow-up: snapshot restore

Reviewer /root/review_limits_v2, 2026-10-14. Candidate sources unchanged. Separate build /tmp/sutekh-independent-asan-review, Debug, GCC cc, AddressSanitizer+UndefinedBehaviorSanitizer, frame pointers. Build parallelism1. Exact commands and outcomes in asan-build.log, asan-tests.log and oversize-guard.log.

CTest snapshot, host_restore, migration_model, migration and migration_runtime:5/5 passed, exit0,9.56seconds. ASAN_OPTIONS=detect_leaks=1:halt_on_error=1; UBSAN_OPTIONS=halt_on_error=1. SUTEKH_WORKER_AS_BYTES=0 accommodates ASan virtual shadow mapping; this is instrumentation evidence, not a 4GiB memory-bound experiment.

Standalone harness passes a single stack byte with claimed length SU_SNAPSHOT_MAX_BYTES+1 to allow_schema_change=1 restore. It returns SU_E_RESOURCE_LIMIT with null world, exit0, without sanitizer finding. This verifies the new guard precedes input traversal/allocation. Harness preserved as oversize_guard.c in evidence scope.

Initial build command named nonexistent test_migration target and failed; corrected actual targets test_migrate/test_migrate_runtime. Also initially supplied unused SUTEKH_ENABLE_NATIVE option; actual SUTEKH_NATIVE defaultOFF remained in force. Both observations retained in build log. No test assertion was weakened.

These bounded checks resolve recommended lifetime/early-guard coverage for this patch. They do not exercise every allocation-failure branch or establish million-row supervisor recovery/performance acceptance.

Reviewed snapshot.c SHA256: 5a97c4b1aaed96ba3dada5c5489d80cb35bee7ad8fdb50f5cb682b2944397e7b
Reviewed snapshot.h SHA256: 4ee3a11e81c69a3638ab90c87f6c065a25311ae293e94794510bc6cd8daff842
Harness SHA256: 8e22c0fdaa1120db05d840177c497e0611b5008821ae85e84b07e3a7ce25509d
