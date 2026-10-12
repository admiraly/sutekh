/* Bounded structural-command and event buffers, canonical merge, apply driver and bounded
 * event log (M2-00b, contract r2 sections 2 and 4). Record constants come from sutekh/entities.h
 * (SU_EVENT_REJECTED, SU_EVENT_DESPAWNED, SU_SYSTEM_INDEX_HOST).
 *
 * Writers: one (system_index, chunk) buffer is written by exactly one writer at a time. Buffers are
 * disjoint, so appends from different buffers need no locks. Shared state is three atomic per-tick
 * counters, a fill-rank counter and the sticky overflow flag.
 *
 * Storage chunks: slots are split into chunks of chunk_slots (last chunk may be short). Buffer
 * (system, chunk) holds min(chunk_size * per_row_cap, per_tick_cap) entries, all allocated at
 * create. A row is a run of appends with the same slot in one buffer; per-row caps reset when the
 * slot changes.
 *
 * Overflow: an append that exceeds a per-row cap, a buffer capacity or a per-tick cap writes
 * nothing, returns SU_E_RESOURCE_LIMIT and sets the sticky flag. The flag depends only on the
 * multiset of attempted appends, not on their interleaving. Commit refuses when it is set.
 *
 * Records: every structural command yields exactly one record at commit, either SU_EVENT_DESPAWNED
 * (success) or SU_EVENT_REJECTED (refused). Each record consumes an event_seq and counts toward
 * the per-tick event cap together with the system-emitted events. The cap is checked before any
 * callback runs, so an over-cap tick changes nothing.
 *
 * Canonical order: (system_index, slot, seq) lexicographic. Merge verifies that every buffer is
 * strictly ordered by (slot, seq); if not, it stable-sorts the concatenation in (system, chunk,
 * emission) order and reports the fallback. Output never depends on worker count or completion
 * order.
 *
 * Status codes: SU_OK, SU_E_INVALID_ARGUMENT, SU_E_RESOURCE_LIMIT (overflow, cap or allocation),
 * SU_E_PARSE (corrupt dump), SU_E_INTERNAL (invariant violated). */
#ifndef SUTEKH_WORLD_ENTITIES_SU_CMDQ_H
#define SUTEKH_WORLD_ENTITIES_SU_CMDQ_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/entities.h"
#include "sutekh/status.h"

/* Record kinds from contract r2 (include/sutekh/entities.h on main). This branch's entities.h
 * predates r2, so the r2 values are defined here only when absent. Values must stay identical. */
#ifndef SU_EVENT_REJECTED
#define SU_EVENT_REJECTED   UINT32_C(0xFFFFFFFF)
#endif
#ifndef SU_EVENT_DESPAWNED
#define SU_EVENT_DESPAWNED  UINT32_C(0xFFFFFFFD)
#endif

/* Creation-time defaults (contract r2 section 1 and 2). */
#define SU_CMDQ_DEFAULT_PER_ROW_CAP        UINT32_C(1)
#define SU_CMDQ_DEFAULT_PER_TICK_CAP       UINT64_C(4096)
#define SU_CMDQ_DEFAULT_EVENT_LOG_MAX      UINT64_C(65536)

#define SU_CMDQ_MAX_BUFFERS       UINT64_C(1048576)   /* nsystems * nchunks */
#define SU_CMDQ_MAX_ENTRIES       UINT64_C(4194304)   /* per storage class: buffers and per-tick */
#define SU_CMDQ_MAX_TICK_CAP      UINT64_C(1048576)
#define SU_CMDQ_MAX_LOG_CAPACITY  UINT64_C(1048576)
#define SU_CMDQ_LOG_HEADER_BYTES  UINT64_C(44)
#define SU_CMDQ_LOG_ENTRY_BYTES   UINT64_C(40)
#define SU_CMDQ_LOG_VERSION       UINT32_C(1)

typedef struct SuCmdQ SuCmdQ;
typedef struct SuEventLog SuEventLog;

/* Host callback used by the apply driver. On success *old_gen receives the generation the slot had
 * before the despawn. A non-OK status rejects the command. */
typedef struct SuCmdCallbacks {
    void *ctx;
    uint32_t (*despawn)(void *ctx, uint64_t handle, uint32_t *old_gen);
} SuCmdCallbacks;

