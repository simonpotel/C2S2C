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
    dx_config cfg;
    dx_config_default(&cfg);
    dx_ctx* ok = dx_create(&cfg); /* the defaults are valid */
    CHECK(ok != NULL);
    dx_destroy(ok);

    dx_config c;
    dx_config_default(&c);
    c.max_conns = 65536;
    CHECK(dx_create(&c) == NULL);
    dx_config_default(&c);
    c.max_payload = DX_MAX_PAYLOAD + 1;
    CHECK(dx_create(&c) == NULL);
    dx_config_default(&c);
    c.rx_ring_bytes = 1000;
    CHECK(dx_create(&c) == NULL);
    dx_config_default(&c);
    c.tx_bytes = 100;
    CHECK(dx_create(&c) == NULL);
    dx_config_default(&c);
    c.backend = 99;
    CHECK(dx_create(&c) == NULL);
    dx_config_default(&c);
    c.size = 8;
    CHECK(dx_create(&c) == NULL);

    /* Refuse each allocation in turn: dx_create fails and frees what it had taken. */
    int failures = 0;
    for (int n = 1; n <= 12; n++) {
        flaky f = {n, 0, 0};
        dx_config_default(&c);
        c.max_conns = 4;
        c.alloc = (dx_alloc){flaky_alloc, flaky_free, &f};
        dx_ctx* ctx = dx_create(&c);
        if (!ctx) {
            failures++;
            CHECK(f.live == 0); /* nothing leaked on the failure path */
        } else {
            dx_destroy(ctx);
            CHECK(f.live == 0);
        }
    }
    CHECK(failures >= 6); /* the context needs about eight blocks: most refusals must fail it */
    dx_destroy(NULL);     /* harmless */
}

static void test_listen_and_connect_errors(void)
{
    dx_ctx* ctx = dx_create(NULL);
    uint32_t conn;
    CHECK(dx_listen_port(ctx) == DX_ERR_INVALID_ARG); /* not listening yet */
    CHECK(dx_listen(ctx, "not an address", 0) == DX_ERR_INVALID_ARG);
    CHECK(dx_listen(NULL, "127.0.0.1", 0) == DX_ERR_INVALID_ARG);
    CHECK(dx_connect(ctx, "not an address", 1, &conn) == DX_ERR_INVALID_ARG);
    CHECK(dx_connect(ctx, "127.0.0.1", 1, NULL) == DX_ERR_INVALID_ARG);
    CHECK(dx_listen(ctx, "127.0.0.1", 0) == DX_OK);
    CHECK(dx_listen(ctx, "127.0.0.1", 0) == DX_ERR_LIMIT); /* one listener per context */
    CHECK(dx_listen_port(ctx) > 0);

    /* IPv6 loopback, where the machine has it. */
    dx_ctx* v6 = dx_create(NULL);
    if (dx_listen(v6, "::1", 0) == DX_OK) {
        uint32_t c6;
        CHECK(dx_connect(ctx, "::1", (uint16_t)dx_listen_port(v6), &c6) == DX_OK);
    }
    dx_destroy(v6);

    /* No free connection slot left. */
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_conns = 1;
    dx_ctx* one = dx_create(&cfg);
    CHECK(dx_connect(one, "127.0.0.1", (uint16_t)dx_listen_port(ctx), &conn) == DX_OK);
    CHECK(dx_connect(one, "127.0.0.1", (uint16_t)dx_listen_port(ctx), &conn) == DX_ERR_LIMIT);
    CHECK(dx_close(one, conn) == DX_OK); /* closing a connection that is still connecting */
    dx_destroy(one);
    dx_destroy(ctx);
}

static void test_send_errors(void)
{
    side S = {0}, C = {0};
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_payload = 64;
    cfg.max_conns = 4;
    S.ctx = dx_create(&cfg);
    C.ctx = dx_create(&cfg);
    dx_listen(S.ctx, "127.0.0.1", 0);
    uint32_t cconn;
    dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(S.ctx), &cconn);
    WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);
    uint8_t b[4] = {0};

    CHECK(dx_send(C.ctx, cconn, DX_PKT_UPLOAD, NULL, 3) == DX_ERR_INVALID_ARG);
    CHECK(dx_send(C.ctx, cconn, DX_PKT_PING, b, 100) == DX_ERR_TOO_LARGE);
    CHECK(dx_send(C.ctx, cconn, DX_PKT_PONG, b, 4) ==
          DX_ERR_BAD_ID); /* a client may not send STOC */
    CHECK(dx_send(S.ctx, S.conn, DX_PKT_PING, b, 4) == DX_ERR_BAD_ID); /* nor a server CTOS */
    CHECK(dx_send(C.ctx, cconn, 0x0100, b, 0) == DX_ERR_UNKNOWN_ID);
    CHECK(dx_send(C.ctx, cconn, DX_PKT_PING, b, 3) == DX_ERR_TRUNCATED); /* ping is a u32 */
    CHECK(dx_send(NULL, cconn, DX_PKT_PING, b, 4) == DX_ERR_BAD_CONN);
    CHECK(dx_send(C.ctx, 0, DX_PKT_PING, b, 4) == DX_ERR_BAD_CONN);
    CHECK(dx_send_batch(C.ctx, NULL, 1) == DX_ERR_INVALID_ARG);
    CHECK(dx_send_batch(C.ctx, (dx_outgoing[1]){{cconn, DX_PKT_PING, 0, b, 4}}, -1) ==
          DX_ERR_INVALID_ARG);
    dx_outgoing batch[3] = {{cconn, DX_PKT_PING, 0, b, 4},
                            {cconn, DX_PKT_PING, 0, b, 4},
                            {cconn, DX_PKT_PONG, 0, b, 4}};
    CHECK(dx_send_batch(C.ctx, batch, 3) == 2); /* stops at the first failure */
    CHECK(dx_poll(C.ctx, NULL, 1, 0) == DX_ERR_INVALID_ARG);

    CHECK(dx_close(C.ctx, cconn) == DX_OK);
    CHECK(dx_close(C.ctx, cconn) == DX_ERR_BAD_CONN); /* already gone: the old handle is stale */
    CHECK(dx_send(C.ctx, cconn, DX_PKT_PING, b, 4) == DX_ERR_BAD_CONN); /* closing or closed */
    WAIT_UNTIL(S, C, S.disconnected == 1);
    CHECK(dx_close(C.ctx, 0xDEADBEEFu) == DX_ERR_BAD_CONN);
    dx_release(C.ctx, -5); /* nonsense counts are ignored */
    dx_release(NULL, 1);
    dx_destroy(S.ctx);
    dx_destroy(C.ctx);
}

