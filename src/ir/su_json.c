#include "su_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/status.h"

#define SU_JSON_CHUNK_BYTES (64u * 1024u)

typedef struct SuJsonChunk SuJsonChunk;
struct SuJsonChunk {
    SuJsonChunk *next;
    size_t used;
    size_t cap;
};

struct SuJsonArena {
    SuJsonChunk *head;
};

static void *arena_alloc(SuJsonArena *a, size_t n) {
    size_t need = (n + 7u) & ~(size_t)7u;
    SuJsonChunk *c = a->head;
    void *r;
    if (need == 0) {
        need = 8;
    }
    if (c == NULL || c->cap - c->used < need) {
        size_t cap = need > SU_JSON_CHUNK_BYTES ? need : SU_JSON_CHUNK_BYTES;
        SuJsonChunk *nc = malloc(sizeof *nc + cap);
        if (nc == NULL) {
            return NULL;
        }
        nc->next = a->head;
        nc->used = 0;
        nc->cap = cap;
        a->head = nc;
        c = nc;
    }
    r = (unsigned char *)(c + 1) + c->used;
    c->used += need;
    return r;
}

static void arena_free(SuJsonArena *a) {
    SuJsonChunk *c = a->head;
    while (c != NULL) {
        SuJsonChunk *next = c->next;
        free(c);
        c = next;
    }
    free(a);
}

typedef struct PathTok {
    int is_index;
    size_t index;
    const char *key;
    size_t key_len;
} PathTok;

typedef struct Parser {
    const uint8_t *src;
    size_t len;
    size_t pos;
    size_t max_depth;
    size_t depth;
    SuJsonArena *arena;
    char *scratch; /* len + 1 bytes: decoded string staging */
    PathTok *path;
    size_t path_len;
    SuJsonError *err;
    int dup_found;
    SuJsonError dup;
} Parser;

static void copy_cstr(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void pbuf_put(char *buf, size_t cap, size_t *n, const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        if (*n + 1 < cap) {
            buf[*n] = s[i];
            (*n)++;
        }
    }
    buf[*n] = '\0';
}

static void build_pointer(const Parser *p, char *out, size_t cap) {
    size_t n = 0, i;
    char tmp[32];
    out[0] = '\0';
    for (i = 0; i < p->path_len; i++) {
        const PathTok *t = &p->path[i];
        pbuf_put(out, cap, &n, "/", 1);
        if (t->is_index) {
            snprintf(tmp, sizeof tmp, "%zu", t->index);
            pbuf_put(out, cap, &n, tmp, strlen(tmp));
        } else {
            size_t k;
            for (k = 0; k < t->key_len; k++) {
                if (t->key[k] == '~') {
                    pbuf_put(out, cap, &n, "~0", 2);
                } else if (t->key[k] == '/') {
                    pbuf_put(out, cap, &n, "~1", 2);
                } else {
                    pbuf_put(out, cap, &n, t->key + k, 1);
                }
            }
        }
    }
}

static uint32_t fail_at(Parser *p, uint32_t code, const char *msg) {
    SuJsonError *e = p->err;
    e->code = code;
    e->offset = p->pos;
    build_pointer(p, e->pointer, sizeof e->pointer);
    copy_cstr(e->message, sizeof e->message, msg);
    return code;
}

static uint32_t fail_top(SuJsonError *e, uint32_t code, size_t offset, const char *msg) {
    e->code = code;
    e->offset = offset;
    e->pointer[0] = '\0';
    copy_cstr(e->message, sizeof e->message, msg);
    return code;
}

static int utf8_valid(const uint8_t *s, size_t n, size_t *bad) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t need;
        uint8_t lo = 0x80u, hi = 0xBFu;
        size_t k;
        if (c < 0x80u) {
            i++;
            continue;
        }
        if (c >= 0xC2u && c <= 0xDFu) {
            need = 1;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            need = 2;
            if (c == 0xE0u) {
                lo = 0xA0u;
            }
            if (c == 0xEDu) {
                hi = 0x9Fu; /* exclude UTF-16 surrogates */
            }
        } else if (c >= 0xF0u && c <= 0xF4u) {
            need = 3;
            if (c == 0xF0u) {
                lo = 0x90u;
            }
            if (c == 0xF4u) {
                hi = 0x8Fu;
            }
        } else {
            *bad = i;
            return 0;
        }
        if (i + need >= n) {
            *bad = i;
            return 0;
        }
        if (s[i + 1] < lo || s[i + 1] > hi) {
            *bad = i;
            return 0;
        }
        for (k = 2; k <= need; k++) {
            if (s[i + k] < 0x80u || s[i + k] > 0xBFu) {
                *bad = i;
                return 0;
            }
        }
        i += need + 1;
    }
    return 1;
}

