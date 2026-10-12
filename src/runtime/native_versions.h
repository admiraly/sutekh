/* Native kernel artifact registry (M1-02b). Compiled only with SU_HAVE_NATIVE (CMake option
 * SUTEKH_NATIVE). The reference VM stays the semantic oracle and the default tier.
 *
 * Ownership: every compiled kernel lives in a process-global registry of at most
 * SU_NV_MAX_ARTIFACTS live artifacts. Callers hold a handle (`seq`, a monotonically increasing
 * number), never a pointer. A handle whose artifact is gone resolves to nothing, so a retired
 * kernel cannot be reached through this API. A schedule entry stores its handle
 * (SuSysEntry.native_seq, 0 = VM tier).
 *
 * Lifetime: PREPARED -> ADMITTED -> INSTALLED -> RETIRING -> freed. su_nv_release detaches the
 * handle at once; the code is unmapped when no kernel call is in flight (in-flight counter,
 * incremented around every call). The M0 worker is serial, so the counter is zero at every tick
 * boundary and retirement completes there; the counter keeps the rule if calls ever overlap.
 *
 * Thread-safety: single-threaded only (the worker handles one request at a time). No locks. */
#ifndef SU_RUNTIME_NATIVE_VERSIONS_H
#define SU_RUNTIME_NATIVE_VERSIONS_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_lir.h"
#include "runtime/jobs/su_jobs.h"
#include "world/su_world.h"

#define SU_NV_MAX_ARTIFACTS 32u

enum {
    SU_NV_STATE_NONE = 0,      /* handle unknown or retired */
    SU_NV_STATE_PREPARED = 1,  /* compiled, not admitted */
    SU_NV_STATE_ADMITTED = 2,  /* differential admission passed */
    SU_NV_STATE_INSTALLED = 3, /* reachable from a schedule entry */
    SU_NV_STATE_RETIRING = 4   /* detached; waits for in-flight calls */
};

/* Compiles m with the SU_NCAP_* set caps. *seq receives the handle. On failure nothing is
 * registered and msg holds the reason. m is borrowed and must outlive the artifact. */
uint32_t su_nv_prepare(const SuLirModule *m, uint32_t layout_generation, uint32_t caps,
                       uint64_t *seq, char *msg, size_t cap);

/* Differential admission against the reference VM over a seeded corpus: row counts
 * 0,1,7,8,9,17,1024; aligned and one-element-unaligned columns; edge values and seeded random
 * words. SU_OK with ADMITTED state when every word matches, SU_E_CANDIDATE_FAILED otherwise. */
uint32_t su_nv_admit(uint64_t seq, uint32_t seed, uint32_t *cases, uint64_t *rows_checked,
                     uint64_t *mismatches);

/* ADMITTED -> INSTALLED. SU_E_INTERNAL for other states or unknown handles. */
uint32_t su_nv_install(uint64_t seq);

/* Runs the installed kernel on the world's open tick (binding selection and commit as
 * su_vm_run_system). SU_E_INTERNAL when seq does not name an installed artifact. */
uint32_t su_nv_run_system(uint64_t seq, SuWorld *w);

/* As su_nv_run_system, over the canonical chunks of the world's rows (su_jobs boundaries, which depend
 * only on the row count) with `workers` threads. Every chunk runs the same kernel on sliced column
 * views; the commit happens once, after all chunks succeed. Lowest failing chunk decides the status.
 * `fault` is the test seam of su_jobs (NULL outside tests). workers == 1 is su_nv_run_system. */
uint32_t su_nv_run_system_chunked(uint64_t seq, SuWorld *w, uint32_t workers, SuJobsFault fault, void *fault_ctx);

/* Detaches the handle. Frees now when no call is in flight, else marks RETIRING and frees
 * when the last call returns. Unknown or already retired handles are a no-op. */
void su_nv_release(uint64_t seq);

/* First mismatch recorded by the last su_nv_admit of this artifact (valid = 0 when none). */
typedef struct SuNvMismatch {
    uint32_t valid;
    uint32_t case_index;  /* corpus case number, admission order */
    uint32_t case_rows;   /* rows of that case */
    uint32_t case_offset; /* 0 aligned, 1 one element into the column */
    uint32_t field_id;
    uint64_t row;
    uint32_t expected;    /* VM word */
    uint32_t actual;      /* kernel word (or the status when st_vm != st_nat) */
} SuNvMismatch;
uint32_t su_nv_first_mismatch(uint64_t seq, SuNvMismatch *out);

/* PREPARED -> ADMITTED without running the kernel, when the artifact identity equals the hash a
 * disposable worker admitted. The active worker calls this after recompiling (compilation only).
 * SU_E_CANDIDATE_FAILED on a hash mismatch (state unchanged). */
uint32_t su_nv_trust_admitted(uint64_t seq, const char *admitted_hash);

/* Test-only fault seams. Outside SU_TEST_FAULT_HOOKS builds this is a no-op and no fault code is
 * compiled. delay_ms applies to SU_NV_FAULT_DELAY. */
enum {
    SU_NV_FAULT_NONE = 0,
    SU_NV_FAULT_WRONG_KERNEL = 1, /* flips one output bit in the corpus and in world runs */
    SU_NV_FAULT_WRONG_WORLD = 2,  /* flips one output bit in world runs only (own test cases) */
    SU_NV_FAULT_CRASH = 3,        /* SIGSEGV before the kernel call */
    SU_NV_FAULT_HANG = 4,         /* never returns from the kernel call */
    SU_NV_FAULT_DELAY = 5,        /* sleeps delay_ms before the kernel call, then runs it */
    SU_NV_FAULT_RELEASE_IN_CALL = 6 /* one shot, world path: su_nv_release of the running artifact from
                                       inside its kernel call; see su_nv_test_release_observation */
};
void su_nv_test_arm(uint32_t mode, uint32_t delay_ms);

/* Test-only (M1-02c-2): the worker dies with SIGSEGV inside an installed kernel's run when the tick
 * being computed is `tick` (0 = off) and the backend id matches `backend_id` (0 = any). No-op outside
 * SU_TEST_FAULT_HOOKS builds. */
void su_nv_test_arm_native_crash(uint64_t tick, uint32_t backend_id);

/* Introspection (tests, engine.info). */
uint32_t su_nv_state(uint64_t seq);
uint32_t su_nv_live_count(void);
uint32_t su_nv_inflight(uint64_t seq);
uint64_t su_nv_released_count(void);
/* Lower-case hex identity, 64 chars + NUL. SU_E_INTERNAL for unknown handles. */
uint32_t su_nv_artifact_hash(uint64_t seq, char out[65]);
uint32_t su_nv_backend_id(uint64_t seq, uint32_t *out);
/* Compiled code size, for mapping hygiene tests. */
uint32_t su_nv_code_bytes(uint64_t seq, uint64_t *out);

/* Test-only: holds an in-flight count so that a release can be observed deferring. */
void su_nv_test_enter(uint64_t seq);
void su_nv_test_leave(uint64_t seq);
/* Test-only: the state, in-flight count and live-artifact count observed right after the
 * SU_NV_FAULT_RELEASE_IN_CALL release (fired = 0 when the seam did not run). */
void su_nv_test_release_observation(uint32_t *fired, uint32_t *state, uint32_t *inflight, uint32_t *live);

#endif