/* Merged view owned by the queue. Valid until the next merge, commit or reset. System events have
 * tick and event_seq unset (0) until commit. */
typedef struct SuCmdView {
    const SuStructCmd *cmds;
    uint64_t n_cmds;
    const SuEvent *events;
    uint64_t n_events;
    uint32_t cmd_fallback;          /* 1: some command buffer was out of order (stable sort used) */
    uint32_t event_fallback;        /* 1: some event buffer was out of order (stable sort used) */
    uint64_t cmd_unsorted_buffers;
    uint64_t event_unsorted_buffers;
} SuCmdView;

typedef struct SuCommitReport {
    uint64_t n_cmds;
    uint64_t n_despawned;
    uint64_t n_rejected;
    uint64_t n_events;              /* system events plus one record per command, all logged */
    uint64_t first_event_seq;
    uint32_t cmd_fallback;
    uint32_t event_fallback;
} SuCommitReport;

/* nslots: total slots (capacity). chunk_slots: slots per storage chunk (SU_CHUNK_SLOTS in the
 * engine). A slot belongs to chunk slot / chunk_slots. per_tick_*_cap >= 1. */
uint32_t su_cmdq_create(uint32_t nsystems, uint64_t nslots, uint64_t chunk_slots,
                        uint32_t per_row_struct_cap, uint32_t per_row_event_cap,
                        uint64_t per_tick_struct_cap, uint64_t per_tick_event_cap, SuCmdQ **out);
void su_cmdq_destroy(SuCmdQ *q);
uint64_t su_cmdq_chunk_count(const SuCmdQ *q);
/* Clears counts and the overflow flag. Allocates nothing. */
void su_cmdq_reset(SuCmdQ *q);

/* Appends for one (system, chunk) writer. slot must lie in chunk (slot / chunk_slots == chunk and
 * slot < nslots). seq is the VM-supplied per-row sequence (contract r2 section 2): ONE counter per
 * row shared by despawn and event records, numbered in instruction order from 0. Within a buffer,
 * seq must strictly increase for the current row, else SU_E_INVALID_ARGUMENT (nothing changes).
 * The per-row caps are still counted per record kind. Rows should be visited in ascending slot
 * order. Out-of-order input is accepted and detected at merge. */
uint32_t su_cmdq_append_struct(SuCmdQ *q, uint32_t system_index, uint64_t chunk, uint32_t slot,
                               uint32_t seq, uint32_t kind, uint64_t arg, uint32_t init0,
                               uint32_t init1);
uint32_t su_cmdq_append_event(SuCmdQ *q, uint32_t system_index, uint64_t chunk, uint32_t slot,
                              uint32_t seq, uint32_t kind, uint32_t a, uint32_t b);

int su_cmdq_overflowed(SuCmdQ *q);
/* Stored record counts for the current tick, summed over buffers. Call only when writers are
 * quiescent. */
void su_cmdq_counts(SuCmdQ *q, uint64_t *n_struct, uint64_t *n_event);

/* Canonical merge. SU_E_RESOURCE_LIMIT when overflowed. */
uint32_t su_cmdq_merge(SuCmdQ *q, SuCmdView *view);

/* Apply driver. Calls cb->despawn in array order and writes exactly one record per command at
 * rec[i]: SU_EVENT_DESPAWNED (a = slot, b = old generation) on success, SU_EVENT_REJECTED (a = the
 * SU_STRUCT_* kind, b = status) otherwise. Unknown or reserved kinds are rejected with
 * SU_E_INVALID_ARGUMENT. The driver never fails because of a callback status. Before any call it
 * returns SU_E_INVALID_ARGUMENT if cb->despawn is NULL and a despawn command exists, or
 * SU_E_RESOURCE_LIMIT if rec_cap < n. Records carry the command's system_index, slot and seq. */
uint32_t su_cmdq_apply(const SuStructCmd *cmds, uint64_t n, const SuCmdCallbacks *cb, uint64_t tick,
                       SuEvent *rec, uint64_t rec_cap);

/* Merge, check the per-tick event cap (system events + one record per command must be at most
 * per_tick_event_cap), apply, combine with system events by (system, slot, seq, source) with system
 * events first on equal keys, assign tick and event_seq from the log's next sequence and append.
 * Failures before apply (SU_E_RESOURCE_LIMIT from overflow or the event cap, SU_E_INVALID_ARGUMENT)
 * run no callback and leave the log unchanged. */
