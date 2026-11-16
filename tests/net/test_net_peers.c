/* End-to-end 3-peer deterministic simulation tests for Sutekh::net + Sutekh::host:
 *   (a) 3 headless SuHostWorld peers (0, 1, 2) with su_host_input_encode / su_host_input_push_encoded /
 *       rollback (su_host_snapshot_restore_inplace + su_host_input_clear) over a deterministic fault-injected
 *       virtual transport (packet loss, duplication, variable delay, out-of-order delivery) -> identical
 *       32-byte su_host_digest across all 3 peers, plus explicit desync detection check when one peer mutates state.
 *   (b) Real 127.0.0.1 UDP loopback socket test exchanging packets across 3 SuNetSession sockets. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

#include "sutekh/host.h"
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

#define NUM_PEERS 3u
#define F_POS 1u
#define F_VEL 2u
#define TOTAL_FAULT_TICKS 60u
#define LOOKAHEAD_TICKS 4u
#define TOTAL_UDP_TICKS 20u

typedef struct Bytes {
    uint8_t *data;
    size_t len;
} Bytes;

static Bytes slurp(const char *path) {
    Bytes b = {NULL, 0u};
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    b.data = malloc((size_t)n + 1u);
    if (b.data == NULL || fread(b.data, 1u, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    b.len = (size_t)n;
    return b;
}

static void sleep_1ms(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    (void)nanosleep(&ts, NULL);
#endif
}

static SuHostCapsule g_move_cap;

static SuHostWorld *make_world(void) {
    SuHostWorldDesc d;
    SuHostWorld *w = NULL;
    SuHostError e;
    uint32_t st;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.capsule_count = 1u;
    d.capsules = &g_move_cap;
    d.max_entities = 4u;
    d.tick_rate_hz = 60u;
    d.event_log_max = 64u;
    d.seed = 42u;
    st = su_host_world_create(&d, &w, &e);
    if (st != SU_OK) {
        fprintf(stderr, "su_host_world_create failed: %s %s\n", su_host_status_name(st), e.message);
        exit(1);
    }
    return w;
}

static void spawn_players(SuHostWorld *w, uint64_t ents[NUM_PEERS]) {
    uint32_t p;
    for (p = 0u; p < NUM_PEERS; p++) {
        SuHostValue v[2];
        SuHostError e;
        v[0].field_id = F_POS;
        v[0].value = (p + 1u) * 100u;
        v[1].field_id = F_VEL;
        v[1].value = 0u;
        CHECK(su_host_spawn(w, v, 2u, p + 1u, &ents[p], &e) == SU_OK);
    }
}

static uint32_t input_vel_for(uint32_t peer, uint64_t tick) {
    return (peer + 1u) * 10u + (uint32_t)((tick * 3u + peer) % 11u) + 1u;
}

/* ---- deterministic fault-injected virtual transport ---- */

#define MAX_INFLIGHT 1024u

typedef struct InflightPkt {
    uint8_t active;
    uint32_t src_peer;
    uint32_t dst_peer;
    uint64_t seq;
    uint64_t deliver_step;
    size_t len;
    uint8_t data[SU_NET_MAX_PACKET_BYTES];
} InflightPkt;

typedef struct FaultNet {
    InflightPkt pkts[MAX_INFLIGHT];
    uint64_t step;
    uint64_t next_seq;
    uint64_t last_rx_seq[NUM_PEERS][NUM_PEERS];
    uint32_t rng;
    uint64_t sent;
    uint64_t dropped;
    uint64_t duplicated;
    uint64_t reordered;
} FaultNet;

typedef struct FaultEnd {
    FaultNet *net;
    uint32_t peer;
} FaultEnd;

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fault_enqueue(FaultNet *net, uint32_t src, uint32_t dst, uint64_t seq, uint64_t deliver_step,
                          const void *data, size_t len) {
    uint32_t i;
    for (i = 0u; i < MAX_INFLIGHT; i++) {
        if (!net->pkts[i].active) {
            net->pkts[i].active = 1u;
            net->pkts[i].src_peer = src;
            net->pkts[i].dst_peer = dst;
            net->pkts[i].seq = seq;
            net->pkts[i].deliver_step = deliver_step;
            net->pkts[i].len = len;
            memcpy(net->pkts[i].data, data, len);
            return;
        }
    }
}

