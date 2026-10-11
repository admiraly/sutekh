/* M1-03 AVX2 lowering of SU-LIR (see su_native_avx2.cpp).
 *
 * Interface is the plain-data seam of backends/native/su_native_lower.h, the same as the scalar
 * lowering. Selected by su_native.c only when the caller requests SU_NCAP_X86_64_AVX2 and the host
 * reports AVX2 usable (su_native_host_caps). Returns SU_NLOW_RESOURCE when the frame would exceed
 * SU_NATIVE_AVX2_FRAME_LIMIT_BYTES; the caller then falls back to the scalar lowering. */
#ifndef SU_NATIVE_AVX2_H
#define SU_NATIVE_AVX2_H

#include <stddef.h>
#include <stdint.h>

#include "backends/native/su_native_lower.h"

/* Documented frame bound: 32 bytes per SU-LIR register plus 8 bytes per column pointer, rounded
 * up to 32. Above this the AVX2 lowering reports SU_NLOW_RESOURCE (-> SU_E_RESOURCE_LIMIT). */
#define SU_NATIVE_AVX2_FRAME_LIMIT_BYTES UINT32_C(65536)

#ifdef __cplusplus
extern "C" {
#endif

uint32_t su_native_lower_avx2(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
                              size_t msg_cap, uint32_t *failed_insn);

#ifdef __cplusplus
}
#endif

#endif
