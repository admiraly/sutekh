/* Supervisor side of the local control plane (docs/AGENT_RPC.md, M0 subset).
 * POSIX only; Windows is not implemented.
 *
 * su_agent_serve reads JSON-lines requests from in_fd and writes exactly one
 * response line per request to out_fd, until EOF. It opens no sockets: stdio or
 * any fd pair the caller provides is the whole transport.
 *
 * Implemented methods: engine.info, capsule.submit, candidate.status,
 * candidate.evaluate, world.create, world.inspect, world.step. Every other method
 * returns SU_E_UNSUPPORTED_METHOD. */
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
} SuAgentConfig;

/* Returns 0 on EOF, -1 on configuration or I/O failure before serving. */
int su_agent_serve(int in_fd, int out_fd, const SuAgentConfig *cfg);

#endif