static uint32_t fault_send(void *user, const SuNetAddr *to, const void *data, size_t len) {
    FaultEnd *end = (FaultEnd *)user;
    FaultNet *net = end->net;
    uint32_t dst = to->peer_tag;
    uint32_t r = xorshift32(&net->rng);
    uint64_t seq = net->next_seq++;

    net->sent++;
    if ((r % 5u) == 0u) {
        /* 20% packet loss */
        net->dropped++;
        return SU_OK;
    }
    {
        uint64_t delay = 1u + (uint64_t)(xorshift32(&net->rng) % 4u); /* 1..4 steps -> out-of-order */
        fault_enqueue(net, end->peer, dst, seq, net->step + delay, data, len);
    }
    if ((xorshift32(&net->rng) % 7u) == 0u) {
        /* ~14% duplication */
        uint64_t dup_delay = 1u + (uint64_t)(xorshift32(&net->rng) % 5u);
        fault_enqueue(net, end->peer, dst, seq, net->step + dup_delay, data, len);
        net->duplicated++;
    }
    return SU_OK;
}

static uint32_t fault_recv(void *user, SuNetAddr *from, void *buf, size_t cap, size_t *out_len) {
    FaultEnd *end = (FaultEnd *)user;
    FaultNet *net = end->net;
    uint32_t i;

    *out_len = 0u;
    for (i = 0u; i < MAX_INFLIGHT; i++) {
        InflightPkt *p = &net->pkts[i];
        if (p->active && p->dst_peer == end->peer && p->deliver_step <= net->step) {
            if (p->len > cap) {
                p->active = 0u;
                return SU_E_RESOURCE_LIMIT;
            }
            memcpy(buf, p->data, p->len);
            *out_len = p->len;
            if (from != NULL) {
                memset(from, 0, sizeof *from);
                from->peer_tag = p->src_peer;
            }
            if (p->seq < net->last_rx_seq[end->peer][p->src_peer]) {
                net->reordered++;
            } else {
                net->last_rx_seq[end->peer][p->src_peer] = p->seq;
            }
            p->active = 0u;
            return SU_OK;
        }
    }
    return SU_OK;
}

typedef struct PeerBlob {
    uint8_t valid;
    size_t len;
    uint8_t bytes[SU_NET_MAX_INPUT_BYTES];
} PeerBlob;

typedef struct PeerSim {
    SuHostWorld *sim;
    SuHostWorld *enc;
    uint64_t ents[NUM_PEERS];
    uint64_t enc_ents[NUM_PEERS];
    SuNetSession *sess;
    uint64_t sim_tick;
    uint64_t confirmed_tick;
    uint8_t *confirmed_snap;
    size_t confirmed_snap_len;
    size_t confirmed_snap_cap;
    PeerBlob blobs[NUM_PEERS][TOTAL_FAULT_TICKS + 8u];
} PeerSim;

static void save_snapshot(PeerSim *ps) {
    size_t need = 0u;
    CHECK(su_host_snapshot_size(ps->sim, &need) == SU_OK);
    if (need > ps->confirmed_snap_cap) {
        uint8_t *nb = realloc(ps->confirmed_snap, need);
        CHECK(nb != NULL);
        ps->confirmed_snap = nb;
        ps->confirmed_snap_cap = need;
    }
    CHECK(su_host_snapshot_write(ps->sim, ps->confirmed_snap, ps->confirmed_snap_cap, &ps->confirmed_snap_len) == SU_OK);
}

