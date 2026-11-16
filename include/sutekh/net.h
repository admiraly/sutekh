/* Sutekh UDP peer session & deterministic input/checksum sync (generation 1).
 *
 * Host-side networking library (`su_net` / `Sutekh::net`): sandboxed VM workers and `su_agent` never link to it
 * (ENGINE_CONSTITUTION C07/C11). Fixed-capacity session storage is allocated once in `su_net_session_create`;
 * `su_net_poll`, `su_net_send_input`, `su_net_pop_remote_input`, `su_net_send_checksum`, `su_net_check_desync`,
 * and `su_net_stats` never allocate.
 *
 * Wire protocol v1 (little-endian framing, magic "SUNET1\0\0", version 1):
 *   HELLO / WELCOME (96 bytes): session_salt, peer_index, peer_count, schema_hash[32], content_hash[32],
 *                               tick_rate_hz, flags. Mismatched hashes or tick rates reject the peer.
 *   INPUT           (52 + frames): first_tick, last_tick, ack_tick, frame_count, up to SU_NET_MAX_REDUNDANCY
 *                               frames (u64 tick, u16 len, bytes[len]), e.g. canonical `su_host_input_encode`
 *                               records up to SU_NET_MAX_INPUT_BYTES per frame.
 *   CHECKSUM        (40 bytes): (tick, u64 digest) for periodic deterministic desync detection.
 *   BYE             (40 bytes): clean disconnect with last_tick and reason.
 *
 * Transport: built-in non-blocking UDP backend (`su_net_udp_open` / `su_net_udp_close`) or caller-supplied
 * virtual `SuNetTransport` (`send_fn`, `recv_fn`, `user`) for deterministic delay, loss, duplication, and
 * out-of-order testing. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_NET_H
#define SUTEKH_NET_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_NET_API

#define SU_NET_API_VERSION 1u
#define SU_NET_WIRE_VERSION 1u
#define SU_NET_MAGIC "SUNET1\0\0"
#define SU_NET_MAGIC_LEN 8u

#define SU_NET_MAX_PEERS 8u
#define SU_NET_MAX_INPUT_BYTES 512u
#define SU_NET_MAX_REDUNDANCY 8u
#define SU_NET_INPUT_RING_CAP 256u
#define SU_NET_CHECKSUM_RING_CAP 128u
#define SU_NET_MAX_PACKET_BYTES 4608u

#define SU_NET_COMMON_HEADER_BYTES 24u
#define SU_NET_HELLO_BYTES 96u
#define SU_NET_INPUT_HEADER_BYTES 52u
#define SU_NET_INPUT_FRAME_HEADER_BYTES 10u
#define SU_NET_CHECKSUM_BYTES 40u
#define SU_NET_BYE_BYTES 40u

#define SU_NET_TICK_NONE UINT64_C(0xFFFFFFFFFFFFFFFF)

/* Wire message types. */
#define SU_NET_MSG_HELLO 1u
#define SU_NET_MSG_WELCOME 2u
#define SU_NET_MSG_INPUT 3u
#define SU_NET_MSG_CHECKSUM 4u
#define SU_NET_MSG_BYE 5u

/* Peer lifecycle states. */
#define SU_NET_PEER_DISCONNECTED 0u
#define SU_NET_PEER_CONNECTING 1u
#define SU_NET_PEER_CONNECTED 2u
#define SU_NET_PEER_REJECTED 3u
#define SU_NET_PEER_BYE 4u

/* Disconnect reasons. */
#define SU_NET_BYE_NORMAL 0u
#define SU_NET_BYE_SCHEMA_MISMATCH 1u
#define SU_NET_BYE_DESYNC 2u

/* Address families. */
#define SU_NET_AF_NONE 0u
#define SU_NET_AF_IPV4 4u
#define SU_NET_AF_IPV6 6u

typedef struct SuNetAddr {
    uint8_t ip[16];     /* IPv4 in ip[0..3] when family == SU_NET_AF_IPV4; IPv6 in ip[0..15] for IPV6 */
    uint16_t port;      /* host byte order */
    uint8_t family;     /* SU_NET_AF_* */
    uint8_t reserved;   /* 0 */
    uint32_t peer_tag;  /* caller tag for virtual transports */
} SuNetAddr;

/* Non-blocking transport callbacks.
 * send_fn: sends one datagram of len bytes to *to. Returns SU_OK, SU_E_RESOURCE_LIMIT (would block), or SU_E_IO.
 * recv_fn: receives at most one datagram into buf[0..cap]. When no datagram is ready, sets *out_len = 0 and
 *          returns SU_OK. On success with a datagram, sets *out_len = bytes_read and fills *from (if non-NULL). */
typedef uint32_t (*SuNetSendFn)(void *user, const SuNetAddr *to, const void *data, size_t len);
typedef uint32_t (*SuNetRecvFn)(void *user, SuNetAddr *from, void *buf, size_t cap, size_t *out_len);

typedef struct SuNetTransport {
    SuNetSendFn send_fn;
    SuNetRecvFn recv_fn;
    void *user;
} SuNetTransport;

typedef struct SuNetPeerConfig {
    SuNetAddr addr;
    uint8_t enabled;
    uint8_t reserved[7];
} SuNetPeerConfig;

