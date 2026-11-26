/* test_stress.c - thousands of simultaneous connections, sustained traffic, memory drift.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <sys/resource.h>

#include "../net_test.h"
#include "IO/net.h"

/*
 * Many clients (one context) talk to one server (another context) over loopback. Every
 * connection sends pings in turn and the server answers each with a pong. We check that all
 * connections come up, every ping gets exactly one pong, the process memory does not keep
 * growing, and report the throughput.
 * Duration: C2S2C_STRESS_SECONDS (default 3). Connections: C2S2C_STRESS_CONNS (default 10000,
 * reduced automatically to what the file-descriptor limit allows: two sockets per connection).
 */
typedef struct {
    c2s2c_ctx* ctx;
    unsigned connected, disconnected;
    uint64_t pings, pongs;
    int reply, bad;
} peer;

static void on_pkt(void* user, const c2s2c_packet* p)
{
    peer* s = user;
    switch (p->packet_id) {
    case C2S2C_SYS_CONNECTED:
        s->connected++;
        break;
    case C2S2C_SYS_DISCONNECTED:
        s->disconnected++;
        break;
    case C2S2C_PKT_PING:
        s->pings++;
        if (s->reply) s->bad += c2s2c_send(s->ctx, p->conn, C2S2C_PKT_PONG, p->data, p->length) < 0;
        break;
    case C2S2C_PKT_PONG:
        s->pongs++;
        break;
    default:
        s->bad++;
    }
}

static void pump(peer* s)
{
    c2s2c_packet pk[256];
    int n = c2s2c_poll(s->ctx, pk, 256, 0);
    for (int i = 0; i < n; i++)
        on_pkt(s, &pk[i]);
    if (n > 0) c2s2c_release(s->ctx, n);
}

static c2s2c_ctx* make(unsigned conns)
{
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_conns = conns + 1;
    cfg.max_payload = 256;
    cfg.rx_ring_bytes = 256u << 10; /* small, so a short warm-up wraps it and touches all of it */
    cfg.rx_queue = 16384;
    cfg.tx_bytes = 4096;
    cfg.idle_timeout_ms = 3600000;
    cfg.backend = test_backend();
    return c2s2c_create(&cfg);
}

static long rss_kb(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return u.ru_maxrss / 1024; /* bytes on macOS */
#else
    return u.ru_maxrss; /* kilobytes on Linux */
#endif
}

