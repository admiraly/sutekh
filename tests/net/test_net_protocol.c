/* Protocol unit tests for Sutekh::net (include/sutekh/net.h):
 *   - session creation argument checks
 *   - packet serialization bounds & redundancy window bundling
 *   - malformed packet rejection (truncated, bad magic/version/salt/peer/counts/order/trailing bytes)
 *   - schema_hash / content_hash / tick_rate_hz mismatch rejection
 *   - fixed-memory ring wrap (> 2 * SU_NET_INPUT_RING_CAP ticks) and out-of-window bounds
 *   - duplicate & conflicting input handling, clean BYE disconnect
 *   - zero heap allocations during su_net_poll / send_input / pop_remote_input / send_checksum / check_desync / stats */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/net.h"

static long g_checks = 0;
static long g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            g_fail++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

/* ---- allocation counting (Linux glibc non-ASan via --wrap) ---- */

#ifdef SU_TEST_WRAP_ALLOC
static volatile int g_count_alloc = 0;
static unsigned long g_allocs = 0;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);

void *__wrap_malloc(size_t n) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t m) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __real_calloc(n, m);
}

void *__wrap_realloc(void *p, size_t n) {
    if (g_count_alloc) {
        g_allocs++;
    }
    return __real_realloc(p, n);
}
#endif

/* ---- little-endian test helpers ---- */

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
    p[4] = (uint8_t)((v >> 32) & 0xFFu);
    p[5] = (uint8_t)((v >> 40) & 0xFFu);
    p[6] = (uint8_t)((v >> 48) & 0xFFu);
    p[7] = (uint8_t)((v >> 56) & 0xFFu);
}

/* ---- in-memory packet queue for unit tests ---- */

#define Q_CAP 64u

