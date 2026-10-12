/* Supervisor side of the local control plane (docs/AGENT_RPC.md, M0 subset plus M1-02 recovery).
 * POSIX only; Windows is not implemented.
 *
 * su_agent_serve reads JSON-lines requests from in_fd and writes exactly one
 * response line per request to out_fd, until EOF. It opens no sockets: stdio or
 * any fd pair the caller provides is the whole transport.
 *
 * Implemented methods: engine.info, capsule.submit, candidate.status,
 * candidate.evaluate, candidate.install, world.create, world.inspect, world.step,
 * world.snapshot, world.restore, world.close. Every other method returns
 * SU_E_UNSUPPORTED_METHOD.
 *
 * Recovery (M1-02): the supervisor holds, per world, its creation parameters, canonical
 * checkpoints and an ordered log of accepted mutating operations. When the active worker dies,
 * it starts a replacement and rebuilds every live world from those records; the request that
 * found the death fails with SU_E_WORKER_FAILED and a "recovery" object. */
#ifndef SU_AGENT_H
#define SU_AGENT_H

#include <stdint.h>

typedef struct SuAgentConfig {
    /* Required. Every client path must resolve under this directory. */
    const char *project_root;
    /* NULL-terminated argv for the worker executable (argv[0] contains '/').
     * It is exec'd directly, never through a shell. Example for the real binary:
     * {"/abs/path/sutekh", "worker", NULL}. NULL means no worker can be started. */
    const char *const *worker_argv;
    /* Per-request wall-clock limit for worker round trips. 0 selects 10000 ms. */
    uint32_t request_timeout_ms;
    /* RLIMIT_AS for worker children. 0 leaves it unchanged. Set 0 under ASan. */
    uint64_t worker_address_space_bytes;
    /* Containment (C07). Every child also gets network denial. 0 selects the default shown. */
    uint64_t worker_file_bytes;   /* RLIMIT_FSIZE of every child: 256 MiB */
    uint32_t worker_nofile;       /* RLIMIT_NOFILE of the active worker: 1024 */
    uint32_t disposable_nofile;   /* RLIMIT_NOFILE of disposable workers: 64 */

    /* Recovery limits (M1-02). 0 selects the default shown. */
    uint32_t checkpoint_interval_ticks; /* 64: a checkpoint once this many ticks were accepted */
    uint32_t checkpoint_retain;         /* 4 per world (max 8) */
    uint32_t log_max_entries;           /* 256 per world (max 1024); reaching it forces a checkpoint */
    uint32_t quarantine_after;          /* 2: identical crashes in a row before the revision is quarantined */
    uint64_t spool_bytes_max;           /* 32 MiB: capsule, source and checkpoint bytes held by the supervisor */
    /* Tick workers (M1 closure B): 1 (0 selects 1) runs every system serially, as before. N > 1 makes
     * the active worker run a system over a world of at least tick_chunk_min_rows rows in N chunked
     * workers (su_jobs_run_system_chunked). Range 1..64. Sent with every world.step. */
    uint32_t tick_workers;
    /* Row threshold for chunked execution. 0 selects 4096. */
    uint64_t tick_chunk_min_rows;
    /* Directory for the content-addressed spool. NULL: a private directory is created under
     * $TMPDIR (or /tmp) and removed at exit. A given directory must exist. */
    const char *spool_dir;
    /* 1 disables the checkpoint that is otherwise taken after every accepted install. Tests only:
     * it leaves installs in the replay log. */
    int no_install_checkpoint;
} SuAgentConfig;

/* Returns 0 on EOF, -1 on configuration or I/O failure before serving. */
int su_agent_serve(int in_fd, int out_fd, const SuAgentConfig *cfg);

#endif
