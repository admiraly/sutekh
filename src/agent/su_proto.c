#define _POSIX_C_SOURCE 200809L

#include "agent/su_proto.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sutekh/ir.h"
#include "sutekh/status.h"

/* ---------- buffers ---------- */

void su_buf_init(SuBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

void su_buf_free(SuBuf *b) {
    free(b->data);
    su_buf_init(b);
}

void su_buf_put(SuBuf *b, const void *p, size_t n) {
    size_t need;
    size_t cap;
    uint8_t *grown;

    if (b->failed || n == 0) {
        return;
    }
    if (n > SU_LIMIT_PROTOCOL_MESSAGE - b->len) {
        b->failed = 1;
        return;
    }
    need = b->len + n;
    if (need > b->cap) {
        cap = b->cap != 0 ? b->cap : 256u;
        while (cap < need) {
            cap *= 2u;
        }
        if (cap > SU_LIMIT_PROTOCOL_MESSAGE) {
            cap = SU_LIMIT_PROTOCOL_MESSAGE;
        }
        grown = realloc(b->data, cap);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void su_buf_str(SuBuf *b, const char *s) {
    su_buf_put(b, s, strlen(s));
}

void su_buf_quoted(SuBuf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t i;

    su_buf_put(b, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            su_buf_put(b, "\\\"", 2);
        } else if (c == '\\') {
            su_buf_put(b, "\\\\", 2);
        } else if (c == '\n') {
            su_buf_put(b, "\\n", 2);
        } else if (c == '\r') {
            su_buf_put(b, "\\r", 2);
        } else if (c == '\t') {
            su_buf_put(b, "\\t", 2);
        } else if (c < 0x20u) {
            char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xFu]};
            su_buf_put(b, esc, sizeof esc);
        } else {
            su_buf_put(b, &s[i], 1);
        }
    }
    su_buf_put(b, "\"", 1);
}

void su_buf_u64(SuBuf *b, uint64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    if (n > 0) {
        su_buf_put(b, tmp, (size_t)n);
    }
}

/* ---------- time and fd I/O ---------- */

int64_t su_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* 0 when ready, -1 on poll error, -2 when the deadline passed. */
static int wait_fd(int fd, short events, int64_t deadline_ms) {
    for (;;) {
        int64_t rem;
        int rc;
        struct pollfd pfd;

        if (deadline_ms < 0) {
            return 0;
        }
        rem = deadline_ms - su_now_ms();
        if (rem <= 0) {
            return -2;
        }
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        rc = poll(&pfd, 1, (int)(rem > 1000000 ? 1000000 : rem));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc > 0) {
            return 0;
        }
    }
}

