/* Counting allocator seam for test_world_acct (M3-08a). Force-included only into the world sources
 * that the accounting test compiles itself (src/world/su_world.c, su_ents.c, su_cmdq.c). Production
 * builds do not use this header. The seam functions are defined in test_world_acct.c. */
#ifndef SUTEKH_TESTS_WORLD_ACCT_SEAM_H
#define SUTEKH_TESTS_WORLD_ACCT_SEAM_H

#include <stddef.h>
#include <stdlib.h>

void *su_seam_malloc(size_t n);
void *su_seam_calloc(size_t n, size_t size);
void su_seam_free(void *p);

#define malloc(n) su_seam_malloc(n)
#define calloc(n, size) su_seam_calloc((n), (size))
#define free(p) su_seam_free(p)

#endif
