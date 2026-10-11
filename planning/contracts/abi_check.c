/* Compile+run check for the M0-00 headers. Not an engine test; it only proves
 * the headers parse under strict C17 and the status table is consistent. */
#include <stdio.h>
#include <string.h>
#include "sutekh/abi.h"

int main(void) {
    for (uint32_t i = 0; i < SU_STATUS_COUNT; i++) {
        const char *n = su_status_name(i);
        if (strncmp(n, "SU_", 3) != 0 || strcmp(n, "SU_E_UNKNOWN") == 0) {
            fprintf(stderr, "bad name for %u\n", (unsigned)i);
            return 1;
        }
        for (uint32_t j = 0; j < i; j++)
            if (strcmp(n, su_status_name(j)) == 0) { fprintf(stderr, "dup %s\n", n); return 1; }
    }
    if (strcmp(su_status_name(SU_STATUS_COUNT), "SU_E_UNKNOWN") != 0) return 1;
    printf("abi_check ok: abi=%u statuses=%u\n", (unsigned)SU_ABI_VERSION, (unsigned)SU_STATUS_COUNT);
    return 0;
}