/* Waits for the CONNECTED event of the connection the context accepted (not its own outgoing). */
static uint32_t accept_one(dx_ctx* ctx, const uint32_t* outgoing, int n_out)
{
    uint64_t t0 = test_now_ms();
    while (test_now_ms() - t0 < 5000) {
        dx_packet pk[16];
        int n = dx_poll(ctx, pk, 16, 1);
        for (int i = 0; i < n; i++) {
            int mine = 0;
            for (int k = 0; k < n_out; k++)
                mine |= pk[i].conn == outgoing[k];
            if (pk[i].packet_id == DX_SYS_CONNECTED && !mine) {
                uint32_t h = pk[i].conn;
                dx_release(ctx, n);
                return h;
            }
        }
        dx_release(ctx, n);
    }
    return 0;
}

static void test_tunnel_errors(void)
{
    CHECK(dx_flush(NULL) == DX_ERR_INVALID_ARG);
    dx_ctx* ctx = dx_create(NULL);
    dx_listen(ctx, "127.0.0.1", 0);
    uint32_t out[2];
    dx_connect(ctx, "127.0.0.1", (uint16_t)dx_listen_port(ctx), &out[0]);
    dx_connect(ctx, "127.0.0.1", (uint16_t)dx_listen_port(ctx), &out[1]);
    uint32_t acc = accept_one(ctx, out, 2);
    uint16_t ids[DX_TUNNEL_MAX_DROP + 1] = {0};
    CHECK(acc != 0);
    CHECK(dx_tunnel(NULL, out[0], acc, NULL, 0) == DX_ERR_INVALID_ARG);
    CHECK(dx_tunnel(ctx, out[0], out[0], NULL, 0) == DX_ERR_INVALID_ARG);
    CHECK(dx_tunnel(ctx, out[0], out[1], NULL, 0) == DX_ERR_INVALID_ARG); /* both outgoing */
    CHECK(dx_tunnel(ctx, out[0], acc, ids, DX_TUNNEL_MAX_DROP + 1) == DX_ERR_INVALID_ARG);
    CHECK(dx_tunnel(ctx, out[0], acc, NULL, 2) == DX_ERR_INVALID_ARG);
    CHECK(dx_tunnel(ctx, out[0], 0xBEEF0001u, NULL, 0) == DX_ERR_BAD_CONN);
    CHECK(dx_tunnel(ctx, out[0], acc, ids, 1) == DX_OK);
    CHECK(dx_tunnel(ctx, out[1], acc, NULL, 0) == DX_ERR_INVALID_ARG); /* acc is already relayed */
    dx_destroy(ctx);

    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_tunnels = 1;
    ctx = dx_create(&cfg);
    dx_listen(ctx, "127.0.0.1", 0);
    dx_connect(ctx, "127.0.0.1", (uint16_t)dx_listen_port(ctx), &out[0]);
    uint32_t a1 = accept_one(ctx, out, 1);
    dx_connect(ctx, "127.0.0.1", (uint16_t)dx_listen_port(ctx), &out[1]);
    uint32_t a2 = accept_one(ctx, out, 2);
    CHECK(dx_tunnel(ctx, out[0], a1, NULL, 0) == DX_OK);
    CHECK(dx_tunnel(ctx, out[1], a2, NULL, 0) == DX_ERR_LIMIT); /* table of one is full */
    dx_destroy(ctx);
}

/* Regression: dx_poll must honour short timeouts even after a long pause between calls (it once
 * measured the remaining time from a stale clock and waited up to 100 ms instead of 1 ms). */
static void test_poll_timeout(void)
{
    dx_ctx* ctx = dx_create(NULL);
    dx_packet pk[4];
    dx_poll(ctx, pk, 4, 0);
    uint64_t pause = test_now_ms();
    while (test_now_ms() - pause < 300) {
    } /* a long gap without polling */
    uint64_t worst = 0;
    for (int i = 0; i < 20; i++) {
        uint64_t t0 = test_now_ms();
        CHECK(dx_poll(ctx, pk, 4, 1) == 0);
        if (test_now_ms() - t0 > worst) worst = test_now_ms() - t0;
    }
    CHECK(worst < 50); /* 1 ms asked; generous margin for a loaded machine, far below 100 ms */
    dx_destroy(ctx);
}

int main(void)
{
    test_poll_timeout();
    test_create_failures();
    test_listen_and_connect_errors();
    test_send_errors();
    test_tunnel_errors();
    return dx_test_done();
}