typedef struct PktQueue {
    uint8_t pkts[Q_CAP][SU_NET_MAX_PACKET_BYTES + 16u];
    size_t lens[Q_CAP];
    SuNetAddr addrs[Q_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} PktQueue;

static void q_push(PktQueue *q, const SuNetAddr *addr, const void *data, size_t len) {
    if (q->count >= Q_CAP) {
        return;
    }
    memcpy(q->pkts[q->tail], data, len);
    q->lens[q->tail] = len;
    if (addr != NULL) {
        q->addrs[q->tail] = *addr;
    } else {
        memset(&q->addrs[q->tail], 0, sizeof q->addrs[q->tail]);
    }
    q->tail = (q->tail + 1u) % Q_CAP;
    q->count++;
}

typedef struct PairBus {
    PktQueue rx[2]; /* rx[dst] receives packets sent by src */
} PairBus;

typedef struct PairEnd {
    PairBus *bus;
    uint32_t self;
} PairEnd;

static uint32_t pair_send(void *user, const SuNetAddr *to, const void *data, size_t len) {
    PairEnd *end = (PairEnd *)user;
    uint32_t dst = to->peer_tag;
    if (dst >= 2u) {
        return SU_E_INVALID_ARGUMENT;
    }
    q_push(&end->bus->rx[dst], to, data, len);
    return SU_OK;
}

static uint32_t pair_recv(void *user, SuNetAddr *from, void *buf, size_t cap, size_t *out_len) {
    PairEnd *end = (PairEnd *)user;
    PktQueue *q = &end->bus->rx[end->self];
    if (q->count == 0u) {
        *out_len = 0u;
        return SU_OK;
    }
    {
        size_t len = q->lens[q->head];
        if (len > cap) {
            q->head = (q->head + 1u) % Q_CAP;
            q->count--;
            return SU_E_RESOURCE_LIMIT;
        }
        memcpy(buf, q->pkts[q->head], len);
        *out_len = len;
        if (from != NULL) {
            *from = q->addrs[q->head];
        }
        q->head = (q->head + 1u) % Q_CAP;
        q->count--;
    }
    return SU_OK;
}

static SuNetSessionDesc make_desc(uint32_t local_peer, uint32_t peer_count, SuNetTransport tr) {
    SuNetSessionDesc d;
    uint32_t i;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.local_peer = local_peer;
    d.peer_count = peer_count;
    d.tick_rate_hz = 60u;
    d.session_salt = UINT64_C(0xCAFEBABE12345678);
    for (i = 0u; i < 32u; i++) {
        d.schema_hash[i] = (uint8_t)(0x10u + i);
        d.content_hash[i] = (uint8_t)(0x80u + i);
    }
    d.redundancy = SU_NET_MAX_REDUNDANCY;
    d.retransmit_polls = 2u;
    d.transport = tr;
    for (i = 0u; i < peer_count; i++) {
        d.peers[i].enabled = 1u;
        d.peers[i].addr.peer_tag = i;
    }
    return d;
}

static void build_header(uint8_t *p, uint8_t msg_type, uint8_t src_peer, uint8_t peer_count, uint64_t salt) {
    memcpy(p, SU_NET_MAGIC, SU_NET_MAGIC_LEN);
    put_u16(p + 8, SU_NET_WIRE_VERSION);
    p[10] = msg_type;
    p[11] = src_peer;
    p[12] = peer_count;
    p[13] = 0u;
    p[14] = 0u;
    p[15] = 0u;
    put_u64(p + 16, salt);
}

static void build_hello(uint8_t *p, uint8_t msg_type, uint8_t src_peer, uint8_t peer_count, uint64_t salt,
                        const uint8_t schema[32], const uint8_t content[32], uint32_t hz) {
    build_header(p, msg_type, src_peer, peer_count, salt);
    memcpy(p + 24, schema, 32u);
    memcpy(p + 56, content, 32u);
    put_u32(p + 88, hz);
    put_u32(p + 92, 0u);
}

/* ---- tests ---- */

static void test_creation_validation(void) {
    PairBus bus;
    PairEnd e0 = {&bus, 0u};
    SuNetTransport tr = {pair_send, pair_recv, &e0};
    SuNetSessionDesc d = make_desc(0u, 2u, tr);
    SuNetSession *s = NULL;

    memset(&bus, 0, sizeof bus);
    CHECK(su_net_api_version() == SU_NET_API_VERSION);

    CHECK(su_net_session_create(NULL, &s) == SU_E_INVALID_ARGUMENT && s == NULL);
    CHECK(su_net_session_create(&d, NULL) == SU_E_INVALID_ARGUMENT);

    d.struct_size = sizeof d - 1u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.struct_size = sizeof d;

    d.peer_count = 0u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.peer_count = SU_NET_MAX_PEERS + 1u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.peer_count = 2u;

    d.local_peer = 2u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.local_peer = 0u;

    d.tick_rate_hz = 0u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.tick_rate_hz = 60u;

    d.redundancy = SU_NET_MAX_REDUNDANCY + 1u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.redundancy = SU_NET_MAX_REDUNDANCY;

    d.transport.send_fn = NULL;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.transport.send_fn = pair_send;

    d.peers[1].enabled = 0u;
    CHECK(su_net_session_create(&d, &s) == SU_E_INVALID_ARGUMENT);
    d.peers[1].enabled = 1u;

    CHECK(su_net_session_create(&d, &s) == SU_OK && s != NULL);
    CHECK(su_net_is_ready(s) == 0u);
    su_net_session_destroy(s);
    su_net_session_destroy(NULL);
    su_net_udp_close(NULL);
}

static void test_malformed_and_schema_rejection(void) {
    PairBus bus;
    PairEnd e0 = {&bus, 0u};
    PairEnd e1 = {&bus, 1u};
    SuNetTransport tr0 = {pair_send, pair_recv, &e0};
    SuNetTransport tr1 = {pair_send, pair_recv, &e1};
    SuNetSessionDesc d0 = make_desc(0u, 2u, tr0);
    SuNetSessionDesc d1 = make_desc(1u, 2u, tr1);
    SuNetSession *s0 = NULL;
    SuNetSession *s1 = NULL;
    SuNetStats st;
    SuNetPeerInfo pi;
    uint8_t pkt[256];

    memset(&bus, 0, sizeof bus);
    CHECK(su_net_session_create(&d0, &s0) == SU_OK);

    /* 1. Truncated packet. */
    memset(pkt, 0, sizeof pkt);
    q_push(&bus.rx[0], NULL, pkt, 10u);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_malformed == 1u);

    /* 2. Bad magic. */
    build_hello(pkt, SU_NET_MSG_HELLO, 1u, 2u, d0.session_salt, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    pkt[5] = '2';
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_malformed == 2u);

    /* 3. Bad wire version. */
    build_hello(pkt, SU_NET_MSG_HELLO, 1u, 2u, d0.session_salt, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    put_u16(pkt + 8, 99u);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_malformed == 3u);

    /* 4. Unknown msg_type. */
    build_hello(pkt, 99u, 1u, 2u, d0.session_salt, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_malformed == 4u);

    /* 5. Wrong salt / peer_count / self-peer. */
    build_hello(pkt, SU_NET_MSG_HELLO, 1u, 2u, d0.session_salt ^ 1u, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES);
    build_hello(pkt, SU_NET_MSG_HELLO, 1u, 3u, d0.session_salt, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES);
    build_hello(pkt, SU_NET_MSG_HELLO, 0u, 2u, d0.session_salt, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_rejected == 3u);

    /* 6. HELLO wrong length, CHECKSUM wrong length, BYE wrong length. */
    build_hello(pkt, SU_NET_MSG_HELLO, 1u, 2u, d0.session_salt, d0.schema_hash, d0.content_hash, d0.tick_rate_hz);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_HELLO_BYTES - 1u);
    build_header(pkt, SU_NET_MSG_CHECKSUM, 1u, 2u, d0.session_salt);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_CHECKSUM_BYTES - 1u);
    build_header(pkt, SU_NET_MSG_BYE, 1u, 2u, d0.session_salt);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_BYE_BYTES + 1u);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_malformed == 7u);

    /* 7. INPUT before handshake must be rejected. */
    build_header(pkt, SU_NET_MSG_INPUT, 1u, 2u, d0.session_salt);
    put_u64(pkt + 24, 0u);
    put_u64(pkt + 32, 0u);
    put_u64(pkt + 40, SU_NET_TICK_NONE);
    put_u16(pkt + 48, 0u);
    put_u16(pkt + 50, 0u);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_INPUT_HEADER_BYTES);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_rejected == 4u);

    /* 8. Schema mismatch rejection via two real sessions (schema_hash, then content_hash, then tick_rate). */
    d1.schema_hash[0] ^= 0xFFu;
    CHECK(su_net_session_create(&d1, &s1) == SU_OK);
    CHECK(su_net_send_hello(s1) == SU_OK);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.schema_mismatches == 1u);
    CHECK(su_net_peer_info(s0, 1u, &pi) == SU_OK && pi.status == SU_NET_PEER_REJECTED);
    CHECK(su_net_is_ready(s0) == 0u);
    su_net_session_destroy(s0);
    su_net_session_destroy(s1);

    memset(&bus, 0, sizeof bus);
    d1 = make_desc(1u, 2u, tr1);
    d1.content_hash[31] ^= 0x01u;
    CHECK(su_net_session_create(&d0, &s0) == SU_OK);
    CHECK(su_net_session_create(&d1, &s1) == SU_OK);
    CHECK(su_net_send_hello(s1) == SU_OK);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.schema_mismatches == 1u);
    CHECK(su_net_peer_info(s0, 1u, &pi) == SU_OK && pi.status == SU_NET_PEER_REJECTED);
    su_net_session_destroy(s0);
    su_net_session_destroy(s1);

    memset(&bus, 0, sizeof bus);
    d1 = make_desc(1u, 2u, tr1);
    d1.tick_rate_hz = 120u;
    CHECK(su_net_session_create(&d0, &s0) == SU_OK);
    CHECK(su_net_session_create(&d1, &s1) == SU_OK);
    CHECK(su_net_send_hello(s1) == SU_OK);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.schema_mismatches == 1u);
    CHECK(su_net_peer_info(s0, 1u, &pi) == SU_OK && pi.status == SU_NET_PEER_REJECTED);
    su_net_session_destroy(s0);
    su_net_session_destroy(s1);
}

