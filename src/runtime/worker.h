/* Worker process entry (M0-05). POSIX only.
 *
 * A worker reads JSON-lines requests on in_fd and writes one response line per
 * request on out_fd until EOF. It speaks the protocol of docs/AGENT_RPC.md with
 * an internal request set (the supervisor is its only client):
 *   world.create              {fixture_path, capsule_path, source_path}
 *   world.inspect             {world_id, fields?, limit?}
 *   world.step                {world_id, expected_world_revision, ticks}
 *   internal.candidate.evaluate {capsule_path, source_path, test_path, world_path,
 *                                expected_source_hash, expected_capsule_hash}
 * The supervisor validates and resolves every path before sending it; the worker
 * trusts those paths and does not apply the path policy itself. */
#ifndef SU_RUNTIME_WORKER_H
#define SU_RUNTIME_WORKER_H

typedef void (*SuWorkerEvalHook)(void);

/* Serves until EOF on in_fd. Returns 0 on clean EOF, 1 on I/O failure. */
int su_worker_main(int in_fd, int out_fd);

/* As su_worker_main; before_eval (may be NULL) runs at the start of every
 * internal.candidate.evaluate request. Test helpers use it to inject faults. */
int su_worker_main_with_hook(int in_fd, int out_fd, SuWorkerEvalHook before_eval);

#endif