static uint32_t prepass(const uint8_t *src, size_t len, const SuJsonLimits *lim, SuJsonError *err) {
    size_t i, depth = 0, bad = 0;
    int in_str = 0;

    if (len > lim->max_bytes) {
        return fail_top(err, SU_E_RESOURCE_LIMIT, 0, "input exceeds byte limit");
    }
    for (i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = 0;
            }
            continue;
        }
        if (c == '"') {
            in_str = 1;
        } else if (c == '[' || c == '{') {
            depth++;
            if (depth > lim->max_depth) {
                return fail_top(err, SU_E_RESOURCE_LIMIT, i, "nesting depth exceeds limit");
            }
        } else if (c == ']' || c == '}') {
            if (depth > 0) {
                depth--;
            }
        }
    }
    if (!utf8_valid(src, len, &bad)) {
        return fail_top(err, SU_E_PARSE, bad, "invalid UTF-8");
    }
    return SU_OK;
}

static void skip_ws(Parser *p) {
    while (p->pos < p->len) {
        uint8_t c = p->src[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->pos++;
    }
}

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int read_hex4(Parser *p, uint32_t *out) {
    uint32_t v = 0;
    int k;
    if (p->pos + 4 > p->len) {
        return 0;
    }
    for (k = 0; k < 4; k++) {
        int h = hexval(p->src[p->pos + (size_t)k]);
        if (h < 0) {
            return 0;
        }
        v = (v << 4) | (uint32_t)h;
    }
    p->pos += 4;
    *out = v;
    return 1;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Parses a string literal at p->pos ('"'). The decoded text is copied into the
 * arena and NUL-terminated. */
static uint32_t parse_string(Parser *p, const char **out, size_t *out_len) {
    size_t n = 0;
    char *buf = p->scratch;
    char *copy;

    p->pos++; /* opening quote */
    for (;;) {
        uint8_t c;
        if (p->pos >= p->len) {
            return fail_at(p, SU_E_PARSE, "unterminated string");
        }
        c = p->src[p->pos];
        if (c == '"') {
            p->pos++;
            break;
        }
        if (c < 0x20u) {
            return fail_at(p, SU_E_PARSE, "control character in string");
        }
        if (c != '\\') {
            buf[n++] = (char)c;
            p->pos++;
            continue;
        }
        p->pos++;
        if (p->pos >= p->len) {
            return fail_at(p, SU_E_PARSE, "unterminated escape");
        }
        c = p->src[p->pos++];
        switch (c) {
        case '"': buf[n++] = '"'; break;
        case '\\': buf[n++] = '\\'; break;
        case '/': buf[n++] = '/'; break;
        case 'b': buf[n++] = '\b'; break;
        case 'f': buf[n++] = '\f'; break;
        case 'n': buf[n++] = '\n'; break;
        case 'r': buf[n++] = '\r'; break;
        case 't': buf[n++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(p, &cp)) {
                return fail_at(p, SU_E_PARSE, "bad \\u escape");
            }
            if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                return fail_at(p, SU_E_PARSE, "unpaired low surrogate");
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                uint32_t lo;
                if (p->pos + 2 > p->len || p->src[p->pos] != '\\' || p->src[p->pos + 1] != 'u') {
                    return fail_at(p, SU_E_PARSE, "unpaired high surrogate");
                }
                p->pos += 2;
                if (!read_hex4(p, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                    return fail_at(p, SU_E_PARSE, "invalid surrogate pair");
                }
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
            }
            if (cp == 0) {
                return fail_at(p, SU_E_PARSE, "NUL is not permitted in strings");
            }
            n += utf8_encode(cp, buf + n);
            break;
        }
        default:
            return fail_at(p, SU_E_PARSE, "invalid escape");
        }
    }

    copy = arena_alloc(p->arena, n + 1);
    if (copy == NULL) {
        return fail_at(p, SU_E_INTERNAL, "out of memory");
    }
    memcpy(copy, buf, n);
    copy[n] = '\0';
    *out = copy;
    *out_len = n;
    return SU_OK;
}

static uint32_t parse_number(Parser *p, SuJsonValue *v) {
    int neg = 0, is_int = 1, overflow = 0;
    uint64_t mag = 0;

    v->type = SU_JSON_NUMBER;
    if (p->src[p->pos] == '-') {
        neg = 1;
        p->pos++;
    }
    if (p->pos >= p->len) {
        return fail_at(p, SU_E_PARSE, "expected digit");
    }
    if (p->src[p->pos] == '0') {
        p->pos++;
    } else if (p->src[p->pos] >= '1' && p->src[p->pos] <= '9') {
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
            p->pos++;
        }
    } else {
        return fail_at(p, SU_E_PARSE, "expected digit");
    }
    /* Exact accumulation of the integer digits (no floating point). */
    {
        size_t k;
        for (k = v->offset + (size_t)(neg ? 1 : 0); k < p->pos; k++) {
            uint64_t d = (uint64_t)(p->src[k] - '0');
            if (!overflow) {
                if (mag > (UINT64_MAX - d) / 10u) {
                    overflow = 1;
                } else {
                    mag = mag * 10u + d;
                }
            }
        }
    }
    if (p->pos < p->len && p->src[p->pos] == '.') {
        is_int = 0;
        p->pos++;
        if (p->pos >= p->len || p->src[p->pos] < '0' || p->src[p->pos] > '9') {
            return fail_at(p, SU_E_PARSE, "expected digit after '.'");
        }
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
            p->pos++;
        }
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        is_int = 0;
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) {
            p->pos++;
        }
        if (p->pos >= p->len || p->src[p->pos] < '0' || p->src[p->pos] > '9') {
            return fail_at(p, SU_E_PARSE, "expected digit in exponent");
        }
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
            p->pos++;
        }
    }
    v->u.number.negative = neg;
    v->u.number.is_integer = is_int;
    v->u.number.overflow = is_int ? overflow : 0;
    v->u.number.magnitude = mag;
    return SU_OK;
}

