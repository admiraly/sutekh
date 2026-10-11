/* Internal seams of the native backend. Not part of the public API.
 *  - su_native.c: C glue (compile orchestration, identity hash, checked invocation, exec mapping)
 *  - su_native_asmjit.cpp: AsmJit lowering, reached through su_native_lower.h only */
#ifndef SU_NATIVE_INTERNAL_H
#define SU_NATIVE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "backends/native/su_native.h"

struct SuNativeArtifact {
    SuNativeArtifactInfo info;
    const SuLirModule *module;     /* borrowed */
    uint64_t row_limit;
    uint8_t *code;                 /* R+X mapping of map_size bytes; code_size used */
    size_t code_size;
    size_t map_size;
    SuNativeKernelFn fn;
    SuNativeSourceRange *ranges;   /* insn_count entries */
    char *disasm;                  /* NUL-terminated */
    size_t disasm_len;
};

/* Executable mapping (su_native.c): RW, copy, then R+X. Never RWX. */
uint32_t su_native_exec_publish(const uint8_t *src, size_t size, uint8_t **code, size_t *map_size);
void su_native_exec_release(uint8_t *code, size_t map_size);

#endif