static unsigned env_or(const char* name, unsigned dflt)
{
    const char* v = getenv(name);
    return v ? (unsigned)atoi(v) : dflt;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* a run killed by a timeout still shows how far it got */
    long fds = c2s2c_net_raise_fd_limit();
    unsigned conns = env_or("C2S2C_STRESS_CONNS", 10000);
    unsigned max_by_fds = fds > 200 ? (unsigned)((fds - 100) / 2) : 50;
    if (conns > max_by_fds) conns = max_by_fds;
    unsigned seconds = env_or("C2S2C_STRESS_SECONDS", 3);
    printf("connections: %u (fd limit %ld), duration: %u s\n", conns, fds, seconds);

    peer S = {0}, C = {0};
    S.ctx = make(conns);
    C.ctx = make(conns);
    S.reply = 1;
    CHECK(S.ctx && C.ctx);
    c2s2c_listen(S.ctx, "127.0.0.1", 0);
    uint32_t* handle = malloc(sizeof(uint32_t) * conns);
    /* Connect in bursts and serve the listener in between: the operating system keeps only a
     * short queue of not-yet-accepted connections (128 on macOS) and refuses the overflow. */
    unsigned opened = 0;
    uint64_t t_start = test_now_ms();
    for (unsigned i = 0; i < conns; i++) {
        if (c2s2c_connect(C.ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(S.ctx), &handle[i]) !=
            C2S2C_OK) {
            /* The system refused (ports or descriptors exhausted): that is its maximum. */
            printf("operating system limit reached after %u connections\n", i);
            break;
        }
        opened++;
        if (i % 64 == 63) { /* let the server accept the whole burst before the next one */
            uint64_t w0 = test_now_ms();
            /* A refused connection ends as a disconnection on the client side; counting it keeps
             * one lost connection from making every later burst wait for nothing. */
            while (S.connected + C.disconnected < opened && test_now_ms() - w0 < 5000) {
                pump(&S);
                pump(&C);
            }
        }
    }
    conns = opened;
    printf("connect: %u opened, %u accepted, %u refused, %llu ms\n", opened, S.connected,
           C.disconnected, (unsigned long long)(test_now_ms() - t_start));
    CHECK(conns >= 1000); /* anything below would not be a stress test */
    uint64_t t0 = test_now_ms();
    while ((S.connected < conns || C.connected < conns) && test_now_ms() - t0 < 60000) {
        pump(&S);
        pump(&C);
    }
    CHECK(S.connected == conns && C.connected == conns);

    /* Warm-up: one ping on every connection, so each connection's buffers have been touched
     * (the operating system hands out memory lazily). The memory baseline is taken afterwards. */
    uint8_t b[4] = {1, 2, 3, 4};
    uint64_t sent = 0;
    for (unsigned i = 0; i < conns; i++) {
        while (c2s2c_send(C.ctx, handle[i], C2S2C_PKT_PING, b, 4) == C2S2C_ERR_WOULD_BLOCK) {
            pump(&S);
            pump(&C);
        }
        sent++;
    }
    uint64_t w1 = test_now_ms();
    while (C.pongs < sent && test_now_ms() - w1 < 60000) {
        pump(&S);
        pump(&C);
    }
    /* Then enough traffic to wrap the receive ring around several times: the ring, the queue and
     * the buffers are reserved up front but the operating system only counts a page once it was
     * touched, so the baseline must be taken after everything was touched at least once. */
    uint64_t next = 0, warm_target = sent + 200000;
    w1 = test_now_ms();
    while (C.pongs < warm_target && test_now_ms() - w1 < 120000) {
        for (int k = 0; k < 256 && sent < warm_target; k++) {
            if (c2s2c_send(C.ctx, handle[next], C2S2C_PKT_PING, b, 4) == C2S2C_OK) sent++;
            next = (next + 1) % conns;
        }
        pump(&S);
        pump(&C);
    }
    long rss_early = rss_kb();
    printf("warm-up: %llu pongs of %llu, %llu ms\n", (unsigned long long)C.pongs,
           (unsigned long long)sent, (unsigned long long)(test_now_ms() - t_start));

    /* Traffic: keep sending pings round-robin until the time is up. */
    uint64_t start = test_now_ms();
    while (test_now_ms() - start < seconds * 1000u) {
        for (int k = 0; k < 256; k++) {
            if (c2s2c_send(C.ctx, handle[next], C2S2C_PKT_PING, b, 4) == C2S2C_OK) sent++;
            next = (next + 1) % conns;
        }
        pump(&S);
        pump(&C);
    }
    uint64_t t1 = test_now_ms();
    while (C.pongs < sent && test_now_ms() - t1 < 30000) {
        pump(&S);
        pump(&C);
    }
    double secs = (double)(test_now_ms() - start) / 1000.0;
    printf("pings sent %llu, pongs %llu, %.0f packets/s (round trips)\n", (unsigned long long)sent,
           (unsigned long long)C.pongs, (double)C.pongs / secs);
    CHECK(C.pongs == sent && S.pings == sent);
    CHECK(S.bad == 0 && C.bad == 0 && S.disconnected == 0 && C.disconnected == 0);

    /* Memory must not keep growing once every connection is warm (10 % plus 5 MB of slack). */
    long rss_end = rss_kb();
    printf("max RSS: %ld kB after warm-up, %ld kB at the end\n", rss_early, rss_end);
    CHECK(rss_end <= rss_early + rss_early / 10 + 5120);

    /* The side that closes first keeps the sockets in TIME_WAIT for a while. Closing the server
     * side first leaves the clients' ephemeral ports free, so tests run right after this one
     * can still connect. */
    c2s2c_destroy(S.ctx);
    c2s2c_destroy(C.ctx);
    free(handle);
    return c2s2c_test_done();
}
