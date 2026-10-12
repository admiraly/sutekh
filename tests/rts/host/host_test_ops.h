/* Test-only host operator descriptors (M3-04a, T-HOST tests). Not registered in the product build
 * (su_host_registry_find returns NULL for these names); the tests build them directly. */
#ifndef SU_TESTS_RTS_HOST_TEST_OPS_H
#define SU_TESTS_RTS_HOST_TEST_OPS_H

#include <stdint.h>

#include "runtime/schedule.h"

#define TEST_HOST_VEL_ID "test.host_vel_bump"
#define TEST_HOST_POS_ID "test.host_pos_writer"
#define TEST_HOST_TGT_ID "test.host_target_writer"
#define TEST_HOST_PROBE_ID "test.host_probe"

/* Counters and switches read and written by the test operators. */
typedef struct TestHostCtx {
    uint32_t calls;        /* completed invocations of run */
    uint32_t fail_at;      /* 0-based call index that fails; UINT32_MAX for none */
    uint32_t fail_status;  /* status returned by the failing call */
    uint32_t bad_views;    /* invocations whose views were not exactly the declared bindings */
    uint32_t dead_nonzero; /* probe: reads of rows >= TEST_ALIVE_ROWS that were nonzero */
} TestHostCtx;

/* Rows at and above this index are dead in the M3-04a test worlds (capacity above the alive count). */
#define TEST_ALIVE_ROWS 3u

/* test.host_vel_bump: declares velocity_x (id 2) read_write. For every row r, staging[r] = working[r] + 1
 * (whole column, as section 3.1 requires). */
extern SuHostOpDesc g_test_vel_bump;
/* test.host_pos_writer: declares position_x (id 1) write only. For every row, staging[r] = 0. */
extern SuHostOpDesc g_test_pos_writer;

/* test.host_target_writer: declares velocity_x (id 2) read_write; stores only row 0 (staging[0] = working[0] + 100).
 * Every other row must keep its working value through the commit (r4 A4, T-HOST-13 a). */
extern SuHostOpDesc g_test_target_writer;
/* test.host_probe: declares velocity_x read only; counts nonzero values of dead rows it is given. */
extern SuHostOpDesc g_test_probe;

/* Canonical constants text of the test operators (hashed at startup into the descriptors). */
void test_host_ops_init(void);

#endif
