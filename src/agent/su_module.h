/* Capsule metadata needed around su_lir_verify (which does the full check).
 * The capsule JSON is parsed only for the fields the control plane needs:
 * "source", "schema.version" and "schema.layout_generation". */
#ifndef SU_AGENT_MODULE_H
#define SU_AGENT_MODULE_H

#include <stddef.h>
#include <stdint.h>

#define SU_MODULE_NAME_MAX 255u

uint32_t su_module_capsule_meta(const uint8_t *cap, size_t len, char *source, size_t source_cap,
                                uint32_t *schema_version, uint32_t *layout_generation);

#endif