int su_write_all(int fd, const uint8_t *p, size_t n, int64_t deadline_ms) {
    while (n > 0) {
        ssize_t w;
        int rc = wait_fd(fd, POLLOUT, deadline_ms);
        if (rc != 0) {
            return rc;
        }
        w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---------- line reader ---------- */

void su_line_reader_init(SuLineReader *r, int fd, int64_t deadline_ms) {
    memset(r, 0, sizeof *r);
    r->fd = fd;
    r->deadline_ms = deadline_ms;
}

void su_line_reader_free(SuLineReader *r) {
    free(r->line);
    memset(r, 0, sizeof *r);
}

static void append_line(SuLineReader *r, const uint8_t *p, size_t n) {
    size_t need;
    size_t cap;
    uint8_t *grown;

    if (r->overflow || n == 0) {
        return;
    }
    if (n > SU_LIMIT_PROTOCOL_MESSAGE - r->line_len) {
        r->overflow = 1;
        return;
    }
    need = r->line_len + n;
    if (need > r->line_cap) {
        cap = r->line_cap != 0 ? r->line_cap : 1024u;
        while (cap < need) {
            cap *= 2u;
        }
        if (cap > SU_LIMIT_PROTOCOL_MESSAGE) {
            cap = SU_LIMIT_PROTOCOL_MESSAGE;
        }
        grown = realloc(r->line, cap);
        if (grown == NULL) {
            r->overflow = 1; /* reported as resource exhaustion */
            return;
        }
        r->line = grown;
        r->line_cap = cap;
    }
    memcpy(r->line + r->line_len, p, n);
    r->line_len += n;
}

int su_line_next(SuLineReader *r, const uint8_t **line, size_t *len, int *overflow) {
    int saw_newline = 0;

    *line = NULL;
    *len = 0;
    *overflow = 0;
    r->line_len = 0;
    r->overflow = 0;

    for (;;) {
        const uint8_t *start;
        const uint8_t *nl;
        size_t avail;
        size_t take;

        if (r->pos == r->fill) {
            ssize_t n;
            int rc;
            if (r->eof) {
                break;
            }
            rc = wait_fd(r->fd, POLLIN, r->deadline_ms);
            if (rc != 0) {
                return rc;
            }
            n = read(r->fd, r->chunk, sizeof r->chunk);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                r->eof = 1;
                break;
            }
            r->pos = 0;
            r->fill = (size_t)n;
        }
        start = r->chunk + r->pos;
        avail = r->fill - r->pos;
        nl = memchr(start, '\n', avail);
        take = nl != NULL ? (size_t)(nl - start) : avail;
        append_line(r, start, take);
        r->pos += take;
        if (nl != NULL) {
            r->pos += 1u;
            saw_newline = 1;
            break;
        }
    }

    if (!saw_newline && r->line_len == 0 && !r->overflow) {
        return 0; /* clean EOF */
    }
    *line = r->line;
    *overflow = r->overflow;
    *len = r->overflow ? 0 : r->line_len;
    return 1;
}

/* ---------- request parsing ---------- */

static void set_msg(char *msg, size_t cap, const char *text) {
    if (msg != NULL && cap > 0) {
        snprintf(msg, cap, "%s", text);
    }
}

static int key_is(const SuJsonMember *m, const char *name) {
    size_t n = strlen(name);
    return m->key_len == n && memcmp(m->key, name, n) == 0;
}

int su_json_is_string(const SuJsonValue *v) {
    return v != NULL && v->type == SU_JSON_STRING;
}

int su_json_copy_string(const SuJsonValue *v, char *dst, size_t cap) {
    if (!su_json_is_string(v) || v->u.string.len >= cap) {
        return -1;
    }
    memcpy(dst, v->u.string.data, v->u.string.len);
    dst[v->u.string.len] = '\0';
    return 0;
}

int su_json_get_u32(const SuJsonValue *obj, const char *key, uint32_t *out) {
    const SuJsonValue *v;
    uint64_t u;
    if (obj == NULL || obj->type != SU_JSON_OBJECT) {
        return -1;
    }
    v = su_json_member(obj, key);
    if (v == NULL || !su_json_get_uint(v, &u) || u > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)u;
    return 0;
}

uint32_t su_proto_parse_request(const uint8_t *line, size_t len, int overflow,
                                SuJsonDoc *doc, SuRequest *req, char *msg, size_t msg_cap) {
    SuJsonLimits limits;
    SuJsonError err;
    const SuJsonValue *root;
    const SuJsonValue *v;
    size_t i;
    uint32_t st;

    memset(req, 0, sizeof *req);
    doc->root = NULL;
    doc->arena = NULL;

    if (overflow || len > SU_LIMIT_PROTOCOL_MESSAGE) {
        set_msg(msg, msg_cap, "message exceeds 1 MiB");
        return SU_E_RESOURCE_LIMIT;
    }
    limits.max_bytes = SU_LIMIT_PROTOCOL_MESSAGE;
    limits.max_depth = SU_PROTO_MAX_DEPTH;
    st = su_json_parse(line, len, &limits, doc, &err);
    if (st == SU_E_DUPLICATE_KEY || st == SU_E_PARSE || st == SU_E_RESOURCE_LIMIT) {
        set_msg(msg, msg_cap, err.message);
        return st;
    }
    if (st != SU_OK) {
        set_msg(msg, msg_cap, err.message);
        return st;
    }

    root = doc->root;
    if (root == NULL || root->type != SU_JSON_OBJECT) {
        set_msg(msg, msg_cap, "request must be a JSON object");
        return SU_E_TYPE;
    }

    v = su_json_member(root, "id");
    if (su_json_is_string(v) && v->u.string.len >= 1 && v->u.string.len <= SU_PROTO_ID_MAX &&
        su_json_copy_string(v, req->id, sizeof req->id) == 0) {
        req->has_id = 1;
    }

    for (i = 0; i < root->u.object.count; i++) {
        const SuJsonMember *m = &root->u.object.members[i];
        if (!key_is(m, "protocol") && !key_is(m, "id") && !key_is(m, "method") &&
            !key_is(m, "params")) {
            set_msg(msg, msg_cap, "unexpected key in request envelope");
            return SU_E_TYPE;
        }
    }

    v = su_json_member(root, "protocol");
    if (!su_json_is_string(v) || v->u.string.len != strlen(SU_PROTOCOL_VERSION) ||
        memcmp(v->u.string.data, SU_PROTOCOL_VERSION, v->u.string.len) != 0) {
        set_msg(msg, msg_cap, "unsupported or missing protocol version (expected \"0.1\")");
        return SU_E_TYPE;
    }
    if (!req->has_id) {
        set_msg(msg, msg_cap, "id must be a string of 1..128 bytes");
        return SU_E_TYPE;
    }
    v = su_json_member(root, "method");
    if (su_json_copy_string(v, req->method, sizeof req->method) != 0 ||
        v->u.string.len == 0) {
        set_msg(msg, msg_cap, "method must be a non-empty string of at most 128 bytes");
        return SU_E_TYPE;
    }
    v = su_json_member(root, "params");
    if (v == NULL || v->type != SU_JSON_OBJECT) {
        set_msg(msg, msg_cap, "params must be a JSON object");
        return SU_E_TYPE;
    }
    req->params = v;
    return SU_OK;
}

/* ---------- response writers ---------- */

static void put_id(SuBuf *out, int has_id, const char *id) {
    if (has_id) {
        su_buf_quoted(out, id, strlen(id));
    } else {
        su_buf_str(out, "null");
    }
}

void su_proto_ok(SuBuf *out, int has_id, const char *id, const SuBuf *result) {
    su_buf_str(out, "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":");
    put_id(out, has_id, id);
    su_buf_str(out, ",\"ok\":true,\"result\":");
    su_buf_put(out, result->data, result->len);
    su_buf_str(out, "}");
}

void su_proto_error_detail(SuBuf *out, int has_id, const char *id, uint32_t code, const char *message,
                           const char *detail_json) {
    int retry = code == SU_E_WORKER_FAILED || code == SU_E_STALE_REVISION;
    su_buf_str(out, "{\"protocol\":\"" SU_PROTOCOL_VERSION "\",\"id\":");
    put_id(out, has_id, id);
    su_buf_str(out, ",\"ok\":false,\"error\":{\"code\":\"");
    su_buf_str(out, su_status_name(code));
    su_buf_str(out, "\",\"status\":");
    su_buf_u64(out, code);
    su_buf_str(out, ",\"message\":");
    su_buf_quoted(out, message, strlen(message));
    su_buf_str(out, ",\"retry\":");
    su_buf_str(out, retry ? "true" : "false");
    if (detail_json != NULL) {
        /* detail_json is a complete JSON object written by the supervisor, never client text. */
        su_buf_str(out, ",\"detail\":");
        su_buf_str(out, detail_json);
    }
    su_buf_str(out, "}}");
}

void su_proto_error(SuBuf *out, int has_id, const char *id, uint32_t code, const char *message) {
    su_proto_error_detail(out, has_id, id, code, message, NULL);
}
