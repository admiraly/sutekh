/* Supervisor-owned, content-addressed spool (M1-02 recovery).
 *
 * The supervisor keeps the bytes of every capsule, source module and canonical checkpoint it
 * depends on as files named <sha256>.<ext> inside one private directory. Recovery passes these
 * paths to a replacement worker together with the sha256 each file must still have, so a replay
 * never depends on project files that may have changed since they were captured.
 *
 * The spool knows nothing about references. The supervisor decides which entries are still
 * referenced and drops the others (su_spool_drop); su_spool_put refuses new bytes past the byte
 * budget so the caller can reclaim unreferenced entries first. POSIX only. */
#ifndef SU_AGENT_SPOOL_H
#define SU_AGENT_SPOOL_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/status.h"

#define SU_SPOOL_MAX_ENTRIES 512u
#define SU_SPOOL_EXT_MAX 12u

typedef struct SuSpool SuSpool;

/* dir NULL: a new directory is created with mkdtemp under $TMPDIR (or /tmp) and removed by
 * su_spool_close. A given dir must exist and is only emptied of spool files by su_spool_close.
 * max_bytes bounds the sum of entry sizes. */
uint32_t su_spool_open(const char *dir, uint64_t max_bytes, SuSpool **out);

/* Unlinks every entry; removes the directory if su_spool_open created it. NULL-safe. */
void su_spool_close(SuSpool *sp);

/* Stores data under its sha256 with extension ext (1..11 lowercase letters). Identical bytes
 * already present are not written again. sha_out receives 64 lowercase hex characters.
 * SU_E_RESOURCE_LIMIT when the entry would exceed max_bytes or the table is full;
 * SU_E_IO on file errors. */
uint32_t su_spool_put(SuSpool *sp, const char *ext, const uint8_t *data, size_t len, char sha_out[65]);

/* Moves a file the worker wrote (src) into the spool as <sha>.<ext>, after the caller has
 * checked its sha256 and byte count. If the entry already exists, src is removed instead. */
uint32_t su_spool_adopt(SuSpool *sp, const char *ext, const char *src, const char *sha, uint64_t bytes);

/* Full path of the entry for (sha, ext); SU_E_INVALID_ARGUMENT if it is not in the spool. */
uint32_t su_spool_path(const SuSpool *sp, const char *sha, const char *ext, char *out, size_t cap);

int su_spool_has(const SuSpool *sp, const char *sha, const char *ext);

/* Unlinks and forgets the entry. SU_OK if it was absent. */
uint32_t su_spool_drop(SuSpool *sp, const char *sha, const char *ext);

uint64_t su_spool_bytes(const SuSpool *sp);
uint64_t su_spool_max_bytes(const SuSpool *sp);
uint32_t su_spool_count(const SuSpool *sp);

/* Entry i (0 .. su_spool_count-1). Returns its sha (valid until the next mutation) or NULL. */
const char *su_spool_entry(const SuSpool *sp, uint32_t i, const char **ext, uint64_t *bytes);

/* Builds "<dir>/<sha>.<ext>"-style paths for a scratch file (no table entry). */
uint32_t su_spool_scratch_path(const SuSpool *sp, const char *tag, char *out, size_t cap);

#endif
