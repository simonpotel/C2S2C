/* test_integration.c - a real server and client over loopback TCP, on every I/O backend.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "../net_test.h"
#include "IO/io.h"

/*
 * Scenario per backend: handshake, ping/pong, 1 MiB payloads both ways, 10 000 ordered
 * packets, and a clean close started by either side. Server and client are two contexts in the
 * same thread, polled in turn.
 */
static dx_ctx* make_ctx(uint32_t backend)
{
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.backend = backend;
    cfg.max_conns = 8;
    cfg.max_payload = 2u << 20;
    cfg.rx_ring_bytes = 8u << 20;
    cfg.tx_bytes = 0;
    return dx_create(&cfg);
}

/* Sends, pumping both sides while the send buffer is full. */
static int send_retry(side* from, side* a, side* b, uint32_t conn, uint16_t id, const uint8_t* d,
                      uint32_t n)
{
    uint64_t t0 = test_now_ms();
    int rc;
    while ((rc = dx_send(from->ctx, conn, id, d, n)) == DX_ERR_WOULD_BLOCK &&
           test_now_ms() - t0 < 10000) {
        pump_side(a, 0);
        pump_side(b, 0);
    }
    return rc;
}

static void run_backend(uint32_t backend)
{
    side S = {0}, C = {0};
    S.ctx = make_ctx(backend);
    C.ctx = make_ctx(backend);
    CHECK(S.ctx && C.ctx);
    if (!S.ctx || !C.ctx) return;
    S.reply = 1;

    /* Handshake: both sides see the connection. */
    CHECK(dx_listen(S.ctx, "127.0.0.1", 0) == DX_OK && dx_listen_port(S.ctx) > 0);
    uint32_t cconn;
    CHECK(dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(S.ctx), &cconn) == DX_OK);
    WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);
    CHECK(S.connected == 1 && C.connected == 1);
    uint32_t sconn = S.conn;

    /* Ping / pong. */
    uint8_t b[4] = {0, 0, 0, 0}; /* nonce 0 */
    CHECK(dx_send(C.ctx, cconn, DX_PKT_PING, b, 4) == DX_OK);
    WAIT_UNTIL(S, C, C.pongs == 1);
    CHECK(C.pongs == 1);
    S.pings = 0; /* the ordered-nonce check restarts below */
    C.pongs = 0;

    /* 1 MiB each way, content verified byte by byte. */
    uint32_t big = 1u << 20;
    uint8_t* buf = malloc(big + 4);
    dx_writer w;
    dx_writer_init(&w, buf, big + 4);
    uint8_t* blob = malloc(big);
    for (uint32_t i = 0; i < big; i++)
        blob[i] = (uint8_t)(i * 7u);
    dx_w_bytes(&w, blob, big);
    CHECK(w.err == 0);
    CHECK(send_retry(&C, &S, &C, cconn, DX_PKT_UPLOAD, buf, w.len) == DX_OK);
    CHECK(send_retry(&S, &S, &C, sconn, DX_PKT_DOWNLOAD, buf, w.len) == DX_OK);
    WAIT_UNTIL(S, C, S.uploads == 1 && C.downloads == 1);
    CHECK(S.uploads == 1 && C.downloads == 1);
    CHECK(S.bad == 0 && C.bad == 0);
    free(buf);
    free(blob);

    /* 10 000 packets arrive complete and in order (nonce and sequence checked by on_packet). */
    S.reply = 0;
    enum { N = 10000 };
    for (uint32_t i = 0; i < N; i++) {
        uint8_t p[4];
        dx_writer_init(&w, p, sizeof p);
        dx_w_u32(&w, i);
        if (send_retry(&C, &S, &C, cconn, DX_PKT_PING, p, 4) != DX_OK) {
            CHECK(0);
            break;
        }
    }
    WAIT_UNTIL(S, C, S.pings == N);
    CHECK(S.pings == N);
    CHECK(S.bad == 0);

    /* Client closes: the server learns it, with a clean reason; the old handle is stale. */
    CHECK(dx_close(C.ctx, cconn) == DX_OK);
    WAIT_UNTIL(S, C, S.disconnected == 1 && C.disconnected == 1);
    CHECK(S.disconnected == 1 && C.disconnected == 1);
    CHECK(S.reason == 0 && C.reason == 0);
    CHECK(dx_send(C.ctx, cconn, DX_PKT_PING, b, 4) == DX_ERR_BAD_CONN);

    /* Server closes the next connection: the client learns it. */
    S.disconnected = C.disconnected = 0;
    S.connected = C.connected = 0;
    CHECK(dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(S.ctx), &cconn) == DX_OK);
    WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);
    CHECK(S.connected == 1 && C.connected == 1);
    CHECK(dx_close(S.ctx, S.conn) == DX_OK);
    WAIT_UNTIL(S, C, S.disconnected == 1 && C.disconnected == 1);
    CHECK(S.disconnected == 1 && C.disconnected == 1);
    CHECK(C.reason == 0);

    CHECK(S.other == 0 && C.other == 0);
    dx_destroy(S.ctx);
    dx_destroy(C.ctx);
}

/* Connecting to a port nobody listens on ends in a disconnect event, never a hang. */
static void test_connect_refused(void)
{
    side S = {0}, C = {0};
    S.ctx = make_ctx(DX_BACKEND_AUTO);
    C.ctx = make_ctx(DX_BACKEND_AUTO);
    dx_listen(S.ctx, "127.0.0.1", 0);
    uint16_t port = (uint16_t)dx_listen_port(S.ctx);
    dx_destroy(S.ctx); /* the port is now closed */
    S.ctx = make_ctx(DX_BACKEND_AUTO);
    uint32_t conn;
    CHECK(dx_connect(C.ctx, "127.0.0.1", port, &conn) == DX_OK);
    WAIT_UNTIL(S, C, C.disconnected == 1);
    CHECK(C.disconnected == 1);
    CHECK(C.reason == DX_ERR_IO && C.connected == 0);
    dx_destroy(S.ctx);
    dx_destroy(C.ctx);
}

int main(void)
{
    run_backend(DX_BACKEND_POLL);
    if (dx_io_get(DX_BACKEND_AUTO) != &dx_io_poll) run_backend(DX_BACKEND_AUTO); /* native one */
    test_connect_refused();
    return dx_test_done();
}
