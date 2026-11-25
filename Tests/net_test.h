/* net_test.h - helpers shared by the tests that run real sockets.
 * SPDX-License-Identifier: MIT */
#ifndef DX_TEST_NET_H
#define DX_TEST_NET_H

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Duplex/duplex.h"
#include "check.h"
#include "dx_packets.h"

/*
 * Tests never sleep for a fixed time: they poll the contexts in a loop and stop as soon as the
 * awaited condition is true (or after a generous deadline, which only matters when it fails).
 * Ports are never hard-coded: servers listen on port 0 and tests read the chosen port back.
 */

/* Backend used by the runtime tests: DX_TEST_BACKEND=<n> (a DX_BACKEND_* value) overrides the
 * default, so the same suite can be replayed on poll, epoll, kqueue or io_uring. */
static inline uint32_t test_backend(void)
{
    const char* v = getenv("DX_TEST_BACKEND");
    return v ? (uint32_t)atoi(v) : DX_BACKEND_AUTO;
}

static inline uint64_t test_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* What a test remembers about one side (client or server) of a conversation. */
typedef struct side {
    dx_ctx* ctx;
    uint32_t conn; /* most recent connection handle seen */
    int connected, disconnected;
    int32_t reason; /* reason of the last disconnect */
    unsigned pings; /* pings received, all checked to arrive in order */
    unsigned pongs, uploads, downloads, other;
    uint32_t seq_base; /* sequence number of the first ping counted in `pings` */
    unsigned bad;      /* anything unexpected: out-of-order nonce, wrong sequence, bad data */
    int reply;         /* server only: answer every ping with a pong */
} side;

static inline int32_t read_reason(const dx_packet* p)
{
    return (int32_t)((uint32_t)p->data[0] | (uint32_t)p->data[1] << 8 | (uint32_t)p->data[2] << 16 |
                     (uint32_t)p->data[3] << 24);
}

/* Blob content used by the big-transfer tests: byte i is (i * 7) modulo 256. */
static inline int blob_ok(const dx_packet* p, uint32_t expect_len)
{
    dx_reader r;
    uint32_t n;
    dx_reader_init(&r, p->data, p->length);
    const uint8_t* b = dx_r_bytes(&r, &n);
    if (r.err || n != expect_len) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] != (uint8_t)(i * 7u)) return 0;
    return 1;
}

/* Default reaction to every packet; the server also answers pings when s->reply is set. */
static inline void on_packet(void* user, const dx_packet* p)
{
    side* s = user;
    switch (p->packet_id) {
    case DX_SYS_CONNECTED:
        s->connected++;
        s->conn = p->conn;
        break;
    case DX_SYS_DISCONNECTED:
        s->disconnected++;
        s->reason = read_reason(p);
        break;
    case DX_PKT_PING: {
        dx_reader r;
        dx_reader_init(&r, p->data, p->length);
        uint32_t nonce = dx_r_u32(&r);
        if (s->pings == 0) s->seq_base = p->sequence; /* first ping of a run */
        if (nonce != s->pings || p->sequence != s->seq_base + s->pings) s->bad++; /* order */
        s->pings++;
        if (s->reply) {
            uint8_t b[4];
            dx_writer w;
            dx_writer_init(&w, b, sizeof b);
            dx_w_u32(&w, nonce);
            (void)dx_send(s->ctx, p->conn, DX_PKT_PONG, b, w.len);
        }
        break;
    }
    case DX_PKT_PONG:
        s->pongs++;
        break;
    case DX_PKT_UPLOAD:
        s->uploads++;
        s->bad += !blob_ok(p, 1u << 20);
        break;
    case DX_PKT_DOWNLOAD:
        s->downloads++;
        s->bad += !blob_ok(p, 1u << 20);
        break;
    default:
        s->other++;
    }
}

/* One round for a side: poll briefly, handle everything, release. */
static inline void pump_side(side* s, int timeout_ms)
{
    dx_packet pk[64];
    int n = dx_poll(s->ctx, pk, 64, timeout_ms);
    for (int i = 0; i < n; i++)
        on_packet(s, &pk[i]);
    if (n > 0) dx_release(s->ctx, n);
}

/* Runs both sides until `cond` holds or 10 s pass; the caller then CHECKs `cond` itself. */
#define WAIT_UNTIL(S, C, cond)                                                                     \
    do {                                                                                           \
        uint64_t t0_ = test_now_ms();                                                              \
        while (!(cond) && test_now_ms() - t0_ < 10000) {                                           \
            pump_side(&(S), 1);                                                                    \
            pump_side(&(C), 1);                                                                    \
        }                                                                                          \
    } while (0)

/* Same, for a test with a single library side (the peer is a raw socket). */
#define WAIT_SIDE(S, cond)                                                                         \
    do {                                                                                           \
        uint64_t t0_ = test_now_ms();                                                              \
        while (!(cond) && test_now_ms() - t0_ < 10000)                                             \
            pump_side(&(S), 1);                                                                    \
    } while (0)

#endif
