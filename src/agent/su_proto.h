/* JSON-lines protocol primitives shared by the supervisor (su_agent.c) and the
 * worker (runtime/worker.c). POSIX only.
 *
 * Envelope (docs/AGENT_RPC.md section 1): one strict UTF-8 JSON object per line,
 * at most SU_LIMIT_PROTOCOL_MESSAGE bytes excluding the newline. Requests carry
 * exactly the keys protocol, id, method, params. Responses are exactly one line. */
#ifndef SU_AGENT_PROTO_H
#define SU_AGENT_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "ir/su_json.h"

#define SU_PROTOCOL_VERSION "0.1"
#define SU_PROTO_ID_MAX 128u
#define SU_PROTO_METHOD_MAX 128u
#define SU_PROTO_MAX_DEPTH 8u

/* Growable byte buffer. Writes past SU_LIMIT_PROTOCOL_MESSAGE (or a failed
 * allocation) set failed; callers must check it before using the contents. */
typedef struct SuBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    int failed;
} SuBuf;

void su_buf_init(SuBuf *b);
void su_buf_free(SuBuf *b);
void su_buf_put(SuBuf *b, const void *p, size_t n);
void su_buf_str(SuBuf *b, const char *s);
void su_buf_quoted(SuBuf *b, const char *s, size_t n);
void su_buf_u64(SuBuf *b, uint64_t v);

/* Bounded line reader. Lines longer than SU_LIMIT_PROTOCOL_MESSAGE are not
 * stored: the reader discards bytes up to the newline and reports overflow. */
typedef struct SuLineReader {
    int fd;
    int64_t deadline_ms; /* absolute, su_now_ms() clock; < 0 means no deadline */
    uint8_t chunk[8192];
    size_t pos;
    size_t fill;
    int eof;
    int overflow;
    uint8_t *line;
    size_t line_len;
    size_t line_cap;
} SuLineReader;

void su_line_reader_init(SuLineReader *r, int fd, int64_t deadline_ms);
void su_line_reader_free(SuLineReader *r);
/* Returns 1 with a line (without its newline; *len is 0 and *overflow set when
 * the line exceeded the limit), 0 at clean EOF, -1 on I/O error, -2 on deadline.
 * The returned pointer is valid until the next call on the same reader. */
int su_line_next(SuLineReader *r, const uint8_t **line, size_t *len, int *overflow);

/* Writes all bytes; deadline_ms < 0 blocks. Returns 0, -1 on error, -2 on deadline. */
int su_write_all(int fd, const uint8_t *p, size_t n, int64_t deadline_ms);
int64_t su_now_ms(void);

/* Parsed request. doc owns the tree; free with su_json_doc_free after use. */
typedef struct SuRequest {
    int has_id;
    char id[SU_PROTO_ID_MAX + 1];
    char method[SU_PROTO_METHOD_MAX + 1];
    const SuJsonValue *params;
} SuRequest;

/* Validation order: size (SU_E_RESOURCE_LIMIT), syntax/UTF-8 (SU_E_PARSE),
 * duplicate keys (SU_E_DUPLICATE_KEY), envelope shape (SU_E_TYPE). On error,
 * req->has_id reports whether the id could still be echoed. */
uint32_t su_proto_parse_request(const uint8_t *line, size_t len, int overflow,
                                SuJsonDoc *doc, SuRequest *req, char *msg, size_t msg_cap);

/* Response writers. Each produces one complete JSON object (no newline). */
void su_proto_ok(SuBuf *out, int has_id, const char *id, const SuBuf *result);
void su_proto_error(SuBuf *out, int has_id, const char *id, uint32_t code, const char *message);
/* As su_proto_error, plus error.detail = detail_json (a complete JSON object; NULL omits it). */
void su_proto_error_detail(SuBuf *out, int has_id, const char *id, uint32_t code, const char *message,
                           const char *detail_json);

/* Shared JSON helpers over parsed values. */
int su_json_is_string(const SuJsonValue *v);
/* Copies a string member into dst (NUL-terminated). Returns 0 on success. */
int su_json_copy_string(const SuJsonValue *v, char *dst, size_t cap);
/* Looks up a u32 member. Returns 0 on success. */
int su_json_get_u32(const SuJsonValue *obj, const char *key, uint32_t *out);

#endif
