/* test_api.c - wrong arguments, limits and failures of the runtime API (nothing may crash or leak).
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "../net_test.h"

/*
 * Every misuse must come back as a precise error code. Memory failures are simulated with an
 * allocator that refuses the Nth request: creating a context must then fail cleanly, with
 * everything already allocated given back.
 */
typedef struct {
    int fail_at; /* refuse allocation number fail_at (1-based); 0 = never */
    int count, live;
} flaky;

static void* flaky_alloc(size_t n, void* user)
{
    flaky* f = user;
    if (++f->count == f->fail_at) return NULL;
    f->live++;
    return malloc(n);
}

static void flaky_free(void* p, void* user)
{
    if (p) ((flaky*)user)->live--;
    free(p);
}

static void test_create_failures(void)
{
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    c2s2c_ctx* ok = c2s2c_create(&cfg); /* the defaults are valid */
    CHECK(ok != NULL);
    c2s2c_destroy(ok);

    c2s2c_config c;
    c2s2c_config_default(&c);
    c.max_conns = 65536;
    CHECK(c2s2c_create(&c) == NULL);
    c2s2c_config_default(&c);
    c.max_payload = C2S2C_MAX_PAYLOAD + 1;
    CHECK(c2s2c_create(&c) == NULL);
    c2s2c_config_default(&c);
    c.rx_ring_bytes = 1000;
    CHECK(c2s2c_create(&c) == NULL);
    c2s2c_config_default(&c);
    c.tx_bytes = 100;
    CHECK(c2s2c_create(&c) == NULL);
    c2s2c_config_default(&c);
    c.backend = 99;
    CHECK(c2s2c_create(&c) == NULL);
    c2s2c_config_default(&c);
    c.size = 8;
    CHECK(c2s2c_create(&c) == NULL);

    /* Refuse each allocation in turn: c2s2c_create fails and frees what it had taken. */
    int failures = 0;
    for (int n = 1; n <= 12; n++) {
        flaky f = {n, 0, 0};
        c2s2c_config_default(&c);
        c.max_conns = 4;
        c.alloc = (c2s2c_alloc){flaky_alloc, flaky_free, &f};
        c2s2c_ctx* ctx = c2s2c_create(&c);
        if (!ctx) {
            failures++;
            CHECK(f.live == 0); /* nothing leaked on the failure path */
        } else {
            c2s2c_destroy(ctx);
            CHECK(f.live == 0);
        }
    }
    CHECK(failures >= 6); /* the context needs about eight blocks: most refusals must fail it */
    c2s2c_destroy(NULL);  /* harmless */
}

static void test_listen_and_connect_errors(void)
{
    c2s2c_ctx* ctx = c2s2c_create(NULL);
    uint32_t conn;
    CHECK(c2s2c_listen_port(ctx) == C2S2C_ERR_INVALID_ARG); /* not listening yet */
    CHECK(c2s2c_listen(ctx, "not an address", 0) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_listen(NULL, "127.0.0.1", 0) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_connect(ctx, "not an address", 1, &conn) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_connect(ctx, "127.0.0.1", 1, NULL) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_listen(ctx, "127.0.0.1", 0) == C2S2C_OK);
    CHECK(c2s2c_listen(ctx, "127.0.0.1", 0) == C2S2C_ERR_LIMIT); /* one listener per context */
    CHECK(c2s2c_listen_port(ctx) > 0);

    /* IPv6 loopback, where the machine has it. */
    c2s2c_ctx* v6 = c2s2c_create(NULL);
    if (c2s2c_listen(v6, "::1", 0) == C2S2C_OK) {
        uint32_t c6;
        CHECK(c2s2c_connect(ctx, "::1", (uint16_t)c2s2c_listen_port(v6), &c6) == C2S2C_OK);
    }
    c2s2c_destroy(v6);

    /* No free connection slot left. */
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_conns = 1;
    c2s2c_ctx* one = c2s2c_create(&cfg);
    CHECK(c2s2c_connect(one, "127.0.0.1", (uint16_t)c2s2c_listen_port(ctx), &conn) == C2S2C_OK);
    CHECK(c2s2c_connect(one, "127.0.0.1", (uint16_t)c2s2c_listen_port(ctx), &conn) ==
          C2S2C_ERR_LIMIT);
    CHECK(c2s2c_close(one, conn) == C2S2C_OK); /* closing a connection that is still connecting */
    c2s2c_destroy(one);
    c2s2c_destroy(ctx);
}