static void push_tick_inputs(PeerSim *ps, uint64_t tick, int use_prediction) {
    uint32_t k;
    SuHostError err;
    for (k = 0u; k < NUM_PEERS; k++) {
        const PeerBlob *pb = &ps->blobs[k][tick];
        if (pb->valid) {
            CHECK(su_host_input_push_encoded(ps->sim, pb->bytes, pb->len, &err) == SU_OK);
        } else if (use_prediction) {
            SuHostInput w;
            w.entity = ps->ents[k];
            w.field_id = F_VEL;
            w.value = 0u; /* prediction that diverges from actual non-zero velocity */
            CHECK(su_host_input_push(ps->sim, tick, &w, 1u, &err) == SU_OK);
        }
    }
}

static void test_fault_injected_3peers_and_desync(void) {
    FaultNet net;
    FaultEnd ends[NUM_PEERS];
    PeerSim peers[NUM_PEERS];
    uint8_t world_id[32];
    uint8_t content_hash[32];
    uint32_t p;
    uint32_t iter;
    uint64_t rollbacks = 0u;

    memset(&net, 0, sizeof net);
    net.rng = 0x5EED2026u;
    memset(content_hash, 0x3Cu, sizeof content_hash);

    for (p = 0u; p < NUM_PEERS; p++) {
        memset(&peers[p], 0, sizeof peers[p]);
        peers[p].sim = make_world();
        peers[p].enc = make_world();
        spawn_players(peers[p].sim, peers[p].ents);
        spawn_players(peers[p].enc, peers[p].enc_ents);
        /* Trigger lazy first-tick entity cache allocation before rollback measurement. */
        {
            uint8_t snap0[4096];
            size_t snap0_len = 0u;
            SuHostError e;
            CHECK(su_host_snapshot_write(peers[p].sim, snap0, sizeof snap0, &snap0_len) == SU_OK);
            CHECK(su_host_snapshot_restore_inplace(peers[p].sim, snap0, snap0_len, &e) == SU_OK);
        }
        save_snapshot(&peers[p]);
    }
    CHECK(su_host_world_identity(peers[0].sim, world_id) == SU_OK);

    for (p = 0u; p < NUM_PEERS; p++) {
        SuNetSessionDesc d;
        uint32_t k;
        ends[p].net = &net;
        ends[p].peer = p;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.local_peer = p;
        d.peer_count = NUM_PEERS;
        d.tick_rate_hz = 60u;
        d.session_salt = UINT64_C(0x1122334455667788);
        memcpy(d.schema_hash, world_id, 32u);
        memcpy(d.content_hash, content_hash, 32u);
        d.redundancy = SU_NET_MAX_REDUNDANCY;
        d.retransmit_polls = 2u;
        d.transport.send_fn = fault_send;
        d.transport.recv_fn = fault_recv;
        d.transport.user = &ends[p];
        for (k = 0u; k < NUM_PEERS; k++) {
            d.peers[k].enabled = 1u;
            d.peers[k].addr.peer_tag = k;
        }
        CHECK(su_net_session_create(&d, &peers[p].sess) == SU_OK);
    }

    /* 1. Handshake under packet loss, delay, duplication, and reordering. */
    for (iter = 0u; iter < 120u; iter++) {
        int all_ready = 1;
        net.step++;
        for (p = 0u; p < NUM_PEERS; p++) {
            CHECK(su_net_poll(peers[p].sess) == SU_OK);
            if (!su_net_is_ready(peers[p].sess)) {
                all_ready = 0;
            }
        }
        if (all_ready) {
            break;
        }
    }
    for (p = 0u; p < NUM_PEERS; p++) {
        CHECK(su_net_is_ready(peers[p].sess) == 1u);
    }

    /* 2. Step 3 peers with speculative prediction + rollback on confirmed remote inputs. */
    for (iter = 0u; iter < 1500u; iter++) {
        int all_done = 1;

        for (p = 0u; p < NUM_PEERS; p++) {
            PeerSim *ps = &peers[p];
            if (ps->sim_tick < TOTAL_FAULT_TICKS && (ps->sim_tick - ps->confirmed_tick) < LOOKAHEAD_TICKS) {
                uint64_t t = ps->sim_tick;
                SuHostInput w;
                SuHostError err;
                uint64_t done = 0u;
                size_t enc_len = 0u;

                w.entity = ps->enc_ents[p];
                w.field_id = F_VEL;
                w.value = input_vel_for(p, t);
                CHECK(su_host_input_push(ps->enc, t, &w, 1u, &err) == SU_OK);
                CHECK(su_host_input_encode(ps->enc, t, ps->blobs[p][t].bytes, sizeof ps->blobs[p][t].bytes,
                                           &enc_len) == SU_OK);
                CHECK(su_host_step(ps->enc, 1u, &done, &err) == SU_OK && done == 1u);
                ps->blobs[p][t].len = enc_len;
                ps->blobs[p][t].valid = 1u;
                CHECK(su_net_send_input(ps->sess, t, ps->blobs[p][t].bytes, enc_len) == SU_OK);

                push_tick_inputs(ps, t, 1);
                CHECK(su_host_step(ps->sim, 1u, &done, &err) == SU_OK && done == 1u);
                ps->sim_tick++;
            }
        }

        net.step++;
        for (p = 0u; p < NUM_PEERS; p++) {
            PeerSim *ps = &peers[p];
            uint32_t k;
            uint64_t t;
            int got_confirmed_at_head = 1;

            CHECK(su_net_poll(ps->sess) == SU_OK);

            for (k = 0u; k < NUM_PEERS; k++) {
                if (k == p) {
                    continue;
                }
                for (t = ps->confirmed_tick; t < TOTAL_FAULT_TICKS; t++) {
                    if (!ps->blobs[k][t].valid) {
                        uint8_t avail = 0u;
                        size_t got_len = 0u;
                        CHECK(su_net_pop_remote_input(ps->sess, k, t, ps->blobs[k][t].bytes,
                                                      sizeof ps->blobs[k][t].bytes, &got_len, &avail) == SU_OK);
                        if (avail) {
                            ps->blobs[k][t].len = got_len;
                            ps->blobs[k][t].valid = 1u;
                        }
                    }
                }
            }

            for (k = 0u; k < NUM_PEERS; k++) {
                if (!ps->blobs[k][ps->confirmed_tick].valid) {
                    got_confirmed_at_head = 0;
                    break;
                }
            }

            if (ps->confirmed_tick < ps->sim_tick && got_confirmed_at_head) {
                SuHostError err;
                uint64_t target = ps->sim_tick;
                uint64_t done = 0u;

                /* Rollback to confirmed snapshot, apply all newly confirmed ticks, save new snapshot,
                 * then re-simulate remaining speculative ticks. */
                CHECK(su_host_snapshot_restore_inplace(ps->sim, ps->confirmed_snap, ps->confirmed_snap_len, &err) == SU_OK);
                su_host_input_clear(ps->sim, ps->confirmed_tick);
                rollbacks++;

                while (ps->confirmed_tick < target) {
                    int all_k = 1;
                    for (k = 0u; k < NUM_PEERS; k++) {
                        if (!ps->blobs[k][ps->confirmed_tick].valid) {
                            all_k = 0;
                            break;
                        }
                    }
                    if (!all_k) {
                        break;
                    }
                    push_tick_inputs(ps, ps->confirmed_tick, 0);
                    CHECK(su_host_step(ps->sim, 1u, &done, &err) == SU_OK && done == 1u);
                    ps->confirmed_tick++;
                    if ((ps->confirmed_tick % 4u) == 0u || ps->confirmed_tick == TOTAL_FAULT_TICKS) {
                        uint64_t fast_d = 0u;
                        CHECK(su_host_digest_fast(ps->sim, &fast_d) == SU_OK);
                        CHECK(su_net_send_checksum(ps->sess, ps->confirmed_tick, fast_d) == SU_OK);
                    }
                }
                save_snapshot(ps);

                for (t = ps->confirmed_tick; t < target; t++) {
                    push_tick_inputs(ps, t, 1);
                    CHECK(su_host_step(ps->sim, 1u, &done, &err) == SU_OK && done == 1u);
                }
            }

            if (ps->confirmed_tick < TOTAL_FAULT_TICKS) {
                all_done = 0;
            }
        }

        if (all_done) {
            break;
        }
    }

    /* Drain trailing ACK/CHECKSUM packets. */
    for (iter = 0u; iter < 20u; iter++) {
        net.step++;
        for (p = 0u; p < NUM_PEERS; p++) {
            CHECK(su_net_poll(peers[p].sess) == SU_OK);
        }
    }

    CHECK(net.dropped > 0u);
    CHECK(net.duplicated > 0u);
    CHECK(net.reordered > 0u);
    CHECK(rollbacks > 0u);

    {
        uint8_t d[NUM_PEERS][32];
        for (p = 0u; p < NUM_PEERS; p++) {
            SuNetDesyncInfo di;
            CHECK(peers[p].confirmed_tick == TOTAL_FAULT_TICKS);
            CHECK(su_host_tick(peers[p].sim) == TOTAL_FAULT_TICKS);
            CHECK(su_host_digest(peers[p].sim, d[p]) == SU_OK);
            CHECK(su_net_check_desync(peers[p].sess, &di) == SU_OK && di.detected == 0u);
        }
        CHECK(memcmp(d[0], d[1], 32u) == 0);
        CHECK(memcmp(d[0], d[2], 32u) == 0);
    }

    /* 3. Explicit desync detection: peer 1 mutates state asymmetrically, checksum exchange catches it. */
    {
        SuHostInput bad;
        SuHostError err;
        uint64_t done = 0u;
        uint32_t step_i;
        bad.entity = peers[1].ents[1];
        bad.field_id = F_VEL;
        bad.value = 99999u;
        CHECK(su_host_input_push(peers[1].sim, TOTAL_FAULT_TICKS, &bad, 1u, &err) == SU_OK);

        for (step_i = 0u; step_i < 4u; step_i++) {
            uint64_t t_after = TOTAL_FAULT_TICKS + (uint64_t)step_i + 1u;
            for (p = 0u; p < NUM_PEERS; p++) {
                uint8_t sha[32];
                CHECK(su_host_step(peers[p].sim, 1u, &done, &err) == SU_OK && done == 1u);
                CHECK(su_host_digest(peers[p].sim, sha) == SU_OK);
                CHECK(su_net_send_checksum(peers[p].sess, t_after, su_net_digest64_from_sha256(sha)) == SU_OK);
            }
        }
        for (iter = 0u; iter < 30u; iter++) {
            net.step++;
            for (p = 0u; p < NUM_PEERS; p++) {
                CHECK(su_net_poll(peers[p].sess) == SU_OK);
            }
        }
        {
            SuNetDesyncInfo di0, di1, di2;
            CHECK(su_net_check_desync(peers[0].sess, &di0) == SU_OK && di0.detected == 1u &&
                  di0.peer_index == 1u && di0.local_digest != di0.remote_digest);
            CHECK(su_net_check_desync(peers[2].sess, &di2) == SU_OK && di2.detected == 1u &&
                  di2.peer_index == 1u && di2.local_digest != di2.remote_digest);
            CHECK(su_net_check_desync(peers[1].sess, &di1) == SU_OK && di1.detected == 1u &&
                  di1.local_digest != di1.remote_digest);
        }
    }

    for (p = 0u; p < NUM_PEERS; p++) {
        su_net_session_destroy(peers[p].sess);
        su_host_world_destroy(peers[p].sim);
        su_host_world_destroy(peers[p].enc);
        free(peers[p].confirmed_snap);
    }
}

