/* Sutekh UDP peer session & deterministic input/checksum sync (include/sutekh/net.h).
 *
 * Host-side networking library (`su_net`). Zero per-packet heap allocations after `su_net_session_create`. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "sutekh/net.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define SU_NET_POLL_MAX_PACKETS 256u
#define SU_NET_DEFAULT_RETRANSMIT_POLLS 2u

/* ---- little-endian wire encoding ---- */

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

/* ---- public utility helpers ---- */

uint32_t su_net_api_version(void) {
    return SU_NET_API_VERSION;
}

uint64_t su_net_digest64_from_sha256(const uint8_t sha256[32]) {
    if (sha256 == NULL) {
        return 0u;
    }
    return rd_u64(sha256);
}

uint32_t su_net_addr_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port, SuNetAddr *out) {
    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    out->family = SU_NET_AF_IPV4;
    out->port = port;
    out->ip[0] = a;
    out->ip[1] = b;
    out->ip[2] = c;
    out->ip[3] = d;
    return SU_OK;
}

uint32_t su_net_addr_ipv6(const uint8_t ip[16], uint16_t port, SuNetAddr *out) {
    if (out == NULL || ip == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    out->family = SU_NET_AF_IPV6;
    out->port = port;
    memcpy(out->ip, ip, 16u);
    return SU_OK;
}

/* ---- built-in non-blocking UDP backend ---- */

struct SuNetUdpSocket {
#ifdef _WIN32
    SOCKET fd;
    int wsa_started;
#else
    int fd;
#endif
    uint8_t family;
};

static uint32_t addr_to_sockaddr(const SuNetAddr *addr, struct sockaddr_storage *ss, socklen_t *ss_len) {
    memset(ss, 0, sizeof *ss);
    if (addr->family == SU_NET_AF_IPV4) {
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->port);
        memcpy(&sin->sin_addr, addr->ip, 4u);
        *ss_len = (socklen_t)sizeof(*sin);
        return SU_OK;
    }
    if (addr->family == SU_NET_AF_IPV6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->port);
        memcpy(&sin6->sin6_addr, addr->ip, 16u);
        *ss_len = (socklen_t)sizeof(*sin6);
        return SU_OK;
    }
    return SU_E_INVALID_ARGUMENT;
}

static uint32_t sockaddr_to_addr(const struct sockaddr_storage *ss, SuNetAddr *out) {
    memset(out, 0, sizeof *out);
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
        out->family = SU_NET_AF_IPV4;
        out->port = ntohs(sin->sin_port);
        memcpy(out->ip, &sin->sin_addr, 4u);
        return SU_OK;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
        out->family = SU_NET_AF_IPV6;
        out->port = ntohs(sin6->sin6_port);
        memcpy(out->ip, &sin6->sin6_addr, 16u);
        return SU_OK;
    }
    return SU_E_IO;
}