static void test_send_errors(void)
{
    side S = {0}, C = {0};
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_payload = 64;
    cfg.max_conns = 4;
    S.ctx = c2s2c_create(&cfg);
    C.ctx = c2s2c_create(&cfg);
    c2s2c_listen(S.ctx, "127.0.0.1", 0);
    uint32_t cconn;
    c2s2c_connect(C.ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(S.ctx), &cconn);
    WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);
    uint8_t b[4] = {0};

    CHECK(c2s2c_send(C.ctx, cconn, C2S2C_PKT_UPLOAD, NULL, 3) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_send(C.ctx, cconn, C2S2C_PKT_PING, b, 100) == C2S2C_ERR_TOO_LARGE);
    CHECK(c2s2c_send(C.ctx, cconn, C2S2C_PKT_PONG, b, 4) ==
          C2S2C_ERR_BAD_ID); /* a client may not send STOC */
    CHECK(c2s2c_send(S.ctx, S.conn, C2S2C_PKT_PING, b, 4) ==
          C2S2C_ERR_BAD_ID); /* nor a server CTOS */
    CHECK(c2s2c_send(C.ctx, cconn, 0x0100, b, 0) == C2S2C_ERR_UNKNOWN_ID);
    CHECK(c2s2c_send(C.ctx, cconn, C2S2C_PKT_PING, b, 3) ==
          C2S2C_ERR_TRUNCATED); /* ping is a u32 */
    CHECK(c2s2c_send(NULL, cconn, C2S2C_PKT_PING, b, 4) == C2S2C_ERR_BAD_CONN);
    CHECK(c2s2c_send(C.ctx, 0, C2S2C_PKT_PING, b, 4) == C2S2C_ERR_BAD_CONN);
    CHECK(c2s2c_send_batch(C.ctx, NULL, 1) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_send_batch(C.ctx, (c2s2c_outgoing[1]){{cconn, C2S2C_PKT_PING, 0, b, 4}}, -1) ==
          C2S2C_ERR_INVALID_ARG);
    c2s2c_outgoing batch[3] = {{cconn, C2S2C_PKT_PING, 0, b, 4},
                               {cconn, C2S2C_PKT_PING, 0, b, 4},
                               {cconn, C2S2C_PKT_PONG, 0, b, 4}};
    CHECK(c2s2c_send_batch(C.ctx, batch, 3) == 2); /* stops at the first failure */
    CHECK(c2s2c_poll(C.ctx, NULL, 1, 0) == C2S2C_ERR_INVALID_ARG);

    CHECK(c2s2c_close(C.ctx, cconn) == C2S2C_OK);
    CHECK(c2s2c_close(C.ctx, cconn) ==
          C2S2C_ERR_BAD_CONN); /* already gone: the old handle is stale */
    CHECK(c2s2c_send(C.ctx, cconn, C2S2C_PKT_PING, b, 4) ==
          C2S2C_ERR_BAD_CONN); /* closing or closed */
    WAIT_UNTIL(S, C, S.disconnected == 1);
    CHECK(c2s2c_close(C.ctx, 0xDEADBEEFu) == C2S2C_ERR_BAD_CONN);
    c2s2c_release(C.ctx, -5); /* nonsense counts are ignored */
    c2s2c_release(NULL, 1);
    c2s2c_destroy(S.ctx);
    c2s2c_destroy(C.ctx);
}

