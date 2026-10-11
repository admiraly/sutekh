/* Test-only worker executable for the M1-02c-1 specialization admission tests. It is the production
 * worker entry (su_worker_main) compiled with SU_TEST_FAULT_HOOKS together with native_versions.c, so
 * the fault seams in worker.c and native_versions.c exist only in this binary. The production
 * `sutekh` binary is never built with that macro (CMakeLists.txt, SUTEKH_NATIVE block). */
#define _POSIX_C_SOURCE 200809L

#include "runtime/worker.h"

int main(void) {
    return su_worker_main(0, 1);
}
