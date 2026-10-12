/* Host operator registry (M3-04a). See host_registry.h. */
#include "runtime/rts/host_registry.h"

#include <string.h>

/* Production table. Empty in M3-04a: no host operator is built into the engine until M3-04. */
static const SuHostOpDesc *const k_host_table[] = {NULL};

#ifdef SU_TEST_HOST_REGISTRY
static const SuHostOpDesc *const *g_test_table = NULL;
void su_host_registry_test_set(const SuHostOpDesc *const *list) {
    g_test_table = list;
}
#endif

const SuHostOpDesc *su_host_find(const char *system_id) {
    const SuHostOpDesc *const *list = k_host_table;
    size_t i;
    if (system_id == NULL) {
        return NULL;
    }
#ifdef SU_TEST_HOST_REGISTRY
    if (g_test_table != NULL) {
        list = g_test_table;
    }
#endif
    for (i = 0; list[i] != NULL; i++) {
        if (strcmp(list[i]->system_id, system_id) == 0) {
            return list[i];
        }
    }
    return NULL;
}
