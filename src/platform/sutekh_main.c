/* sutekh executable: --version, and role words serve / worker.
 * M0 skeleton: serve and worker are not implemented yet and fail with a
 * structured SU_E_UNSUPPORTED_METHOD error rather than reporting success. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sutekh/abi.h"
#include "su_platform.h"

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

static int print_unsupported(const char *role) {
    /* Single-line JSON on stdout so the failure stays machine-parseable. */
    printf("{\"ok\":false,\"error\":{\"code\":\"%s\",\"status\":%u,"
           "\"message\":\"role '%s' is not implemented in the M0 skeleton\"}}\n",
           su_status_name(SU_E_UNSUPPORTED_METHOD),
           (unsigned)SU_E_UNSUPPORTED_METHOD, role);
    return 1;
}

static void print_usage(FILE *out) {
    fprintf(out, "usage: sutekh --version | serve | worker\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        print_usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--version") == 0) {
        return print_version();
    }
    if (strcmp(argv[1], "serve") == 0 || strcmp(argv[1], "worker") == 0) {
        return print_unsupported(argv[1]);
    }
    print_usage(stderr);
    return 2;
}
