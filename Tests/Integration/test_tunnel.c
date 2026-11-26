/* test_tunnel.c - the native relay: a filtering proxy between a client and a server.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "../net_test.h"

/*
 * Real use case: a proxy (P) in front of a server (U). For every client connection P accepts, it
 * opens one to U and ties both with dx_tunnel, dropping chat messages. Packets then flow
 * client <-> U without P's application ever seeing them. Checked: relaying and dropping, no data
 * loss when U stops reading for a while (back-pressure through the proxy), teardown of both sides.
 */
typedef struct {
    dx_ctx* ctx;
    uint32_t outgoing[8], accepted[8];
    int n_out, n_acc, disconnects;
    unsigned pings, chats, uploads, data_seen, bad;
} node;

static int is_outgoing(const node* n, uint32_t h)
{
    for (int i = 0; i < n->n_out; i++)
        if (n->outgoing[i] == h) return 1;
    return 0;
}

/* One round: the server answers pings; the proxy tunnels each new client; everyone counts. */
static void step(node* n, int is_server, int is_proxy, uint16_t upstream_port, int timeout)
{
    dx_packet pk[64];
    int got = dx_poll(n->ctx, pk, 64, timeout);
    for (int i = 0; i < got; i++) {
        const dx_packet* p = &pk[i];
        if (p->packet_id == DX_SYS_CONNECTED) {
            if (is_outgoing(n, p->conn)) continue;
            n->accepted[n->n_acc++] = p->conn;
            if (is_proxy) { /* a client arrived: open the upstream side and relay */
                uint32_t up;
                uint16_t drop[] = {DX_PKT_CHAT_SEND};
                if (dx_connect(n->ctx, "127.0.0.1", upstream_port, &up) != DX_OK) n->bad++;
                n->outgoing[n->n_out++] = up;
                if (dx_tunnel(n->ctx, p->conn, up, drop, 1) != DX_OK) n->bad++;
            }
        } else if (p->packet_id == DX_SYS_DISCONNECTED) {
            n->disconnects++;
        } else {
            n->data_seen++;
            if (p->packet_id == DX_PKT_PING) {
                n->pings++;
                if (is_server) dx_send(n->ctx, p->conn, DX_PKT_PONG, p->data, p->length);
            }
            if (p->packet_id == DX_PKT_CHAT_SEND) n->chats++;
            if (p->packet_id == DX_PKT_UPLOAD) { /* the blob starts with its index: order check */
                uint32_t index = (uint32_t)p->data[4] | (uint32_t)p->data[5] << 8;
                if (index != n->uploads) n->bad++;
                n->uploads++;
            }
        }
    }
    dx_release(n->ctx, got);
}

static dx_ctx* make(void)
{
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_conns = 8;
    cfg.max_payload = 4096;
    cfg.rx_ring_bytes = 1u << 20;
    cfg.tx_bytes = 0;
    cfg.backend = test_backend();
    return dx_create(&cfg);
}

#define RUN(cond)                                                                                  \
    do {                                                                                           \
        uint64_t t0_ = test_now_ms();                                                              \
        while (!(cond) && test_now_ms() - t0_ < 10000) {                                           \
            step(&U, 1, 0, 0, 1);                                                                  \
            step(&P, 0, 1, uport, 1);                                                              \
            step(&C, 0, 0, 0, 1);                                                                  \
        }                                                                                          \
    } while (0)

int main(void)
{
    node U = {0}, P = {0}, C = {0};
    U.ctx = make();
    P.ctx = make();
    C.ctx = make();
    dx_listen(U.ctx, "127.0.0.1", 0);
    dx_listen(P.ctx, "127.0.0.1", 0);
    uint16_t uport = (uint16_t)dx_listen_port(U.ctx);

    uint32_t conn;
    dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(P.ctx), &conn);
    C.outgoing[C.n_out++] = conn;
    RUN(U.n_acc == 1);
    CHECK(U.n_acc == 1 && P.n_acc == 1 && P.bad == 0);

    /* ping, chat (dropped by the proxy), ping: two pongs come back through the relay. */
    uint8_t nonce[4] = {0, 0, 0, 0}, chat[8] = {2, 0, 'h', 'i'};
    CHECK(dx_send(C.ctx, conn, DX_PKT_PING, nonce, 4) == DX_OK);
    CHECK(dx_send(C.ctx, conn, DX_PKT_CHAT_SEND, chat, 4) == DX_OK);
    CHECK(dx_send(C.ctx, conn, DX_PKT_PING, nonce, 4) == DX_OK);
    RUN(C.pings == 0 && U.pings == 2 && C.data_seen == 2);
    CHECK(U.pings == 2 && U.chats == 0 && C.data_seen == 2);
    CHECK(P.data_seen == 0); /* the proxy application never saw a relayed packet */

    /* Back-pressure: U stops reading while the client keeps pushing through the proxy. Kernel
     * socket buffers absorb some megabytes (more on Linux), so the client sends until it has been
     * held back (dx_send refusing for 300 ms), then U reads again and everything must arrive. */
    enum { N_MAX = 20000, SIZE = 4000 };
    uint8_t* blob = calloc(1, SIZE + 4);
    blob[0] = (uint8_t)(SIZE & 0xFF); /* bytes field: u32 length, then data */
    blob[1] = (uint8_t)(SIZE >> 8);
    unsigned sent = 0;
    int held = 0;
    uint64_t t0 = test_now_ms(), refused_since = 0;
    while (!held && sent < N_MAX && test_now_ms() - t0 < 20000) { /* U is not polled here */
        blob[4] = (uint8_t)sent; /* index, so U can check the order */
        blob[5] = (uint8_t)(sent >> 8);
        int rc = dx_send(C.ctx, conn, DX_PKT_UPLOAD, blob, SIZE + 4);
        if (rc == DX_OK) {
            sent++;
            refused_since = 0;
        } else if (!refused_since) {
            refused_since = test_now_ms();
        } else if (test_now_ms() - refused_since > 300) {
            held = 1;
        }
        step(&P, 0, 1, uport, 0);
        step(&C, 0, 0, 0, 0);
    }
    CHECK(held); /* the client was held back: the proxy did not buffer without limit */
    RUN(U.uploads == sent);
    if (U.uploads != sent || U.bad)
        printf("sent %u, received %u, out of order %u\n", sent, U.uploads, U.bad);
    CHECK(U.uploads == sent && U.bad == 0); /* all of them, in order */
    free(blob);

    /* The client leaves: the proxy closes the upstream side, U learns it. */
    dx_close(C.ctx, conn);
    RUN(U.disconnects == 1 && P.disconnects == 2);
    CHECK(U.disconnects == 1 && P.disconnects == 2);
    dx_destroy(C.ctx);
    dx_destroy(P.ctx);
    dx_destroy(U.ctx);
    return dx_test_done();
}