static void test_udp_loopback_3peers(void) {
    SuNetUdpSocket *socks[NUM_PEERS];
    SuNetAddr addrs[NUM_PEERS];
    SuNetSession *sess[NUM_PEERS];
    SuHostWorld *sims[NUM_PEERS];
    SuHostWorld *encs[NUM_PEERS];
    uint64_t ents[NUM_PEERS][NUM_PEERS];
    uint64_t enc_ents[NUM_PEERS][NUM_PEERS];
    uint8_t world_id[32];
    uint8_t content_hash[32];
    uint32_t p;
    uint32_t iter;
    uint64_t t;

    memset(socks, 0, sizeof socks);
    memset(sess, 0, sizeof sess);
    memset(content_hash, 0x5Au, sizeof content_hash);

    for (p = 0u; p < NUM_PEERS; p++) {
        SuNetAddr bind_lo;
        CHECK(su_net_addr_ipv4(127u, 0u, 0u, 1u, 0u, &bind_lo) == SU_OK);
        CHECK(su_net_udp_open(&bind_lo, &socks[p], &addrs[p]) == SU_OK);
        CHECK(addrs[p].port != 0u);
        sims[p] = make_world();
        encs[p] = make_world();
        spawn_players(sims[p], ents[p]);
        spawn_players(encs[p], enc_ents[p]);
    }
    CHECK(su_host_world_identity(sims[0], world_id) == SU_OK);

    for (p = 0u; p < NUM_PEERS; p++) {
        SuNetSessionDesc d;
        uint32_t k;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.local_peer = p;
        d.peer_count = NUM_PEERS;
        d.tick_rate_hz = 60u;
        d.session_salt = UINT64_C(0xA1B2C3D4E5F60708);
        memcpy(d.schema_hash, world_id, 32u);
        memcpy(d.content_hash, content_hash, 32u);
        d.redundancy = SU_NET_MAX_REDUNDANCY;
        d.retransmit_polls = 2u;
        d.transport = su_net_udp_transport(socks[p]);
        for (k = 0u; k < NUM_PEERS; k++) {
            d.peers[k].enabled = 1u;
            d.peers[k].addr = addrs[k];
        }
        CHECK(su_net_session_create(&d, &sess[p]) == SU_OK);
    }

    /* Handshake over real 127.0.0.1 UDP sockets. */
    for (iter = 0u; iter < 500u; iter++) {
        int all_ready = 1;
        for (p = 0u; p < NUM_PEERS; p++) {
            CHECK(su_net_poll(sess[p]) == SU_OK);
            if (!su_net_is_ready(sess[p])) {
                all_ready = 0;
            }
        }
        if (all_ready) {
            break;
        }
        sleep_1ms();
    }
    for (p = 0u; p < NUM_PEERS; p++) {
        CHECK(su_net_is_ready(sess[p]) == 1u);
    }

    /* Step TOTAL_UDP_TICKS over real UDP loopback. */
    for (t = 0u; t < TOTAL_UDP_TICKS; t++) {
        uint8_t local_blobs[NUM_PEERS][SU_NET_MAX_INPUT_BYTES];
        size_t local_lens[NUM_PEERS];
        uint8_t got_remote[NUM_PEERS][NUM_PEERS];

        memset(got_remote, 0, sizeof got_remote);
        for (p = 0u; p < NUM_PEERS; p++) {
            SuHostInput w;
            SuHostError err;
            uint64_t done = 0u;
            w.entity = enc_ents[p][p];
            w.field_id = F_VEL;
            w.value = input_vel_for(p, t);
            CHECK(su_host_input_push(encs[p], t, &w, 1u, &err) == SU_OK);
            CHECK(su_host_input_encode(encs[p], t, local_blobs[p], sizeof local_blobs[p], &local_lens[p]) == SU_OK);
            CHECK(su_host_step(encs[p], 1u, &done, &err) == SU_OK && done == 1u);
            CHECK(su_net_send_input(sess[p], t, local_blobs[p], local_lens[p]) == SU_OK);
            CHECK(su_host_input_push_encoded(sims[p], local_blobs[p], local_lens[p], &err) == SU_OK);
            got_remote[p][p] = 1u;
        }

        for (iter = 0u; iter < 500u; iter++) {
            int all_got = 1;
            for (p = 0u; p < NUM_PEERS; p++) {
                uint32_t k;
                CHECK(su_net_poll(sess[p]) == SU_OK);
                for (k = 0u; k < NUM_PEERS; k++) {
                    if (!got_remote[p][k]) {
                        uint8_t rx_buf[SU_NET_MAX_INPUT_BYTES];
                        size_t rx_len = 0u;
                        uint8_t avail = 0u;
                        CHECK(su_net_pop_remote_input(sess[p], k, t, rx_buf, sizeof rx_buf, &rx_len, &avail) == SU_OK);
                        if (avail) {
                            SuHostError err;
                            CHECK(su_host_input_push_encoded(sims[p], rx_buf, rx_len, &err) == SU_OK);
                            got_remote[p][k] = 1u;
                        } else {
                            all_got = 0;
                        }
                    }
                }
            }
            if (all_got) {
                break;
            }
            sleep_1ms();
        }

        for (p = 0u; p < NUM_PEERS; p++) {
            uint64_t done = 0u;
            uint64_t d64 = 0u;
            SuHostError err;
            uint32_t k;
            for (k = 0u; k < NUM_PEERS; k++) {
                CHECK(got_remote[p][k] == 1u);
            }
            CHECK(su_host_step(sims[p], 1u, &done, &err) == SU_OK && done == 1u);
            CHECK(su_host_digest_fast(sims[p], &d64) == SU_OK);
            CHECK(su_net_send_checksum(sess[p], t + 1u, d64) == SU_OK);
        }
    }

    /* Send BYE and drain. */
    for (p = 0u; p < NUM_PEERS; p++) {
        CHECK(su_net_send_bye(sess[p], SU_NET_BYE_NORMAL) == SU_OK);
    }
    for (iter = 0u; iter < 50u; iter++) {
        int all_bye = 1;
        for (p = 0u; p < NUM_PEERS; p++) {
            uint32_t k;
            CHECK(su_net_poll(sess[p]) == SU_OK);
            for (k = 0u; k < NUM_PEERS; k++) {
                if (k != p) {
                    SuNetPeerInfo pi;
                    CHECK(su_net_peer_info(sess[p], k, &pi) == SU_OK);
                    if (pi.status != SU_NET_PEER_BYE) {
                        all_bye = 0;
                    }
                }
            }
        }
        if (all_bye) {
            break;
        }
        sleep_1ms();
    }

    {
        uint8_t d[NUM_PEERS][32];
        for (p = 0u; p < NUM_PEERS; p++) {
            SuNetDesyncInfo di;
            uint32_t k;
            CHECK(su_host_tick(sims[p]) == TOTAL_UDP_TICKS);
            CHECK(su_host_digest(sims[p], d[p]) == SU_OK);
            CHECK(su_net_check_desync(sess[p], &di) == SU_OK && di.detected == 0u);
            for (k = 0u; k < NUM_PEERS; k++) {
                if (k != p) {
                    SuNetPeerInfo pi;
                    CHECK(su_net_peer_info(sess[p], k, &pi) == SU_OK && pi.status == SU_NET_PEER_BYE);
                }
            }
        }
        CHECK(memcmp(d[0], d[1], 32u) == 0);
        CHECK(memcmp(d[0], d[2], 32u) == 0);
    }

    for (p = 0u; p < NUM_PEERS; p++) {
        su_net_session_destroy(sess[p]);
        su_net_udp_close(socks[p]);
        su_host_world_destroy(sims[p]);
        su_host_world_destroy(encs[p]);
    }
}

int main(void) {
    Bytes cap = slurp("examples/movement.capsule.json");
    Bytes lir = slurp("examples/movement.lir.json");
    g_move_cap.capsule_json = cap.data;
    g_move_cap.capsule_len = cap.len;
    g_move_cap.lir_json = lir.data;
    g_move_cap.lir_len = lir.len;

    test_fault_injected_3peers_and_desync();
    test_udp_loopback_3peers();

    free(cap.data);
    free(lir.data);
    printf("test_net_peers: %ld checks, %ld failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
