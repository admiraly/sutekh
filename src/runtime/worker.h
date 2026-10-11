/* Worker process entry (M0-05, extended by M0-07). POSIX only.
 *
 * A worker reads JSON-lines requests on in_fd and writes one response line per
 * request on out_fd until EOF. It speaks the protocol of docs/AGENT_RPC.md with an
 * internal request set (the supervisor is its only client):
 *   world.create                {fixture_path, capsules:[{capsule_path, source_path}, ...]}
 *   world.inspect               {world_id, fields?, limit?, offset?}
 *   world.step                  {world_id, expected_world_revision, ticks}
 *   world.close                 {world_id, expected_world_revision}
 *   internal.candidate.evaluate {capsule_path, source_path, test_path, world_path,
 *                                expected_source_hash, expected_capsule_hash}
 *   internal.candidate.install  {world_id, candidate_id, capsule_path, source_path,
 *                                expected_source_hash, expected_capsule_hash, evidence,
 *                                expected_world_revision, allowed_effective_tick}
 * The supervisor validates and resolves every path before sending it; the worker
 * trusts those paths and does not apply the path policy itself. */
#ifndef SU_RUNTIME_WORKER_H
#define SU_RUNTIME_WORKER_H

typedef void (*SuWorkerEvalHook)(void);
/* Returns nonzero to fail the system with this index during a tick (test seam). */
typedef int (*SuWorkerSystemFault)(unsigned system_index);

typedef struct SuWorkerHooks {
    SuWorkerEvalHook before_eval;      /* may be NULL; runs at the start of each evaluation */
    SuWorkerSystemFault fail_system;   /* may be NULL; consulted before each system */
} SuWorkerHooks;

/* Serves until EOF on in_fd. Returns 0 on clean EOF, 1 on I/O failure. */
int su_worker_main(int in_fd, int out_fd);

/* As su_worker_main; before_eval (may be NULL) runs at the start of every
 * internal.candidate.evaluate request. Test helpers use it to inject faults. */
int su_worker_main_with_hook(int in_fd, int out_fd, SuWorkerEvalHook before_eval);

/* As su_worker_main_with_hook with every hook. hooks may be NULL. */
int su_worker_main_with_hooks(int in_fd, int out_fd, const SuWorkerHooks *hooks);

#endif