static uint32_t parse_value(Parser *p, SuJsonValue **out);

static uint32_t push_member_key(Parser *p, const char *key, size_t len) {
    if (p->path_len >= p->max_depth + 2u) {
        return SU_E_INTERNAL;
    }
    p->path[p->path_len].is_index = 0;
    p->path[p->path_len].key = key;
    p->path[p->path_len].key_len = len;
    p->path_len++;
    return SU_OK;
}

static void push_index(Parser *p, size_t index) {
    p->path[p->path_len].is_index = 1;
    p->path[p->path_len].index = index;
    p->path[p->path_len].key = NULL;
    p->path[p->path_len].key_len = 0;
    p->path_len++;
}

static uint32_t finish_members(Parser *p, SuJsonValue *v, SuJsonMember *tmp, size_t n) {
    SuJsonMember *dst = NULL;
    if (n > 0) {
        dst = arena_alloc(p->arena, n * sizeof *dst);
        if (dst == NULL) {
            free(tmp);
            return fail_at(p, SU_E_INTERNAL, "out of memory");
        }
        memcpy(dst, tmp, n * sizeof *dst);
    }
    free(tmp);
    v->u.object.members = dst;
    v->u.object.count = n;
    return SU_OK;
}

static uint32_t parse_object(Parser *p, SuJsonValue *v) {
    SuJsonMember *tmp = NULL;
    size_t cap = 0, n = 0;
    uint32_t rc;

    v->type = SU_JSON_OBJECT;
    p->pos++; /* '{' */
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
        p->pos++;
        return finish_members(p, v, tmp, 0);
    }
    for (;;) {
        const char *key;
        size_t klen, i;
        SuJsonValue *child;
        SuJsonMember *m;
        size_t key_pos;

        skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != '"') {
            rc = fail_at(p, SU_E_PARSE, "expected object key string");
            goto fail;
        }
        key_pos = p->pos;
        rc = parse_string(p, &key, &klen);
        if (rc != SU_OK) {
            goto fail;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            rc = fail_at(p, SU_E_PARSE, "expected ':'");
            goto fail;
        }
        p->pos++;

        for (i = 0; i < n; i++) {
            if (tmp[i].key_len == klen && memcmp(tmp[i].key, key, klen) == 0) {
                if (!p->dup_found) {
                    size_t saved_len = p->path_len;
                    p->dup_found = 1;
                    p->dup.code = SU_E_DUPLICATE_KEY;
                    p->dup.offset = key_pos;
                    push_member_key(p, key, klen);
                    build_pointer(p, p->dup.pointer, sizeof p->dup.pointer);
                    p->path_len = saved_len;
                    copy_cstr(p->dup.message, sizeof p->dup.message, "duplicate object key");
                }
                break;
            }
        }

        if (push_member_key(p, key, klen) != SU_OK) {
            rc = SU_E_INTERNAL;
            goto fail;
        }
        skip_ws(p);
        rc = parse_value(p, &child);
        if (rc != SU_OK) {
            goto fail;
        }
        p->path_len--;

        if (n == cap) {
            size_t ncap = cap == 0 ? 4 : cap * 2u;
            SuJsonMember *nt = realloc(tmp, ncap * sizeof *nt);
            if (nt == NULL) {
                rc = fail_at(p, SU_E_INTERNAL, "out of memory");
                goto fail;
            }
            tmp = nt;
            cap = ncap;
        }
        m = &tmp[n++];
        m->key = key;
        m->key_len = klen;
        m->value = child;

        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->src[p->pos] == '}') {
            p->pos++;
            break;
        }
        rc = fail_at(p, SU_E_PARSE, "expected ',' or '}'");
        goto fail;
    }
    return finish_members(p, v, tmp, n);