/* Waits for the CONNECTED event of the connection the context accepted (not its own outgoing). */
static uint32_t accept_one(c2s2c_ctx* ctx, const uint32_t* outgoing, int n_out)
{
    uint64_t t0 = test_now_ms();
    while (test_now_ms() - t0 < 5000) {
        c2s2c_packet pk[16];
        int n = c2s2c_poll(ctx, pk, 16, 1);
        for (int i = 0; i < n; i++) {
            int mine = 0;
            for (int k = 0; k < n_out; k++)
                mine |= pk[i].conn == outgoing[k];
            if (pk[i].packet_id == C2S2C_SYS_CONNECTED && !mine) {
                uint32_t h = pk[i].conn;
                c2s2c_release(ctx, n);
                return h;
            }
        }
        c2s2c_release(ctx, n);
    }
    return 0;
}

static void test_tunnel_errors(void)
{
    CHECK(c2s2c_flush(NULL) == C2S2C_ERR_INVALID_ARG);
    c2s2c_ctx* ctx = c2s2c_create(NULL);
    c2s2c_listen(ctx, "127.0.0.1", 0);
    uint32_t out[2];
    c2s2c_connect(ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(ctx), &out[0]);
    c2s2c_connect(ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(ctx), &out[1]);
    uint32_t acc = accept_one(ctx, out, 2);
    uint16_t ids[C2S2C_TUNNEL_MAX_DROP + 1] = {0};
    CHECK(acc != 0);
    CHECK(c2s2c_tunnel(NULL, out[0], acc, NULL, 0) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_tunnel(ctx, out[0], out[0], NULL, 0) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_tunnel(ctx, out[0], out[1], NULL, 0) == C2S2C_ERR_INVALID_ARG); /* both outgoing */
    CHECK(c2s2c_tunnel(ctx, out[0], acc, ids, C2S2C_TUNNEL_MAX_DROP + 1) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_tunnel(ctx, out[0], acc, NULL, 2) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_tunnel(ctx, out[0], 0xBEEF0001u, NULL, 0) == C2S2C_ERR_BAD_CONN);
    CHECK(c2s2c_tunnel(ctx, out[0], acc, ids, 1) == C2S2C_OK);
    CHECK(c2s2c_tunnel(ctx, out[1], acc, NULL, 0) ==
          C2S2C_ERR_INVALID_ARG); /* acc is already relayed */
    c2s2c_destroy(ctx);

    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_tunnels = 1;
    ctx = c2s2c_create(&cfg);
    c2s2c_listen(ctx, "127.0.0.1", 0);
    c2s2c_connect(ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(ctx), &out[0]);
    uint32_t a1 = accept_one(ctx, out, 1);
    c2s2c_connect(ctx, "127.0.0.1", (uint16_t)c2s2c_listen_port(ctx), &out[1]);
    uint32_t a2 = accept_one(ctx, out, 2);
    CHECK(c2s2c_tunnel(ctx, out[0], a1, NULL, 0) == C2S2C_OK);
    CHECK(c2s2c_tunnel(ctx, out[1], a2, NULL, 0) == C2S2C_ERR_LIMIT); /* table of one is full */
    c2s2c_destroy(ctx);
}

/* Regression: c2s2c_poll must honour short timeouts even after a long pause between calls (it once
 * measured the remaining time from a stale clock and waited up to 100 ms instead of 1 ms). */
static void test_poll_timeout(void)
{
    c2s2c_ctx* ctx = c2s2c_create(NULL);
    c2s2c_packet pk[4];
    c2s2c_poll(ctx, pk, 4, 0);
    uint64_t pause = test_now_ms();
    while (test_now_ms() - pause < 300) {
    } /* a long gap without polling */
    uint64_t worst = 0;
    for (int i = 0; i < 20; i++) {
        uint64_t t0 = test_now_ms();
        CHECK(c2s2c_poll(ctx, pk, 4, 1) == 0);
        if (test_now_ms() - t0 > worst) worst = test_now_ms() - t0;
    }
    CHECK(worst < 50); /* 1 ms asked; generous margin for a loaded machine, far below 100 ms */
    c2s2c_destroy(ctx);
}

int main(void)
{
    test_poll_timeout();
    test_create_failures();
    test_listen_and_connect_errors();
    test_send_errors();
    test_tunnel_errors();
    return c2s2c_test_done();
}
