/* Host operator registry (M3-04a, contract M3_RTS_CONTRACT 3.1 "Host registry", r4).
 * A fixed table of static descriptors. worker.c reads it through su_host_find and nothing else.
 * M3-04 adds its two production descriptors to host_registry.c; M3-04a registers none. */
#ifndef SU_RUNTIME_RTS_HOST_REGISTRY_H
#define SU_RUNTIME_RTS_HOST_REGISTRY_H

#include "runtime/schedule.h"

/* The registered descriptor for a host system ID, or NULL when none is registered. */
const SuHostOpDesc *su_host_find(const char *system_id);

#ifdef SU_TEST_HOST_REGISTRY
/* Test-only hook, compiled only into test translation units that define SU_TEST_HOST_REGISTRY. A NULL
 * list restores the production table. The list is NULL-terminated and borrowed. */
void su_host_registry_test_set(const SuHostOpDesc *const *list);
#endif

#endif