typedef struct SuNetSessionDesc {
    uint32_t struct_size;        /* sizeof(SuNetSessionDesc) */
    uint32_t local_peer;         /* 0 .. peer_count - 1 */
    uint32_t peer_count;         /* 1 .. SU_NET_MAX_PEERS */
    uint32_t tick_rate_hz;       /* > 0 */
    uint64_t session_salt;       /* session identifier; mismatches are rejected */
    uint8_t schema_hash[32];     /* pinned schema / world identity digest */
    uint8_t content_hash[32];    /* pinned content / pack digest */
    uint32_t redundancy;         /* 1 .. SU_NET_MAX_REDUNDANCY (0 -> SU_NET_MAX_REDUNDANCY) */
    uint32_t retransmit_polls;   /* polls between unacked retransmits (0 -> default 2) */
    SuNetTransport transport;    /* send_fn, recv_fn, user */
    SuNetPeerConfig peers[SU_NET_MAX_PEERS];
} SuNetSessionDesc;

typedef struct SuNetStats {
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint64_t packets_malformed;
    uint64_t packets_rejected;
    uint64_t schema_mismatches;
    uint64_t duplicate_frames;
    uint64_t out_of_window_frames;
    uint64_t conflicting_frames;
    uint64_t input_frames_sent;
    uint64_t input_frames_recv;
    uint64_t checksums_sent;
    uint64_t checksums_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} SuNetStats;

typedef struct SuNetDesyncInfo {
    uint64_t tick;
    uint64_t local_digest;
    uint64_t remote_digest;
    uint32_t peer_index;
    uint8_t detected;
    uint8_t reserved[3];
} SuNetDesyncInfo;

typedef struct SuNetPeerInfo {
    uint64_t local_ack_tick;   /* highest contiguous tick received from this peer (SU_NET_TICK_NONE if none) */
    uint64_t remote_ack_tick;  /* highest contiguous tick this peer acknowledged from us (SU_NET_TICK_NONE if none) */
    uint64_t highest_rx_tick;  /* highest tick received from this peer (SU_NET_TICK_NONE if none) */
    uint32_t status;           /* SU_NET_PEER_* */
    uint32_t bye_reason;       /* SU_NET_BYE_* when status == SU_NET_PEER_BYE */
} SuNetPeerInfo;

typedef struct SuNetSession SuNetSession;
typedef struct SuNetUdpSocket SuNetUdpSocket;

SU_NET_API uint32_t su_net_api_version(void);

/* Helper to collapse a 32-byte SHA-256 digest (e.g. su_host_digest) into a 64-bit wire checksum. */
SU_NET_API uint64_t su_net_digest64_from_sha256(const uint8_t sha256[32]);

/* Address constructors. */
SU_NET_API uint32_t su_net_addr_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port, SuNetAddr *out);
SU_NET_API uint32_t su_net_addr_ipv6(const uint8_t ip[16], uint16_t port, SuNetAddr *out);

/* Built-in non-blocking UDP socket backend. Pass bind_addr == NULL for 0.0.0.0:0. */
SU_NET_API uint32_t su_net_udp_open(const SuNetAddr *bind_addr, SuNetUdpSocket **out, SuNetAddr *bound_addr);
SU_NET_API void su_net_udp_close(SuNetUdpSocket *sock);
SU_NET_API SuNetTransport su_net_udp_transport(SuNetUdpSocket *sock);

/* Session lifecycle: allocates fixed-capacity rings once in create, frees in destroy (NULL-safe). */
SU_NET_API uint32_t su_net_session_create(const SuNetSessionDesc *desc, SuNetSession **out);
SU_NET_API void su_net_session_destroy(SuNetSession *s);

/* Zero-allocation runtime operations. */
SU_NET_API uint32_t su_net_send_hello(SuNetSession *s);
SU_NET_API uint32_t su_net_send_input(SuNetSession *s, uint64_t tick, const void *blob, size_t len);
SU_NET_API uint32_t su_net_pop_remote_input(SuNetSession *s, uint32_t peer_index, uint64_t tick,
                                            void *buf, size_t cap, size_t *out_len, uint8_t *out_available);
SU_NET_API uint32_t su_net_peek_remote_input(const SuNetSession *s, uint32_t peer_index, uint64_t tick,
                                             void *buf, size_t cap, size_t *out_len, uint8_t *out_available);
SU_NET_API uint32_t su_net_send_checksum(SuNetSession *s, uint64_t tick, uint64_t digest);
SU_NET_API uint32_t su_net_check_desync(const SuNetSession *s, SuNetDesyncInfo *out);
SU_NET_API uint32_t su_net_send_bye(SuNetSession *s, uint32_t reason);
SU_NET_API uint32_t su_net_poll(SuNetSession *s);

/* Inspection helpers (zero-allocation). */
SU_NET_API uint8_t su_net_is_ready(const SuNetSession *s);
SU_NET_API uint32_t su_net_peer_info(const SuNetSession *s, uint32_t peer_index, SuNetPeerInfo *out);
SU_NET_API uint32_t su_net_stats(const SuNetSession *s, SuNetStats *out);

#ifdef __cplusplus
}
#endif

#endif
