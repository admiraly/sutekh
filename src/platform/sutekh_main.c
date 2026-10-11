/* sutekh executable: --version, `serve [--root DIR]` (supervisor control plane
 * on stdio) and `worker` (internal worker role). POSIX only; on other
 * platforms serve/worker fail with a structured SU_E_UNSUPPORTED_METHOD error. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sutekh/abi.h"
#include "su_platform.h"
#if !defined(_WIN32)
#  include <limits.h>
#  include <unistd.h>
#  include "agent/su_agent.h"
#  include "runtime/worker.h"
#endif

#ifndef SUTEKH_VERSION_STRING
#  define SUTEKH_VERSION_STRING "unknown"
#endif
#ifndef SUTEKH_BUILD_TYPE
#  define SUTEKH_BUILD_TYPE "unspecified"
#endif
#ifndef SUTEKH_C_COMPILER_ID
#  define SUTEKH_C_COMPILER_ID "unknown"
#endif
#ifndef SUTEKH_C_COMPILER_VERSION
#  define SUTEKH_C_COMPILER_VERSION "unknown"
#endif

static int print_version(void) {
    printf("sutekh %s\n", SUTEKH_VERSION_STRING);
    printf("abi_version %u\n", (unsigned)SU_ABI_VERSION);
    printf("ir_version %s\n", SU_IR_VERSION_STRING);
    printf("artifact_format %u\n", (unsigned)SU_ARTIFACT_FORMAT_VERSION);
    printf("build_type %s\n", SUTEKH_BUILD_TYPE);
    printf("compiler %s %s\n", SUTEKH_C_COMPILER_ID, SUTEKH_C_COMPILER_VERSION);
    printf("pid %u\n", (unsigned)su_platform_pid());
    return 0;
}

#if defined(_WIN32)
static int print_unsupported(const char *role) {
    /* Single-line JSON on stdout so the failure stays machine-parseable. */
    printf("{\"ok\":false,\"error\":{\"code\":\"%s\",\"status\":%u,"
           "\"message\":\"role '%s' is not implemented in the M0 skeleton\"}}\n",
           su_status_name(SU_E_UNSUPPORTED_METHOD),
           (unsigned)SU_E_UNSUPPORTED_METHOD, role);
    return 1;
}
#endif

static void print_usage(FILE *out) {
    fprintf(out, "usage: sutekh --version | serve | worker\n");
}

#if !defined(_WIN32)
static int run_serve(int argc, char **argv) {
    const char *root = ".";
    if (argc == 4 && strcmp(argv[2], "--root") == 0) {
        root = argv[3];
    } else if (argc != 2) {
        print_usage(stderr);
        return 2;
    }
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) {
        fprintf(stderr, "sutekh: cannot resolve own executable path\n");
        return 1;
    }
    self[n] = '\0';
    const char *const worker_argv[] = {self, "worker", NULL};
    SuAgentConfig cfg = {0};
    cfg.project_root = root;
    cfg.worker_argv = worker_argv;
    return su_agent_serve(0, 1, &cfg) == 0 ? 0 : 1;
}
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--version") == 0 && argc == 2) {
        return print_version();
    }
#if !defined(_WIN32)
    if (strcmp(argv[1], "serve") == 0) {
        return run_serve(argc, argv);
    }
    if (strcmp(argv[1], "worker") == 0 && argc == 2) {
        return su_worker_main(0, 1) == 0 ? 0 : 1;
    }
#else
    if (strcmp(argv[1], "serve") == 0 || strcmp(argv[1], "worker") == 0) {
        return print_unsupported(argv[1]);
    }
#endif
    print_usage(stderr);
    return 2;
}
