/* M1 closure D: unit checks for two supervisor guards that the earlier suites did not exercise.
 *  - checkpoint digest comparison (su_agent_digest_fields_match): the revision, the tick and the snapshot SHA-256
 *    must each be compared (a restore with a wrong SHA, or with a different revision, must not match);
 *  - spool directory mode (su_spool_open): a group- or world-accessible directory is refused.
 *
 * Usage: test_hardening <scratch_dir>   (a writable directory outside the source tree) */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "agent/su_agent.h"
#include "agent/su_spool.h"
#include "sutekh/status.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static const char g_sha_a[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char g_sha_b[] = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

static void digest_cases(void) {
    /* every field matching: match */
    CHECK(su_agent_digest_fields_match("r7", "r7", 42u, 42u, g_sha_a, g_sha_a) == 1);
    /* SHA not requested (NULL): the other two decide */
    CHECK(su_agent_digest_fields_match("r7", "r7", 42u, 42u, g_sha_b, NULL) == 1);
    /* wrong SHA: no match (M-DIGEST-SHA) */
    CHECK(su_agent_digest_fields_match("r7", "r7", 42u, 42u, g_sha_b, g_sha_a) == 0);
    /* wrong revision: no match (M-DIGEST-REV) */
    CHECK(su_agent_digest_fields_match("r8", "r7", 42u, 42u, g_sha_a, g_sha_a) == 0);
    /* wrong tick: no match */
    CHECK(su_agent_digest_fields_match("r7", "r7", 43u, 42u, g_sha_a, g_sha_a) == 0);
    /* wrong revision with a SHA not requested: still no match */
    CHECK(su_agent_digest_fields_match("r8", "r7", 42u, 42u, g_sha_a, NULL) == 0);
}

/* A directory the spool must refuse: mode 0755 (world-readable). Mode 0700 must be accepted. */
static void spool_cases(const char *scratch) {
    char dir[512];
    SuSpool *sp = NULL;
    uint32_t st;
    snprintf(dir, sizeof dir, "%s/spool-loose", scratch);
    (void)rmdir(dir);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(chmod(dir, 0755) == 0);
    st = su_spool_open(dir, 1u << 20, &sp);
    CHECK(st == SU_E_CAPABILITY_DENIED); /* M-SPOOL-MODE: the mode check is what refuses it */
    CHECK(sp == NULL);
    if (sp != NULL) {
        su_spool_close(sp);
    }
    CHECK(chmod(dir, 0770) == 0);
    st = su_spool_open(dir, 1u << 20, &sp);
    CHECK(st == SU_E_CAPABILITY_DENIED); /* group access is refused too */
    CHECK(chmod(dir, 0700) == 0);
    st = su_spool_open(dir, 1u << 20, &sp);
    CHECK(st == SU_OK);
    if (sp != NULL) {
        su_spool_close(sp);
    }
    (void)rmdir(dir);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    digest_cases();
    spool_cases(argv[1]);
    printf("test_hardening: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