static void test_input_malformed_and_bounds(void) {
    PairBus bus;
    PairEnd e0 = {&bus, 0u};
    PairEnd e1 = {&bus, 1u};
    SuNetTransport tr0 = {pair_send, pair_recv, &e0};
    SuNetTransport tr1 = {pair_send, pair_recv, &e1};
    SuNetSessionDesc d0 = make_desc(0u, 2u, tr0);
    SuNetSessionDesc d1 = make_desc(1u, 2u, tr1);
    SuNetSession *s0 = NULL;
    SuNetSession *s1 = NULL;
    uint8_t pkt[SU_NET_MAX_PACKET_BYTES];
    uint8_t big_in[SU_NET_MAX_INPUT_BYTES + 1u];
    uint8_t out[64];
    size_t out_len = 0u;
    uint8_t avail = 0u;
    SuNetStats st;
    uint32_t i;

    memset(&bus, 0, sizeof bus);
    memset(big_in, 0xAB, sizeof big_in);
    CHECK(su_net_session_create(&d0, &s0) == SU_OK);
    CHECK(su_net_session_create(&d1, &s1) == SU_OK);

    /* Handshake. */
    for (i = 0u; i < 4u && (!su_net_is_ready(s0) || !su_net_is_ready(s1)); i++) {
        CHECK(su_net_poll(s0) == SU_OK);
        CHECK(su_net_poll(s1) == SU_OK);
    }
    CHECK(su_net_is_ready(s0) == 1u && su_net_is_ready(s1) == 1u);
    memset(&bus, 0, sizeof bus); /* clear any trailing packets */

    /* Oversized input blob rejected. */
    CHECK(su_net_send_input(s0, 0u, big_in, SU_NET_MAX_INPUT_BYTES + 1u) == SU_E_RESOURCE_LIMIT);

    /* Exact max-size input blob accepted. */
    CHECK(su_net_send_input(s0, 0u, big_in, SU_NET_MAX_INPUT_BYTES) == SU_OK);
    CHECK(bus.rx[1].count == 1u);
    bus.rx[1].head = bus.rx[1].tail = bus.rx[1].count = 0u;

    /* Redundancy window: send ticks 1..7 without ACKs -> last packet carries 8 frames (0..7). */
    for (i = 1u; i <= 7u; i++) {
        uint8_t b[4] = {(uint8_t)i, 1u, 2u, 3u};
        if (i < 7u) {
            bus.rx[1].head = bus.rx[1].tail = bus.rx[1].count = 0u;
        }
        CHECK(su_net_send_input(s0, (uint64_t)i, b, sizeof b) == SU_OK);
    }
    CHECK(bus.rx[1].count == 2u); /* tick 6 kept + tick 7 */
    {
        const uint8_t *last_pkt = bus.rx[1].pkts[(bus.rx[1].tail + Q_CAP - 1u) % Q_CAP];
        CHECK(rd_u64(last_pkt + 24) == 0u);
        CHECK(rd_u64(last_pkt + 32) == 7u);
        CHECK(rd_u16(last_pkt + 48) == 8u);
    }

    /* Crafted malformed INPUT packets into s0:
     * (a) frame_count > SU_NET_MAX_REDUNDANCY */
    build_header(pkt, SU_NET_MSG_INPUT, 1u, 2u, d0.session_salt);
    put_u64(pkt + 24, 0u);
    put_u64(pkt + 32, 0u);
    put_u64(pkt + 40, SU_NET_TICK_NONE);
    put_u16(pkt + 48, SU_NET_MAX_REDUNDANCY + 1u);
    put_u16(pkt + 50, 0u);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_INPUT_HEADER_BYTES);

    /* (b) atomic rejection: frame 0 valid (tick 0, len 2), frame 1 truncated/non-ascending -> tick 0 NOT committed */
    put_u64(pkt + 24, 0u);
    put_u64(pkt + 32, 1u);
    put_u16(pkt + 48, 2u);
    put_u64(pkt + 52, 0u);
    put_u16(pkt + 60, 2u);
    pkt[62] = 0xAAu;
    pkt[63] = 0xBBu;
    put_u64(pkt + 64, 0u); /* non-ascending tick 0 <= 0 */
    put_u16(pkt + 72, 2u);
    pkt[74] = 0xCCu;
    pkt[75] = 0xDDu;
    q_push(&bus.rx[0], NULL, pkt, 76u);

    /* (c) trailing byte */
    put_u64(pkt + 24, 0u);
    put_u64(pkt + 32, 0u);
    put_u16(pkt + 48, 1u);
    put_u64(pkt + 52, 0u);
    put_u16(pkt + 60, 2u);
    pkt[62] = 1u;
    pkt[63] = 2u;
    pkt[64] = 0u; /* trailing */
    q_push(&bus.rx[0], NULL, pkt, 65u);

    /* (d) frame blob_len > SU_NET_MAX_INPUT_BYTES */
    put_u64(pkt + 52, 0u);
    put_u16(pkt + 60, SU_NET_MAX_INPUT_BYTES + 1u);
    q_push(&bus.rx[0], NULL, pkt, SU_NET_INPUT_HEADER_BYTES + 10u + SU_NET_MAX_INPUT_BYTES + 1u);

    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.packets_malformed == 4u && st.input_frames_recv == 0u);
    CHECK(su_net_pop_remote_input(s0, 1u, 0u, out, sizeof out, &out_len, &avail) == SU_OK && avail == 0u);

    /* Pop undersized buffer test: deliver valid tick 0 (len 4), fail pop with cap=2, succeed with cap=4. */
    put_u64(pkt + 24, 0u);
    put_u64(pkt + 32, 0u);
    put_u16(pkt + 48, 1u);
    put_u64(pkt + 52, 0u);
    put_u16(pkt + 60, 4u);
    pkt[62] = 0x11u;
    pkt[63] = 0x22u;
    pkt[64] = 0x33u;
    pkt[65] = 0x44u;
    q_push(&bus.rx[0], NULL, pkt, 66u);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_pop_remote_input(s0, 1u, 0u, out, 2u, &out_len, &avail) == SU_E_RESOURCE_LIMIT &&
          out_len == 4u && avail == 0u);
    CHECK(su_net_pop_remote_input(s0, 1u, 0u, out, sizeof out, &out_len, &avail) == SU_OK &&
          avail == 1u && out_len == 4u && out[0] == 0x11u && out[3] == 0x44u);

    /* Conflicting duplicate frame for tick 1: first wins, conflicting_frames increments. */
    put_u64(pkt + 24, 1u);
    put_u64(pkt + 32, 1u);
    put_u16(pkt + 48, 1u);
    put_u64(pkt + 52, 1u);
    put_u16(pkt + 60, 2u);
    pkt[62] = 0xAAu;
    pkt[63] = 0xBBu;
    q_push(&bus.rx[0], NULL, pkt, 64u);
    pkt[62] = 0xFFu;
    q_push(&bus.rx[0], NULL, pkt, 64u);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.conflicting_frames == 1u);
    CHECK(su_net_pop_remote_input(s0, 1u, 1u, out, sizeof out, &out_len, &avail) == SU_OK &&
          avail == 1u && out_len == 2u && out[0] == 0xAAu);

    /* Out-of-window future frame (next_pop_tick is 2, send tick 2 + SU_NET_INPUT_RING_CAP). */
    put_u64(pkt + 24, 2u + SU_NET_INPUT_RING_CAP);
    put_u64(pkt + 32, 2u + SU_NET_INPUT_RING_CAP);
    put_u16(pkt + 48, 1u);
    put_u64(pkt + 52, 2u + SU_NET_INPUT_RING_CAP);
    put_u16(pkt + 60, 2u);
    pkt[62] = 0x55u;
    pkt[63] = 0x66u;
    q_push(&bus.rx[0], NULL, pkt, 64u);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.out_of_window_frames == 1u);

    su_net_session_destroy(s0);
    su_net_session_destroy(s1);
}

