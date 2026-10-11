/* Path policy for the control plane (docs/AGENT_RPC.md section 6).
 *
 * Every client path is resolved against the canonical project root. A path is
 * accepted only if it names an existing regular file whose realpath lies under
 * that root. ".." components and symlinks that leave the root are refused with
 * SU_E_CAPABILITY_DENIED. */
#ifndef SU_AGENT_PATH_H
#define SU_AGENT_PATH_H

#include <stddef.h>
#include <stdint.h>

#define SU_PATH_BUF 4096u

/* Canonicalizes the project root with realpath. Returns SU_OK or SU_E_IO. */
uint32_t su_path_canonical_root(const char *root, char *out, size_t cap);

/* Resolves a client path (relative to root_real, or absolute) to a realpath
 * under root_real. SU_E_CAPABILITY_DENIED for "..", escapes and empty input;
 * SU_E_IO when the file does not exist or is not reachable; SU_E_RESOURCE_LIMIT
 * when the path is too long. */
uint32_t su_path_resolve(const char *root_real, const char *input, char *out, size_t cap);

/* Resolves a bare file name (no '/', not "." or "..") in the directory of an
 * already-resolved file, then applies su_path_resolve. */
uint32_t su_path_sibling(const char *root_real, const char *resolved_file, const char *name,
                         char *out, size_t cap);

/* Reads a regular file of at most max_bytes. On success *data is malloc'd and
 * *len is its size. Size is checked before allocation. */
uint32_t su_path_read_file(const char *path, size_t max_bytes, uint8_t **data, size_t *len);

#endif