static uint32_t udp_send(void *user, const SuNetAddr *to, const void *data, size_t len) {
    SuNetUdpSocket *sock = (SuNetUdpSocket *)user;
    struct sockaddr_storage ss;
    socklen_t ss_len = 0;
#ifdef _WIN32
    int sent;
#else
    ssize_t sent;
#endif

    if (sock == NULL || to == NULL || (data == NULL && len > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (len > SU_NET_MAX_PACKET_BYTES) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (addr_to_sockaddr(to, &ss, &ss_len) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    sent = sendto(sock->fd, (const char *)data, (int)len, 0, (const struct sockaddr *)&ss, (int)ss_len);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        return (err == WSAEWOULDBLOCK) ? SU_E_RESOURCE_LIMIT : SU_E_IO;
    }
    return ((size_t)sent == len) ? SU_OK : SU_E_IO;
#else
    for (;;) {
        sent = sendto(sock->fd, data, len, 0, (const struct sockaddr *)&ss, ss_len);
        if (sent >= 0) {
            return ((size_t)sent == len) ? SU_OK : SU_E_IO;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SU_E_RESOURCE_LIMIT;
        }
        return SU_E_IO;
    }
#endif
}

static uint32_t udp_recv(void *user, SuNetAddr *from, void *buf, size_t cap, size_t *out_len) {
    SuNetUdpSocket *sock = (SuNetUdpSocket *)user;
    struct sockaddr_storage ss;
    socklen_t ss_len = (socklen_t)sizeof(ss);
#ifdef _WIN32
    int got;
#else
    ssize_t got;
#endif

    if (out_len == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out_len = 0u;
    if (sock == NULL || buf == NULL || cap == 0u) {
        return SU_E_INVALID_ARGUMENT;
    }
    memset(&ss, 0, sizeof ss);
#ifdef _WIN32
    {
        int slen = (int)ss_len;
        got = recvfrom(sock->fd, (char *)buf, (int)cap, 0, (struct sockaddr *)&ss, &slen);
        if (got == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                return SU_OK;
            }
            if (err == WSAEMSGSIZE) {
                return SU_E_RESOURCE_LIMIT;
            }
            return SU_E_IO;
        }
        *out_len = (size_t)got;
    }
#else
    for (;;) {
        got = recvfrom(sock->fd, buf, cap, 0, (struct sockaddr *)&ss, &ss_len);
        if (got >= 0) {
            *out_len = (size_t)got;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SU_OK;
        }
        return SU_E_IO;
    }
#endif
    if (from != NULL) {
        (void)sockaddr_to_addr(&ss, from);
    }
    return SU_OK;
}

uint32_t su_net_udp_open(const SuNetAddr *bind_addr, SuNetUdpSocket **out, SuNetAddr *bound_addr) {
    SuNetAddr effective;
    struct sockaddr_storage ss;
    socklen_t ss_len = 0;
    SuNetUdpSocket *sock;
    int af;
#ifdef _WIN32
    WSADATA wsa;
    SOCKET fd;
    u_long nb = 1;
#else
    int fd;
    int flags;
#endif

    if (out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (bind_addr == NULL) {
        su_net_addr_ipv4(0u, 0u, 0u, 0u, 0u, &effective);
    } else {
        effective = *bind_addr;
    }
    if (effective.family == SU_NET_AF_IPV4) {
        af = AF_INET;
    } else if (effective.family == SU_NET_AF_IPV6) {
        af = AF_INET6;
    } else {
        return SU_E_INVALID_ARGUMENT;
    }
    if (addr_to_sockaddr(&effective, &ss, &ss_len) != SU_OK) {
        return SU_E_INVALID_ARGUMENT;
    }

    sock = calloc(1u, sizeof *sock);
    if (sock == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    sock->family = effective.family;

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        free(sock);
        return SU_E_IO;
    }
    sock->wsa_started = 1;
    fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        WSACleanup();
        free(sock);
        return SU_E_IO;
    }
    if (ioctlsocket(fd, FIONBIO, &nb) != 0 ||
        bind(fd, (const struct sockaddr *)&ss, (int)ss_len) != 0) {
        closesocket(fd);
        WSACleanup();
        free(sock);
        return SU_E_IO;
    }
    if (bound_addr != NULL) {
        struct sockaddr_storage actual;
        int actual_len = (int)sizeof(actual);
        memset(&actual, 0, sizeof actual);
        if (getsockname(fd, (struct sockaddr *)&actual, &actual_len) != 0 ||
            sockaddr_to_addr(&actual, bound_addr) != SU_OK) {
            closesocket(fd);
            WSACleanup();
            free(sock);
            return SU_E_IO;
        }
    }
    sock->fd = fd;
#else
    fd = socket(af, SOCK_DGRAM, 0);
    if (fd < 0) {
        free(sock);
        return SU_E_IO;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        free(sock);
        return SU_E_IO;
    }
#ifdef FD_CLOEXEC
    {
        int fdflags = fcntl(fd, F_GETFD, 0);
        if (fdflags >= 0) {
            (void)fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
        }
    }
#endif
    if (bind(fd, (const struct sockaddr *)&ss, ss_len) != 0) {
        close(fd);
        free(sock);
        return SU_E_IO;
    }
    if (bound_addr != NULL) {
        struct sockaddr_storage actual;
        socklen_t actual_len = (socklen_t)sizeof(actual);
        memset(&actual, 0, sizeof actual);
        if (getsockname(fd, (struct sockaddr *)&actual, &actual_len) != 0 ||
            sockaddr_to_addr(&actual, bound_addr) != SU_OK) {
            close(fd);
            free(sock);
            return SU_E_IO;
        }
    }
    sock->fd = fd;
#endif

    *out = sock;
    return SU_OK;
}

void su_net_udp_close(SuNetUdpSocket *sock) {
    if (sock == NULL) {
        return;
    }
#ifdef _WIN32
    if (sock->fd != INVALID_SOCKET) {
        closesocket(sock->fd);
    }
    if (sock->wsa_started) {
        WSACleanup();
    }
#else
    if (sock->fd >= 0) {
        close(sock->fd);
    }
#endif
    free(sock);
}

SuNetTransport su_net_udp_transport(SuNetUdpSocket *sock) {
    SuNetTransport t;
    if (sock == NULL) {
        t.send_fn = NULL;
        t.recv_fn = NULL;
        t.user = NULL;
        return t;
    }
    t.send_fn = udp_send;
    t.recv_fn = udp_recv;
    t.user = sock;
    return t;
}

/* ---- session internals ---- */

typedef struct SuNetInputSlot {
    uint64_t tick;
    uint16_t len;
    uint8_t valid;
    uint8_t popped;
    uint8_t data[SU_NET_MAX_INPUT_BYTES];
} SuNetInputSlot;

typedef struct SuNetChecksumSlot {
    uint64_t tick;
    uint64_t digest;
    uint8_t valid;
} SuNetChecksumSlot;

typedef struct SuNetPeerState {
    uint32_t status;
    uint32_t bye_reason;
    uint8_t rx_hello;
    uint8_t rx_welcome;
    uint8_t tx_welcome_needed;
    uint8_t ack_dirty;
    uint32_t polls_since_hello;
    uint32_t polls_since_tx;
    uint64_t local_ack_tick;
    uint64_t remote_ack_tick;
    uint64_t highest_rx_tick;
    uint64_t next_pop_tick;
    uint64_t last_bye_tick;
    SuNetInputSlot rx_ring[SU_NET_INPUT_RING_CAP];
    SuNetChecksumSlot checksum_ring[SU_NET_CHECKSUM_RING_CAP];
} SuNetPeerState;

struct SuNetSession {
    uint32_t local_peer;
    uint32_t peer_count;
    uint32_t tick_rate_hz;
    uint32_t redundancy;
    uint32_t retransmit_polls;
    uint64_t session_salt;
    uint8_t schema_hash[32];
    uint8_t content_hash[32];
    SuNetTransport transport;
    SuNetPeerConfig peers[SU_NET_MAX_PEERS];
    SuNetPeerState peers_state[SU_NET_MAX_PEERS];
    SuNetInputSlot local_ring[SU_NET_INPUT_RING_CAP];
    SuNetChecksumSlot local_checksums[SU_NET_CHECKSUM_RING_CAP];
    uint64_t local_highest_tick;
    SuNetDesyncInfo desync;
    SuNetStats stats;
    uint8_t scratch_pkt[SU_NET_MAX_PACKET_BYTES];
};

static void write_common_header(uint8_t *pkt, const SuNetSession *s, uint8_t msg_type) {
    memcpy(pkt, SU_NET_MAGIC, SU_NET_MAGIC_LEN);
    put_u16(pkt + 8, (uint16_t)SU_NET_WIRE_VERSION);
    pkt[10] = msg_type;
    pkt[11] = (uint8_t)s->local_peer;
    pkt[12] = (uint8_t)s->peer_count;
    pkt[13] = 0u;
    pkt[14] = 0u;
    pkt[15] = 0u;
    put_u64(pkt + 16, s->session_salt);
}

static uint32_t transmit_packet(SuNetSession *s, uint32_t dst_peer, size_t len) {
    uint32_t st;
    st = s->transport.send_fn(s->transport.user, &s->peers[dst_peer].addr, s->scratch_pkt, len);
    if (st == SU_OK) {
        s->stats.packets_sent++;
        s->stats.bytes_sent += (uint64_t)len;
    }
    return st;
}

static uint32_t send_hello_to(SuNetSession *s, uint32_t dst_peer, uint8_t msg_type) {
    uint8_t *p = s->scratch_pkt;
    write_common_header(p, s, msg_type);
    memcpy(p + 24, s->schema_hash, 32u);
    memcpy(p + 56, s->content_hash, 32u);
    put_u32(p + 88, s->tick_rate_hz);
    put_u32(p + 92, 0u);
    return transmit_packet(s, dst_peer, SU_NET_HELLO_BYTES);
}

static uint32_t send_input_to(SuNetSession *s, uint32_t dst_peer) {
    SuNetPeerState *ps = &s->peers_state[dst_peer];
    uint8_t *p = s->scratch_pkt;
    uint64_t ticks[SU_NET_MAX_REDUNDANCY];
    uint16_t frame_count = 0u;
    size_t off = SU_NET_INPUT_HEADER_BYTES;
    uint64_t first_tick = 0u;
    uint64_t last_tick = 0u;

    if (s->local_highest_tick != SU_NET_TICK_NONE) {
        uint64_t newest = s->local_highest_tick;
        uint64_t oldest_in_ring = (newest >= SU_NET_INPUT_RING_CAP)
                                      ? (newest - SU_NET_INPUT_RING_CAP + 1u)
                                      : 0u;
        uint64_t start = (ps->remote_ack_tick == SU_NET_TICK_NONE) ? 0u : (ps->remote_ack_tick + 1u);
        if (start < oldest_in_ring) {
            start = oldest_in_ring;
        }
        if (start <= newest) {
            uint64_t span = newest - start + 1u;
            uint64_t t;
            if (span <= (uint64_t)s->redundancy) {
                for (t = start; t <= newest && frame_count < s->redundancy; t++) {
                    const SuNetInputSlot *slot = &s->local_ring[t % SU_NET_INPUT_RING_CAP];
                    if (slot->valid && slot->tick == t) {
                        ticks[frame_count++] = t;
                    }
                }
            } else {
                uint32_t head_cap = s->redundancy - 1u;
                for (t = start; t < newest && frame_count < head_cap; t++) {
                    const SuNetInputSlot *slot = &s->local_ring[t % SU_NET_INPUT_RING_CAP];
                    if (slot->valid && slot->tick == t) {
                        ticks[frame_count++] = t;
                    }
                }
                {
                    const SuNetInputSlot *ns = &s->local_ring[newest % SU_NET_INPUT_RING_CAP];
                    if (ns->valid && ns->tick == newest && frame_count < s->redundancy) {
                        ticks[frame_count++] = newest;
                    }
                }
            }
        }
    }

    if (frame_count > 0u) {
        uint16_t actual_count = 0u;
        uint16_t i;
        for (i = 0u; i < frame_count; i++) {
            const SuNetInputSlot *slot = &s->local_ring[ticks[i] % SU_NET_INPUT_RING_CAP];
            size_t need = SU_NET_INPUT_FRAME_HEADER_BYTES + (size_t)slot->len;
            if (off + need > sizeof s->scratch_pkt) {
                break;
            }
            put_u64(p + off, slot->tick);
            put_u16(p + off + 8, slot->len);
            if (slot->len > 0u) {
                memcpy(p + off + 10, slot->data, slot->len);
            }
            off += need;
            if (actual_count == 0u) {
                first_tick = slot->tick;
            }
            last_tick = slot->tick;
            actual_count++;
        }
        frame_count = actual_count;
    }

    write_common_header(p, s, (uint8_t)SU_NET_MSG_INPUT);
    put_u64(p + 24, first_tick);
    put_u64(p + 32, last_tick);
    put_u64(p + 40, ps->local_ack_tick);
    put_u16(p + 48, frame_count);
    put_u16(p + 50, 0u);

    {
        uint32_t st = transmit_packet(s, dst_peer, off);
        if (st == SU_OK) {
            s->stats.input_frames_sent += (uint64_t)frame_count;
            ps->polls_since_tx = 0u;
            ps->ack_dirty = 0u;
        }
        return st;
    }
}

static uint32_t send_checksum_to(SuNetSession *s, uint32_t dst_peer, uint64_t tick, uint64_t digest) {
    uint8_t *p = s->scratch_pkt;
    write_common_header(p, s, (uint8_t)SU_NET_MSG_CHECKSUM);
    put_u64(p + 24, tick);
    put_u64(p + 32, digest);
    return transmit_packet(s, dst_peer, SU_NET_CHECKSUM_BYTES);
}

static uint32_t send_bye_to(SuNetSession *s, uint32_t dst_peer, uint64_t last_tick, uint32_t reason) {
    uint8_t *p = s->scratch_pkt;
    write_common_header(p, s, (uint8_t)SU_NET_MSG_BYE);
    put_u64(p + 24, last_tick);
    put_u32(p + 32, reason);
    put_u32(p + 36, 0u);
    return transmit_packet(s, dst_peer, SU_NET_BYE_BYTES);
}

static void record_desync(SuNetSession *s, uint32_t peer_index, uint64_t tick,
                          uint64_t local_digest, uint64_t remote_digest) {
    if (!s->desync.detected || tick < s->desync.tick) {
        s->desync.detected = 1u;
        s->desync.tick = tick;
        s->desync.peer_index = peer_index;
        s->desync.local_digest = local_digest;
        s->desync.remote_digest = remote_digest;
    }
}

static void handle_hello_or_welcome(SuNetSession *s, uint32_t src_peer, uint8_t msg_type,
                                    const uint8_t *pkt, size_t len) {
    SuNetPeerState *ps = &s->peers_state[src_peer];
    uint32_t tick_rate;
    int match;

    if (len != SU_NET_HELLO_BYTES) {
        s->stats.packets_malformed++;
        return;
    }
    tick_rate = rd_u32(pkt + 88);
    match = (memcmp(pkt + 24, s->schema_hash, 32u) == 0) &&
            (memcmp(pkt + 56, s->content_hash, 32u) == 0) &&
            (tick_rate == s->tick_rate_hz);
    if (!match) {
        s->stats.schema_mismatches++;
        ps->status = SU_NET_PEER_REJECTED;
        ps->tx_welcome_needed = 0u;
        return;
    }
    if (ps->status == SU_NET_PEER_REJECTED || ps->status == SU_NET_PEER_BYE) {
        s->stats.packets_rejected++;
        return;
    }
    if (msg_type == SU_NET_MSG_HELLO) {
        ps->rx_hello = 1u;
        ps->tx_welcome_needed = 1u;
    } else {
        ps->rx_welcome = 1u;
        ps->status = SU_NET_PEER_CONNECTED;
    }
}

static void handle_input_packet(SuNetSession *s, uint32_t src_peer, const uint8_t *pkt, size_t len) {
    SuNetPeerState *ps = &s->peers_state[src_peer];
    uint64_t first_tick;
    uint64_t last_tick;
    uint64_t ack_tick;
    uint16_t frame_count;
    size_t off;
    uint64_t prev_tick = 0u;
    uint16_t i;

    if (len < SU_NET_INPUT_HEADER_BYTES) {
        s->stats.packets_malformed++;
        return;
    }
    if (ps->status == SU_NET_PEER_REJECTED || ps->status == SU_NET_PEER_BYE ||
        (!ps->rx_hello && !ps->rx_welcome)) {
        s->stats.packets_rejected++;
        return;
    }

    first_tick = rd_u64(pkt + 24);
    last_tick = rd_u64(pkt + 32);
    ack_tick = rd_u64(pkt + 40);
    frame_count = rd_u16(pkt + 48);

    if (frame_count > SU_NET_MAX_REDUNDANCY || (frame_count > 0u && first_tick > last_tick)) {
        s->stats.packets_malformed++;
        return;
    }

    /* Atomic dry-run validation of all frames in the packet before mutating session state. */
    off = SU_NET_INPUT_HEADER_BYTES;
    for (i = 0u; i < frame_count; i++) {
        uint64_t f_tick;
        uint16_t f_len;
        if (off + SU_NET_INPUT_FRAME_HEADER_BYTES > len) {
            s->stats.packets_malformed++;
            return;
        }
        f_tick = rd_u64(pkt + off);
        f_len = rd_u16(pkt + off + 8);
        if (f_len > SU_NET_MAX_INPUT_BYTES ||
            off + SU_NET_INPUT_FRAME_HEADER_BYTES + (size_t)f_len > len) {
            s->stats.packets_malformed++;
            return;
        }
        if (f_tick < first_tick || f_tick > last_tick || (i > 0u && f_tick <= prev_tick)) {
            s->stats.packets_malformed++;
            return;
        }
        prev_tick = f_tick;
        off += SU_NET_INPUT_FRAME_HEADER_BYTES + (size_t)f_len;
    }
    if (off != len) {
        s->stats.packets_malformed++;
        return;
    }

    /* Commit remote ack_tick. */
    if (ack_tick != SU_NET_TICK_NONE) {
        if (ps->remote_ack_tick == SU_NET_TICK_NONE || ack_tick > ps->remote_ack_tick) {
            ps->remote_ack_tick = ack_tick;
        }
    }

    /* Commit frames into bounded rx_ring. */
    off = SU_NET_INPUT_HEADER_BYTES;
    for (i = 0u; i < frame_count; i++) {
        uint64_t f_tick = rd_u64(pkt + off);
        uint16_t f_len = rd_u16(pkt + off + 8);
        const uint8_t *f_data = pkt + off + SU_NET_INPUT_FRAME_HEADER_BYTES;
        off += SU_NET_INPUT_FRAME_HEADER_BYTES + (size_t)f_len;

        if (f_tick < ps->next_pop_tick) {
            s->stats.duplicate_frames++;
            continue;
        }
        if (f_tick >= ps->next_pop_tick + (uint64_t)SU_NET_INPUT_RING_CAP) {
            s->stats.out_of_window_frames++;
            continue;
        }

        {
            SuNetInputSlot *slot = &ps->rx_ring[f_tick % SU_NET_INPUT_RING_CAP];
            if (slot->valid && slot->tick == f_tick) {
                if (slot->len != f_len || (f_len > 0u && memcmp(slot->data, f_data, f_len) != 0)) {
                    s->stats.conflicting_frames++;
                } else {
                    s->stats.duplicate_frames++;
                }
            } else {
                slot->tick = f_tick;
                slot->len = f_len;
                slot->valid = 1u;
                slot->popped = 0u;
                if (f_len > 0u) {
                    memcpy(slot->data, f_data, f_len);
                }
                s->stats.input_frames_recv++;
                if (ps->highest_rx_tick == SU_NET_TICK_NONE || f_tick > ps->highest_rx_tick) {
                    ps->highest_rx_tick = f_tick;
                }
                for (;;) {
                    uint64_t next = (ps->local_ack_tick == SU_NET_TICK_NONE) ? 0u : (ps->local_ack_tick + 1u);
                    const SuNetInputSlot *ns = &ps->rx_ring[next % SU_NET_INPUT_RING_CAP];
                    if (!ns->valid || ns->tick != next) {
                        break;
                    }
                    ps->local_ack_tick = next;
                    ps->ack_dirty = 1u;
                }
            }
        }
    }
}

static void handle_checksum_packet(SuNetSession *s, uint32_t src_peer, const uint8_t *pkt, size_t len) {
    SuNetPeerState *ps = &s->peers_state[src_peer];
    uint64_t tick;
    uint64_t digest;
    size_t idx;

    if (len != SU_NET_CHECKSUM_BYTES) {
        s->stats.packets_malformed++;
        return;
    }
    if (ps->status == SU_NET_PEER_REJECTED || ps->status == SU_NET_PEER_BYE ||
        (!ps->rx_hello && !ps->rx_welcome)) {
        s->stats.packets_rejected++;
        return;
    }
    tick = rd_u64(pkt + 24);
    digest = rd_u64(pkt + 32);
    s->stats.checksums_recv++;

    idx = (size_t)(tick % SU_NET_CHECKSUM_RING_CAP);
    ps->checksum_ring[idx].tick = tick;
    ps->checksum_ring[idx].digest = digest;
    ps->checksum_ring[idx].valid = 1u;

    if (s->local_checksums[idx].valid && s->local_checksums[idx].tick == tick &&
        s->local_checksums[idx].digest != digest) {
        record_desync(s, src_peer, tick, s->local_checksums[idx].digest, digest);
    }
}

static void handle_bye_packet(SuNetSession *s, uint32_t src_peer, const uint8_t *pkt, size_t len) {
    SuNetPeerState *ps = &s->peers_state[src_peer];
    if (len != SU_NET_BYE_BYTES) {
        s->stats.packets_malformed++;
        return;
    }
    ps->status = SU_NET_PEER_BYE;
    ps->last_bye_tick = rd_u64(pkt + 24);
    ps->bye_reason = rd_u32(pkt + 32);
    ps->tx_welcome_needed = 0u;
}

static void process_packet(SuNetSession *s, const uint8_t *pkt, size_t len) {
    uint16_t ver;
    uint8_t msg_type;
    uint8_t src_peer;
    uint8_t pkt_peer_count;
    uint64_t salt;

    if (len < SU_NET_COMMON_HEADER_BYTES || memcmp(pkt, SU_NET_MAGIC, SU_NET_MAGIC_LEN) != 0) {
        s->stats.packets_malformed++;
        return;
    }
    ver = rd_u16(pkt + 8);
    if (ver != SU_NET_WIRE_VERSION) {
        s->stats.packets_malformed++;
        return;
    }
    msg_type = pkt[10];
    src_peer = pkt[11];
    pkt_peer_count = pkt[12];
    salt = rd_u64(pkt + 16);

    if ((uint32_t)pkt_peer_count != s->peer_count ||
        (uint32_t)src_peer >= s->peer_count ||
        (uint32_t)src_peer == s->local_peer ||
        !s->peers[src_peer].enabled ||
        salt != s->session_salt) {
        s->stats.packets_rejected++;
        return;
    }

    switch (msg_type) {
    case SU_NET_MSG_HELLO:
    case SU_NET_MSG_WELCOME:
        handle_hello_or_welcome(s, src_peer, msg_type, pkt, len);
        break;
    case SU_NET_MSG_INPUT:
        handle_input_packet(s, src_peer, pkt, len);
        break;
    case SU_NET_MSG_CHECKSUM:
        handle_checksum_packet(s, src_peer, pkt, len);
        break;
    case SU_NET_MSG_BYE:
        handle_bye_packet(s, src_peer, pkt, len);
        break;
    default:
        s->stats.packets_malformed++;
        break;
    }
}

/* ---- public session API ---- */

uint32_t su_net_session_create(const SuNetSessionDesc *desc, SuNetSession **out) {
    SuNetSession *s;
    uint32_t i;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL || desc->struct_size != sizeof(SuNetSessionDesc)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (desc->peer_count == 0u || desc->peer_count > SU_NET_MAX_PEERS ||
        desc->local_peer >= desc->peer_count || desc->tick_rate_hz == 0u ||
        desc->redundancy > SU_NET_MAX_REDUNDANCY ||
        desc->transport.send_fn == NULL || desc->transport.recv_fn == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < desc->peer_count; i++) {
        if (i != desc->local_peer && !desc->peers[i].enabled) {
            return SU_E_INVALID_ARGUMENT;
        }
    }

    s = calloc(1u, sizeof *s);
    if (s == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }

    s->local_peer = desc->local_peer;
    s->peer_count = desc->peer_count;
    s->tick_rate_hz = desc->tick_rate_hz;
    s->redundancy = (desc->redundancy == 0u) ? SU_NET_MAX_REDUNDANCY : desc->redundancy;
    s->retransmit_polls = (desc->retransmit_polls == 0u) ? SU_NET_DEFAULT_RETRANSMIT_POLLS : desc->retransmit_polls;
    s->session_salt = desc->session_salt;
    memcpy(s->schema_hash, desc->schema_hash, 32u);
    memcpy(s->content_hash, desc->content_hash, 32u);
    s->transport = desc->transport;
    s->local_highest_tick = SU_NET_TICK_NONE;

    for (i = 0u; i < SU_NET_MAX_PEERS; i++) {
        s->peers[i] = desc->peers[i];
        s->peers_state[i].local_ack_tick = SU_NET_TICK_NONE;
        s->peers_state[i].remote_ack_tick = SU_NET_TICK_NONE;
        s->peers_state[i].highest_rx_tick = SU_NET_TICK_NONE;
        s->peers_state[i].last_bye_tick = SU_NET_TICK_NONE;
        if (i < desc->peer_count) {
            if (i == desc->local_peer) {
                s->peers_state[i].status = SU_NET_PEER_CONNECTED;
            } else {
                s->peers_state[i].status = SU_NET_PEER_CONNECTING;
            }
        } else {
            s->peers_state[i].status = SU_NET_PEER_DISCONNECTED;
        }
    }

    *out = s;
    return SU_OK;
}

void su_net_session_destroy(SuNetSession *s) {
    free(s);
}

uint32_t su_net_send_hello(SuNetSession *s) {
    uint32_t p;
    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    for (p = 0u; p < s->peer_count; p++) {
        if (p == s->local_peer || !s->peers[p].enabled) {
            continue;
        }
        if (s->peers_state[p].status == SU_NET_PEER_DISCONNECTED ||
            s->peers_state[p].status == SU_NET_PEER_CONNECTING) {
            s->peers_state[p].status = SU_NET_PEER_CONNECTING;
            (void)send_hello_to(s, p, (uint8_t)SU_NET_MSG_HELLO);
            s->peers_state[p].polls_since_hello = 1u;
        }
    }
    return SU_OK;
}

uint32_t su_net_send_input(SuNetSession *s, uint64_t tick, const void *blob, size_t len) {
    SuNetInputSlot *slot;
    uint32_t p;

    if (s == NULL || (blob == NULL && len > 0u)) {
        return SU_E_INVALID_ARGUMENT;
    }
    if (len > SU_NET_MAX_INPUT_BYTES || tick == SU_NET_TICK_NONE) {
        return SU_E_RESOURCE_LIMIT;
    }

    slot = &s->local_ring[tick % SU_NET_INPUT_RING_CAP];
    slot->tick = tick;
    slot->len = (uint16_t)len;
    slot->valid = 1u;
    slot->popped = 0u;
    if (len > 0u) {
        memcpy(slot->data, blob, len);
    }
    if (s->local_highest_tick == SU_NET_TICK_NONE || tick > s->local_highest_tick) {
        s->local_highest_tick = tick;
    }

    for (p = 0u; p < s->peer_count; p++) {
        SuNetPeerState *ps = &s->peers_state[p];
        if (p == s->local_peer || !s->peers[p].enabled) {
            continue;
        }
        if (ps->status == SU_NET_PEER_CONNECTED ||
            (ps->status == SU_NET_PEER_CONNECTING && ps->rx_hello)) {
            (void)send_input_to(s, p);
        }
    }
    return SU_OK;
}

static uint32_t read_remote_input(SuNetSession *s, uint32_t peer_index, uint64_t tick,
                                  void *buf, size_t cap, size_t *out_len, uint8_t *out_available,
                                  int consume) {
    SuNetPeerState *ps;
    SuNetInputSlot *slot;

    if (out_available != NULL) {
        *out_available = 0u;
    }
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (s == NULL || out_available == NULL || peer_index >= s->peer_count ||
        peer_index == s->local_peer) {
        return SU_E_INVALID_ARGUMENT;
    }
    ps = &s->peers_state[peer_index];
    if (tick < ps->next_pop_tick || tick >= ps->next_pop_tick + (uint64_t)SU_NET_INPUT_RING_CAP) {
        return SU_OK;
    }
    slot = &ps->rx_ring[tick % SU_NET_INPUT_RING_CAP];
    if (!slot->valid || slot->tick != tick || (consume && slot->popped)) {
        return SU_OK;
    }
    if (out_len != NULL) {
        *out_len = slot->len;
    }
    if (slot->len > 0u && (buf == NULL || cap < (size_t)slot->len)) {
        return SU_E_RESOURCE_LIMIT;
    }
    if (slot->len > 0u) {
        memcpy(buf, slot->data, slot->len);
    }
    *out_available = 1u;
    if (consume) {
        slot->popped = 1u;
        for (;;) {
            SuNetInputSlot *ns = &ps->rx_ring[ps->next_pop_tick % SU_NET_INPUT_RING_CAP];
            if (!ns->valid || ns->tick != ps->next_pop_tick || !ns->popped) {
                break;
            }
            ns->valid = 0u;
            ns->popped = 0u;
            ps->next_pop_tick++;
        }
    }
    return SU_OK;
}

uint32_t su_net_pop_remote_input(SuNetSession *s, uint32_t peer_index, uint64_t tick,
                                 void *buf, size_t cap, size_t *out_len, uint8_t *out_available) {
    return read_remote_input(s, peer_index, tick, buf, cap, out_len, out_available, 1);
}

uint32_t su_net_peek_remote_input(const SuNetSession *s, uint32_t peer_index, uint64_t tick,
                                  void *buf, size_t cap, size_t *out_len, uint8_t *out_available) {
    return read_remote_input((SuNetSession *)(uintptr_t)s, peer_index, tick, buf, cap, out_len, out_available, 0);
}

uint32_t su_net_send_checksum(SuNetSession *s, uint64_t tick, uint64_t digest) {
    size_t idx;
    uint32_t p;

    if (s == NULL || tick == SU_NET_TICK_NONE) {
        return SU_E_INVALID_ARGUMENT;
    }
    idx = (size_t)(tick % SU_NET_CHECKSUM_RING_CAP);
    s->local_checksums[idx].tick = tick;
    s->local_checksums[idx].digest = digest;
    s->local_checksums[idx].valid = 1u;

    for (p = 0u; p < s->peer_count; p++) {
        SuNetPeerState *ps = &s->peers_state[p];
        if (p == s->local_peer || !s->peers[p].enabled) {
            continue;
        }
        if (ps->checksum_ring[idx].valid && ps->checksum_ring[idx].tick == tick &&
            ps->checksum_ring[idx].digest != digest) {
            record_desync(s, p, tick, digest, ps->checksum_ring[idx].digest);
        }
        if (ps->status == SU_NET_PEER_CONNECTED ||
            (ps->status == SU_NET_PEER_CONNECTING && ps->rx_hello)) {
            if (send_checksum_to(s, p, tick, digest) == SU_OK) {
                s->stats.checksums_sent++;
            }
        }
    }
    return SU_OK;
}

uint32_t su_net_check_desync(const SuNetSession *s, SuNetDesyncInfo *out) {
    if (s == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = s->desync;
    return SU_OK;
}

uint32_t su_net_send_bye(SuNetSession *s, uint32_t reason) {
    uint64_t last_tick;
    uint32_t p;

    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    last_tick = (s->local_highest_tick == SU_NET_TICK_NONE) ? 0u : s->local_highest_tick;
    for (p = 0u; p < s->peer_count; p++) {
        if (p == s->local_peer || !s->peers[p].enabled) {
            continue;
        }
        (void)send_bye_to(s, p, last_tick, reason);
    }
    return SU_OK;
}

uint32_t su_net_poll(SuNetSession *s) {
    SuNetAddr from;
    uint32_t n;
    uint32_t p;

    if (s == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }

    for (n = 0u; n < SU_NET_POLL_MAX_PACKETS; n++) {
        size_t pkt_len = 0u;
        uint32_t st;
        memset(&from, 0, sizeof from);
        st = s->transport.recv_fn(s->transport.user, &from, s->scratch_pkt, sizeof s->scratch_pkt, &pkt_len);
        if (st == SU_E_RESOURCE_LIMIT) {
            s->stats.packets_malformed++;
            continue;
        }
        if (st != SU_OK) {
            return st;
        }
        if (pkt_len == 0u) {
            break;
        }
        s->stats.packets_recv++;
        s->stats.bytes_recv += (uint64_t)pkt_len;
        process_packet(s, s->scratch_pkt, pkt_len);
    }

    for (p = 0u; p < s->peer_count; p++) {
        SuNetPeerState *ps = &s->peers_state[p];
        if (p == s->local_peer || !s->peers[p].enabled) {
            continue;
        }
        if (ps->tx_welcome_needed) {
            if (send_hello_to(s, p, (uint8_t)SU_NET_MSG_WELCOME) == SU_OK) {
                ps->tx_welcome_needed = 0u;
            }
        }
        if (ps->status == SU_NET_PEER_CONNECTING) {
            if (ps->polls_since_hello == 0u || ps->polls_since_hello >= s->retransmit_polls) {
                if (send_hello_to(s, p, (uint8_t)SU_NET_MSG_HELLO) == SU_OK) {
                    ps->polls_since_hello = 1u;
                }
            } else {
                ps->polls_since_hello++;
            }
        } else if (ps->status == SU_NET_PEER_CONNECTED) {
            int unacked = (s->local_highest_tick != SU_NET_TICK_NONE) &&
                          (ps->remote_ack_tick == SU_NET_TICK_NONE || ps->remote_ack_tick < s->local_highest_tick);
            if (unacked && ps->polls_since_tx >= s->retransmit_polls) {
                (void)send_input_to(s, p);
            } else if (ps->ack_dirty) {
                (void)send_input_to(s, p);
            } else if (unacked) {
                ps->polls_since_tx++;
            }
        }
    }

    return SU_OK;
}

uint8_t su_net_is_ready(const SuNetSession *s) {
    uint32_t p;
    if (s == NULL) {
        return 0u;
    }
    for (p = 0u; p < s->peer_count; p++) {
        if (p == s->local_peer || !s->peers[p].enabled) {
            continue;
        }
        if (s->peers_state[p].status != SU_NET_PEER_CONNECTED) {
            return 0u;
        }
    }
    return 1u;
}

uint32_t su_net_peer_info(const SuNetSession *s, uint32_t peer_index, SuNetPeerInfo *out) {
    const SuNetPeerState *ps;
    if (s == NULL || out == NULL || peer_index >= s->peer_count) {
        return SU_E_INVALID_ARGUMENT;
    }
    ps = &s->peers_state[peer_index];
    out->local_ack_tick = ps->local_ack_tick;
    out->remote_ack_tick = ps->remote_ack_tick;
    out->highest_rx_tick = ps->highest_rx_tick;
    out->status = ps->status;
    out->bye_reason = ps->bye_reason;
    return SU_OK;
}

uint32_t su_net_stats(const SuNetSession *s, SuNetStats *out) {
    if (s == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = s->stats;
    return SU_OK;
}