static void test_ring_wrap_bye_and_zero_alloc(void) {
    PairBus bus;
    PairEnd e0 = {&bus, 0u};
    PairEnd e1 = {&bus, 1u};
    SuNetTransport tr0 = {pair_send, pair_recv, &e0};
    SuNetTransport tr1 = {pair_send, pair_recv, &e1};
    SuNetSessionDesc d0 = make_desc(0u, 2u, tr0);
    SuNetSessionDesc d1 = make_desc(1u, 2u, tr1);
    SuNetSession *s0 = NULL;
    SuNetSession *s1 = NULL;
    SuNetPeerInfo pi;
    SuNetDesyncInfo di;
    SuNetStats st;
    uint64_t t;
    unsigned long allocs = 0;

    memset(&bus, 0, sizeof bus);
    CHECK(su_net_session_create(&d0, &s0) == SU_OK);
    CHECK(su_net_session_create(&d1, &s1) == SU_OK);

#ifdef SU_TEST_WRAP_ALLOC
    g_allocs = 0;
    g_count_alloc = 1;
#endif

    CHECK(su_net_send_hello(s0) == SU_OK);
    CHECK(su_net_send_hello(s1) == SU_OK);
    for (t = 0u; t < 8u && (!su_net_is_ready(s0) || !su_net_is_ready(s1)); t++) {
        CHECK(su_net_poll(s0) == SU_OK);
        CHECK(su_net_poll(s1) == SU_OK);
    }
    CHECK(su_net_is_ready(s0) == 1u && su_net_is_ready(s1) == 1u);

    /* Wrap both SU_NET_INPUT_RING_CAP (256) and SU_NET_CHECKSUM_RING_CAP (128): run 600 ticks. */
    for (t = 0u; t < 600u; t++) {
        uint8_t b0[8];
        uint8_t b1[8];
        uint8_t got[8];
        size_t got_len = 0u;
        uint8_t avail = 0u;
        uint64_t digest = UINT64_C(0x9E3779B97F4A7C15) ^ (t * UINT64_C(0xBF58476D1CE4E5B9));

        put_u64(b0, t * 3u + 1u);
        put_u64(b1, t * 5u + 2u);
        CHECK(su_net_send_input(s0, t, b0, sizeof b0) == SU_OK);
        CHECK(su_net_send_input(s1, t, b1, sizeof b1) == SU_OK);
        CHECK(su_net_poll(s0) == SU_OK);
        CHECK(su_net_poll(s1) == SU_OK);

        CHECK(su_net_pop_remote_input(s0, 1u, t, got, sizeof got, &got_len, &avail) == SU_OK &&
              avail == 1u && got_len == sizeof b1 && memcmp(got, b1, sizeof b1) == 0);
        CHECK(su_net_pop_remote_input(s1, 0u, t, got, sizeof got, &got_len, &avail) == SU_OK &&
              avail == 1u && got_len == sizeof b0 && memcmp(got, b0, sizeof b0) == 0);

        /* Popping the same tick again returns avail == 0. */
        CHECK(su_net_pop_remote_input(s0, 1u, t, got, sizeof got, &got_len, &avail) == SU_OK && avail == 0u);

        if ((t % 4u) == 0u) {
            CHECK(su_net_send_checksum(s0, t, digest) == SU_OK);
            CHECK(su_net_send_checksum(s1, t, digest) == SU_OK);
        }
    }
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_poll(s1) == SU_OK);
    CHECK(su_net_peer_info(s0, 1u, &pi) == SU_OK && pi.local_ack_tick == 599u && pi.highest_rx_tick == 599u);
    CHECK(su_net_check_desync(s0, &di) == SU_OK && di.detected == 0u);
    CHECK(su_net_check_desync(s1, &di) == SU_OK && di.detected == 0u);

    /* Clean BYE. */
    CHECK(su_net_send_bye(s1, SU_NET_BYE_NORMAL) == SU_OK);
    CHECK(su_net_poll(s0) == SU_OK);
    CHECK(su_net_peer_info(s0, 1u, &pi) == SU_OK && pi.status == SU_NET_PEER_BYE &&
          pi.bye_reason == SU_NET_BYE_NORMAL);
    CHECK(su_net_stats(s0, &st) == SU_OK && st.input_frames_recv >= 600u);

#ifdef SU_TEST_WRAP_ALLOC
    g_count_alloc = 0;
    allocs = g_allocs;
#endif
    CHECK(allocs == 0u);

    su_net_session_destroy(s0);
    su_net_session_destroy(s1);
}

int main(void) {
    test_creation_validation();
    test_malformed_and_schema_rejection();
    test_input_malformed_and_bounds();
    test_ring_wrap_bye_and_zero_alloc();
#ifndef SU_TEST_WRAP_ALLOC
    printf("note: allocation counting not built in (non-Linux or ASan)\n");
#endif
    printf("test_net_protocol: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