fail:
    free(tmp);
    return rc;
}

static uint32_t parse_array(Parser *p, SuJsonValue *v) {
    SuJsonValue **tmp = NULL;
    SuJsonValue **dst;
    size_t cap = 0, n = 0;
    uint32_t rc;

    v->type = SU_JSON_ARRAY;
    p->pos++; /* '[' */
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') {
        p->pos++;
        v->u.array.items = NULL;
        v->u.array.count = 0;
        return SU_OK;
    }
    for (;;) {
        SuJsonValue *child;

        push_index(p, n);
        rc = parse_value(p, &child);
        if (rc != SU_OK) {
            goto fail;
        }
        p->path_len--;

        if (n == cap) {
            size_t ncap = cap == 0 ? 4 : cap * 2u;
            SuJsonValue **nt = realloc(tmp, ncap * sizeof *nt);
            if (nt == NULL) {
                rc = fail_at(p, SU_E_INTERNAL, "out of memory");
                goto fail;
            }
            tmp = nt;
            cap = ncap;
        }
        tmp[n++] = child;

        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            skip_ws(p);
            continue;
        }
        if (p->pos < p->len && p->src[p->pos] == ']') {
            p->pos++;
            break;
        }
        rc = fail_at(p, SU_E_PARSE, "expected ',' or ']'");
        goto fail;
    }
    dst = arena_alloc(p->arena, n * sizeof *dst);
    if (dst == NULL) {
        rc = fail_at(p, SU_E_INTERNAL, "out of memory");
        goto fail;
    }
    memcpy(dst, tmp, n * sizeof *dst);
    free(tmp);
    v->u.array.items = dst;
    v->u.array.count = n;
    return SU_OK;

fail:
    free(tmp);
    return rc;
}

static uint32_t parse_literal(Parser *p, SuJsonValue *v, const char *lit, SuJsonType type, int b) {
    size_t n = strlen(lit);
    if (p->pos + n > p->len || memcmp(p->src + p->pos, lit, n) != 0) {
        return fail_at(p, SU_E_PARSE, "invalid literal");
    }
    p->pos += n;
    v->type = type;
    if (type == SU_JSON_BOOL) {
        v->u.boolean = b;
    }
    return SU_OK;
}

