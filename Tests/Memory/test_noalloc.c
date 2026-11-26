/* test_noalloc.c - after warm-up the library must not allocate; nothing may leak.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "../net_test.h"

/*
 * How: the library takes an allocator in its config. Here it is a wrapper that counts calls.
 * We warm up (connect, exchange packets), note the counters, then run heavy traffic: batched
 * sends, 5000 round trips, connections opened and closed repeatedly. The counters must not move.
 * After c2s2c_destroy every block must have been returned.
 * (Source/ has no other allocation path: Tools/Scripts/check_source_rules.sh enforces it.)
 */
typedef struct {
    size_t allocs, frees;
} counter;

static void* count_alloc(size_t n, void* user)
{
    ((counter*)user)->allocs++;
    return malloc(n);
}

static void count_free(void* p, void* user)
{
    if (p) ((counter*)user)->frees++;
    free(p);
}

static c2s2c_ctx* make(counter* c)
{
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_conns = 8;
    cfg.max_payload = 1024;
    cfg.rx_ring_bytes = 1u << 16;
    cfg.tx_bytes = 0;
    cfg.alloc = (c2s2c_alloc){count_alloc, count_free, c};
    return c2s2c_create(&cfg);
}

/* Sends `n` pings in batches of 8 and waits for every pong. */
static void round_trips(side* S, side* C, uint32_t conn, unsigned n)
{
    unsigned want = C->pongs + n;
    uint8_t bufs[8][4];
    for (unsigned sent = 0; sent < n;) {
        c2s2c_outgoing out[8];
        unsigned k = n - sent < 8 ? n - sent : 8;
        for (unsigned i = 0; i < k; i++) {
            c2s2c_writer w;
            c2s2c_writer_init(&w, bufs[i], 4);
            c2s2c_w_u32(&w, sent + i);
            out[i] = (c2s2c_outgoing){conn, C2S2C_PKT_PING, 0, bufs[i], 4};
        }
        int rc = c2s2c_send_batch(C->ctx, out, (int)k);
        sent += rc > 0 ? (unsigned)rc : 0;
        pump_side(S, 0);
        pump_side(C, 0);
    }
    WAIT_UNTIL(*S, *C, C->pongs == want);
}

int main(void)
{
    counter cs = {0, 0}, cc = {0, 0};
    side S = {0}, C = {0};
    S.ctx = make(&cs);
    C.ctx = make(&cc);
    CHECK(S.ctx && C.ctx);
    S.reply = 1;
    c2s2c_listen(S.ctx, "127.0.0.1", 0);
    uint32_t conn;
    c2s2c_connect(C.ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(S.ctx), &conn);
    WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);
    S.pings = 0; /* ping ordering is not what this test is about */
    S.bad = 0;

    round_trips(&S, &C, conn, 100); /* warm-up */
    size_t before_s = cs.allocs, before_c = cc.allocs;

    S.bad = 0;
    round_trips(&S, &C, conn, 5000);
    for (int i = 0; i < 20; i++) { /* connection churn: open and close */
        uint32_t extra;
        S.connected = C.connected = 0;
        CHECK(c2s2c_connect(C.ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(S.ctx), &extra) ==
              C2S2C_OK);
        WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);
        S.disconnected = C.disconnected = 0;
        c2s2c_close(C.ctx, extra);
        WAIT_UNTIL(S, C, S.disconnected == 1 && C.disconnected == 1);
    }
    CHECK(cs.allocs == before_s && cc.allocs == before_c); /* nothing allocated after warm-up */
    CHECK(C.pongs >= 5100);

    c2s2c_destroy(S.ctx);
    c2s2c_destroy(C.ctx);
    CHECK(cs.allocs == cs.frees && cc.allocs == cc.frees); /* nothing leaked */
    CHECK(cs.allocs > 0);                                  /* the counting hook really was used */
    return c2s2c_test_done();
}