uint32_t su_cmdq_commit(SuCmdQ *q, const SuCmdCallbacks *cb, uint64_t tick, SuEventLog *log,
                        SuCommitReport *rep);

/* ---- bounded event log (ring) ---- */

/* capacity in [1, SU_CMDQ_MAX_LOG_CAPACITY]. base_seq is the event_seq of the first event ever
 * appended. */
uint32_t su_evlog_create(uint64_t capacity, uint64_t base_seq, SuEventLog **out);
void su_evlog_destroy(SuEventLog *log);
/* Events must have event_seq == next_seq + i, otherwise SU_E_INVALID_ARGUMENT and nothing is
 * appended. When full, the OLDEST retained event is evicted and dropped is incremented. */
uint32_t su_evlog_append(SuEventLog *log, const SuEvent *ev, uint64_t n);
uint64_t su_evlog_capacity(const SuEventLog *log);
uint64_t su_evlog_count(const SuEventLog *log);
uint64_t su_evlog_dropped(const SuEventLog *log);
uint64_t su_evlog_oldest_seq(const SuEventLog *log);
uint64_t su_evlog_next_seq(const SuEventLog *log);
/* Rebuilds a log from snapshot state: capacity, next_seq, dropped and the retained events (oldest
 * first, contiguous, ending at next_seq - 1). SU_E_INVALID_ARGUMENT for any inconsistent state. */
uint32_t su_evlog_restore(uint64_t capacity, uint64_t next_seq, uint64_t dropped, const SuEvent *ev,
                          uint64_t n, SuEventLog **out);
/* Copies the retained events oldest first. cap must be at least su_evlog_count. */
uint32_t su_evlog_copy_retained(const SuEventLog *log, SuEvent *out, uint64_t cap, uint64_t *n);
/* events.read semantics (contract r2 section 2). Returns events with seq >= cursor, at most max
 * (out must hold max entries). If cursor is below the oldest retained seq, reading starts at the
 * oldest retained record and *skipped is the number of events lost before it; otherwise *skipped
 * is 0. *dropped is the ring's dropped counter. next_cursor is the seq after the last returned
 * event (the start when none). cursor > next_seq is SU_E_INVALID_ARGUMENT. */
uint32_t su_evlog_read(const SuEventLog *log, uint64_t cursor, uint64_t max, SuEvent *out,
                       uint64_t *n_out, uint64_t *next_cursor, uint64_t *skipped, uint64_t *dropped);
/* Canonical little-endian dump: 44-byte header, then 40 bytes per retained event, oldest first.
 * *len is set to the required size. buf == NULL is a size query (OK). A too-small buf returns
 * SU_E_RESOURCE_LIMIT. */
uint32_t su_evlog_dump(const SuEventLog *log, uint8_t *buf, uint64_t cap, uint64_t *len);
/* Strict parser and validator. Oversize capacity or length is rejected before allocation
 * (SU_E_RESOURCE_LIMIT). Malformed, non-monotonic, inconsistent or trailing-byte input is
 * SU_E_PARSE. */
uint32_t su_evlog_parse(const uint8_t *buf, uint64_t len, SuEventLog **out);
/* Compares configuration, counters and retained events. */
int su_evlog_equal(const SuEventLog *a, const SuEventLog *b);

/* Exact heap accounting of one command queue (M3-08a, contract 9.3 and 9.5). structural: the command
 * buffer (sbuf); events: the event buffer (ebuf); scratch: the per-tick merge and record arrays (m_cmd,
 * t_cmd, rec, m_evt, t_evt, fin); bookkeeping: the buffer metadata, offsets, cursors, heap and the
 * queue struct. total is the sum. NULL q or out gives SU_E_INVALID_ARGUMENT. */
typedef struct SuCmdQMemory {
    uint64_t structural;
    uint64_t events;
    uint64_t scratch;
    uint64_t bookkeeping;
    uint64_t total;
} SuCmdQMemory;
uint32_t su_cmdq_memory(const SuCmdQ *q, SuCmdQMemory *out);
/* Exact heap bytes of one event log: the struct and its ring of capacity records. NULL gives 0. */
uint64_t su_evlog_memory_bytes(const SuEventLog *log);

#endif