static uint32_t parse_value(Parser *p, SuJsonValue **out) {
    SuJsonValue *v;
    uint32_t rc = SU_OK;

    skip_ws(p);
    if (p->pos >= p->len) {
        return fail_at(p, SU_E_PARSE, "unexpected end of input");
    }
    v = arena_alloc(p->arena, sizeof *v);
    if (v == NULL) {
        return fail_at(p, SU_E_INTERNAL, "out of memory");
    }
    memset(v, 0, sizeof *v);
    v->offset = p->pos;

    switch (p->src[p->pos]) {
    case '{':
    case '[':
        if (p->depth + 1u > p->max_depth) {
            return fail_at(p, SU_E_RESOURCE_LIMIT, "nesting depth exceeds limit");
        }
        p->depth++;
        rc = (p->src[p->pos] == '{') ? parse_object(p, v) : parse_array(p, v);
        p->depth--;
        break;
    case '"': {
        const char *s;
        size_t sl;
        rc = parse_string(p, &s, &sl);
        if (rc == SU_OK) {
            v->type = SU_JSON_STRING;
            v->u.string.data = s;
            v->u.string.len = sl;
        }
        break;
    }
    case 't':
        rc = parse_literal(p, v, "true", SU_JSON_BOOL, 1);
        break;
    case 'f':
        rc = parse_literal(p, v, "false", SU_JSON_BOOL, 0);
        break;
    case 'n':
        rc = parse_literal(p, v, "null", SU_JSON_NULL, 0);
        break;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        rc = parse_number(p, v);
        break;
    default:
        rc = fail_at(p, SU_E_PARSE, "unexpected character");
        break;
    }
    if (rc != SU_OK) {
        return rc;
    }
    *out = v;
    return SU_OK;
}

uint32_t su_json_parse(const uint8_t *data, size_t len, const SuJsonLimits *limits,
                       SuJsonDoc *doc, SuJsonError *err) {
    SuJsonLimits lim_default;
    const SuJsonLimits *lim = limits;
    Parser p;
    SuJsonValue *root = NULL;
    uint32_t rc;

    memset(doc, 0, sizeof *doc);
    memset(err, 0, sizeof *err);
    if (lim == NULL) {
        lim_default.max_bytes = 256u * 1024u;
        lim_default.max_depth = 32u;
        lim = &lim_default;
    }
    rc = prepass(data, len, lim, err);
    if (rc != SU_OK) {
        return rc;
    }

    memset(&p, 0, sizeof p);
    p.src = data;
    p.len = len;
    p.max_depth = lim->max_depth;
    p.err = err;
    p.arena = calloc(1, sizeof *p.arena);
    p.scratch = malloc(len + 1u);
    p.path = calloc(lim->max_depth + 2u, sizeof *p.path);
    if (p.arena == NULL || p.scratch == NULL || p.path == NULL) {
        rc = SU_E_INTERNAL;
        goto cleanup_fail;
    }

    rc = parse_value(&p, &root);
    if (rc != SU_OK) {
        goto cleanup_fail;
    }
    skip_ws(&p);
    if (p.pos != len) {
        rc = fail_at(&p, SU_E_PARSE, "trailing data after document");
        goto cleanup_fail;
    }

    free(p.scratch);
    free(p.path);
    doc->root = root;
    doc->arena = p.arena;
    if (p.dup_found) {
        *err = p.dup;
        return SU_E_DUPLICATE_KEY;
    }
    return SU_OK;

cleanup_fail:
    free(p.scratch);
    free(p.path);
    if (p.arena != NULL) {
        arena_free(p.arena);
    }
    return rc;
}

void su_json_doc_free(SuJsonDoc *doc) {
    if (doc->arena != NULL) {
        arena_free(doc->arena);
    }
    doc->arena = NULL;
    doc->root = NULL;
}

const SuJsonValue *su_json_member(const SuJsonValue *obj, const char *key) {
    size_t i, klen;
    if (obj == NULL || obj->type != SU_JSON_OBJECT) {
        return NULL;
    }
    klen = strlen(key);
    for (i = 0; i < obj->u.object.count; i++) {
        const SuJsonMember *m = &obj->u.object.members[i];
        if (m->key_len == klen && memcmp(m->key, key, klen) == 0) {
            return m->value;
        }
    }
    return NULL;
}

int su_json_get_uint(const SuJsonValue *v, uint64_t *out) {
    if (v == NULL || v->type != SU_JSON_NUMBER) {
        return 0;
    }
    if (!v->u.number.is_integer || v->u.number.overflow) {
        return 0;
    }
    if (v->u.number.negative && v->u.number.magnitude != 0) {
        return 0;
    }
    *out = v->u.number.magnitude;
    return 1;
}
