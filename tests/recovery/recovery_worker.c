/* Test-only worker executable for the M1-02 recovery tests. It is the production worker entry
 * (su_worker_main) compiled with SU_TEST_FAULT_HOOKS, which enables the crash seam described in
 * src/runtime/worker.c. The production `sutekh` binary is never built with that macro. */
#define _POSIX_C_SOURCE 200809L

#include "runtime/worker.h"

int main(void) {
    return su_worker_main(0, 1);
}
