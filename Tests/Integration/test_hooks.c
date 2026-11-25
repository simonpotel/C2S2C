/* test_hooks.c - handlers, hooks and tunnels used through dx_dispatch.
 * SPDX-License-Identifier: MIT */
#include "../net_test.h"
#include "../raw_peer.h"

/*
 * Real use cases: a server that answers pings with a handler, a server that blocks chat
 * messages with a hook, and a proxy (tunnel) that relays a client to the real server while
 * dropping the messages its filter does not like.
 */
typedef struct {
    dx_ctx* ctx;
    unsigned connects, disconnects, errors, pings, chats, pongs, dropped, rejected, other;
    int32_t last_error;
    uint32_t last_conn;
} app;

static dx_ctx* make(void)
{
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_conns = 8;
    cfg.backend = test_backend();
    cfg.max_payload = 4096;
    cfg.rx_ring_bytes = 1u << 20;
    cfg.tx_bytes = 0;
    return dx_create(&cfg);
}

static void run(app** apps, int n, int rounds)
{
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < n; i++)
            dx_dispatch(apps[i]->ctx, 0);
}

/* Spins the given contexts until `cond` holds (or 10 s). */
#define SPIN(apps, n, cond)                                                                        \
    do {                                                                                           \
        uint64_t t0_ = test_now_ms();                                                              \
        while (!(cond) && test_now_ms() - t0_ < 10000)                                             \
            run(apps, n, 1);                                                                       \
    } while (0)

static void on_ping(dx_ctx* ctx, const dx_packet* p, void* user)
{
    app* a = user;
    a->pings++;
    (void)dx_send(ctx, p->conn, DX_PKT_PONG, p->data, p->length); /* replying from a handler */
}

static void on_pong(dx_ctx* ctx, const dx_packet* p, void* user)
{
    (void)ctx;
    (void)p;
    ((app*)user)->pongs++;
}

static void on_chat(dx_ctx* ctx, const dx_packet* p, void* user)
{
    (void)ctx;
    (void)p;
    ((app*)user)->chats++;
}

static void on_other(dx_ctx* ctx, const dx_packet* p, void* user)
{
    (void)ctx;
    (void)p;
    ((app*)user)->other++;
}

static void on_connect(dx_ctx* ctx, uint32_t conn, void* user)
{
    (void)ctx;
    ((app*)user)->connects++;
    ((app*)user)->last_conn = conn;
}

static void on_disconnect(dx_ctx* ctx, uint32_t conn, int reason, void* user)
{
    (void)ctx;
    (void)conn;
    (void)reason;
    ((app*)user)->disconnects++;
}

static void on_error(dx_ctx* ctx, uint32_t conn, int err, void* user)
{
    (void)ctx;
    (void)conn;
    ((app*)user)->errors++;
    ((app*)user)->last_error = err;
}

/* after_recv hook: silently drops chat messages that contain "spam". */
static int drop_spam(dx_ctx* ctx, const dx_packet* p, void* user)
{
    (void)ctx;
    if (p->packet_id != DX_PKT_CHAT_SEND) return 0;
    dx_reader r;
    uint16_t n;
    dx_reader_init(&r, p->data, p->length);
    const char* s = dx_r_string(&r, &n);
    if (n >= 4 && memcmp(s, "spam", 4) == 0) {
        ((app*)user)->dropped++;
        return 1;
    }
    return 0;
}

/* before_send hook: refuses to send pings (e.g. a rate limit that is exhausted). */
static int refuse_ping(dx_ctx* ctx, uint32_t conn, uint16_t id, const uint8_t* d, uint32_t n,
                       void* user)
{
    (void)ctx;
    (void)conn;
    (void)d;
    (void)n;
    if (id != DX_PKT_PING) return 0;
    ((app*)user)->rejected++;
    return 1;
}

static void send_chat(dx_ctx* ctx, uint32_t conn, const char* text)
{
    uint8_t b[64];
    dx_writer w;
    dx_writer_init(&w, b, sizeof b);
    dx_w_string(&w, text, (uint16_t)strlen(text));
    CHECK(dx_send(ctx, conn, DX_PKT_CHAT_SEND, b, w.len) == DX_OK);
}

static void send_ping(dx_ctx* ctx, uint32_t conn, uint32_t nonce, int expect)
{
    uint8_t b[4] = {(uint8_t)nonce, 0, 0, 0};
    CHECK(dx_send(ctx, conn, DX_PKT_PING, b, 4) == expect);
}

/* Handlers and hooks on a plain client/server pair. */
static void test_handlers_and_hooks(void)
{
    app S = {0}, C = {0};
    S.ctx = make();
    C.ctx = make();
    app* both[] = {&S, &C};
    dx_listen(S.ctx, "127.0.0.1", 0);

    CHECK(dx_on_packet(S.ctx, DX_PKT_PING, on_ping, &S) == DX_OK);
    CHECK(dx_on_packet(S.ctx, DX_PKT_CHAT_SEND, on_chat, &S) == DX_OK);
    CHECK(dx_on_packet(S.ctx, 0, on_other, &S) == DX_OK); /* default handler */
    CHECK(dx_on_packet(S.ctx, 0x1234, on_chat, &S) == DX_ERR_UNKNOWN_ID);
    CHECK(dx_on_packet(C.ctx, DX_PKT_PONG, on_pong, &C) == DX_OK);

    dx_hooks hs = {sizeof hs, 0, on_connect, on_disconnect, NULL, drop_spam, on_error, &S};
    dx_hooks hc = {sizeof hc, 0, on_connect, on_disconnect, refuse_ping, NULL, NULL, &C};
    CHECK(dx_set_hooks(S.ctx, &hs) == DX_OK && dx_set_hooks(C.ctx, &hc) == DX_OK);

    uint32_t conn;
    dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(S.ctx), &conn);
    SPIN(both, 2, S.connects == 1 && C.connects == 1);
    CHECK(S.connects == 1 && C.connects == 1);

    /* A handler answers: ping out, pong back. */
    dx_set_hooks(C.ctx,
                 &(dx_hooks){sizeof(dx_hooks), 0, on_connect, on_disconnect, NULL, NULL, NULL, &C});
    send_ping(C.ctx, conn, 7, DX_OK);
    SPIN(both, 2, C.pongs == 1);
    CHECK(S.pings == 1 && C.pongs == 1);

    /* before_send hook vetoes pings; the packet is not sent. */
    dx_set_hooks(C.ctx, &hc);
    send_ping(C.ctx, conn, 8, DX_ERR_REJECTED);
    CHECK(C.rejected == 1);

    /* after_recv hook drops spam; normal chat reaches its handler. */
    send_chat(C.ctx, conn, "spam spam");
    send_chat(C.ctx, conn, "hello");
    SPIN(both, 2, S.chats == 1 && S.dropped == 1);
    CHECK(S.chats == 1 && S.dropped == 1);

    /* Packets without a handler of their own reach the default handler. */
    uint8_t b[16];
    dx_writer w;
    dx_writer_init(&w, b, sizeof b);
    dx_w_string(&w, "me", 2);
    dx_w_bytes(&w, NULL, 0);
    dx_on_packet(S.ctx, DX_PKT_CHAT_SEND, NULL, NULL); /* remove the chat handler */
    send_chat(C.ctx, conn, "now unhandled");
    SPIN(both, 2, S.other == 1);
    CHECK(S.other == 1 && S.chats == 1);

    /* on_error fires, then on_disconnect, for a protocol violation. */
    int raw = raw_connect(S.ctx);
    uint8_t bad[14];
    raw_header(bad, 0xBEEF, 1, 0, DX_PKT_PING, 0, 0);
    CHECK(write(raw, bad, 14) == 14);
    SPIN(both, 2, S.errors == 1);
    CHECK(S.errors == 1 && S.last_error == DX_ERR_BAD_MAGIC);
    close(raw);

    /* Closing: both sides' on_disconnect run. */
    dx_close(C.ctx, conn);
    SPIN(both, 2, C.disconnects == 1 && S.disconnects == 2); /* raw peer + this client */
    CHECK(C.disconnects == 1 && S.disconnects == 2);
    dx_destroy(S.ctx);
    dx_destroy(C.ctx);
}

/* tunnel filter: forwards everything except chat messages. */
static int filter_chat(dx_ctx* ctx, const dx_packet* p, int from_first, void* user)
{
    (void)ctx;
    (void)from_first;
    if (p->packet_id != DX_PKT_CHAT_SEND) return 0;
    ((app*)user)->dropped++;
    return 1;
}

/* Proxy hook: when a client connects, open the upstream connection and tie both together. */
typedef struct {
    app a;
    uint16_t upstream_port;
    int tunnels_made;
} proxy;

static void proxy_on_connect(dx_ctx* ctx, uint32_t conn, void* user)
{
    proxy* p = user;
    if (p->tunnels_made) return; /* this is the upstream's own CONNECTED event */
    uint32_t upstream;
    if (dx_connect(ctx, "127.0.0.1", p->upstream_port, &upstream) == DX_OK &&
        dx_tunnel(ctx, conn, upstream, filter_chat, &p->a) == DX_OK)
        p->tunnels_made++;
}

/* Client -> proxy -> real server, with the proxy dropping chat messages. */
static void test_tunnel(void)
{
    app U = {0}, C = {0};
    proxy P = {{0}, 0, 0};
    U.ctx = make();
    C.ctx = make();
    P.a.ctx = make();
    app* all[] = {&U, &P.a, &C};
    dx_listen(U.ctx, "127.0.0.1", 0);
    dx_listen(P.a.ctx, "127.0.0.1", 0);
    P.upstream_port = (uint16_t)dx_listen_port(U.ctx);
    dx_on_packet(U.ctx, DX_PKT_PING, on_ping, &U);
    dx_on_packet(U.ctx, DX_PKT_CHAT_SEND, on_chat, &U);
    dx_on_packet(C.ctx, DX_PKT_PONG, on_pong, &C);
    dx_set_hooks(P.a.ctx,
                 &(dx_hooks){sizeof(dx_hooks), 0, proxy_on_connect, NULL, NULL, NULL, NULL, &P});
    dx_set_hooks(C.ctx,
                 &(dx_hooks){sizeof(dx_hooks), 0, on_connect, on_disconnect, NULL, NULL, NULL, &C});

    uint32_t conn;
    dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(P.a.ctx), &conn);
    SPIN(all, 3, C.connects == 1 && P.tunnels_made == 1);
    CHECK(P.tunnels_made == 1);

    send_ping(C.ctx, conn, 1, DX_OK); /* relayed to the server, answer relayed back */
    send_chat(C.ctx, conn, "hello");  /* dropped by the filter */
    send_ping(C.ctx, conn, 2, DX_OK);
    SPIN(all, 3, C.pongs == 2);
    CHECK(U.pings == 2 && C.pongs == 2);
    CHECK(P.a.dropped == 1 && U.chats == 0);

    /* The client leaves: the proxy closes the upstream side too. */
    dx_close(C.ctx, conn);
    SPIN(all, 3, U.other == 0 && C.disconnects == 1);
    uint64_t t0 = test_now_ms();
    unsigned before = U.disconnects;
    dx_set_hooks(U.ctx,
                 &(dx_hooks){sizeof(dx_hooks), 0, NULL, on_disconnect, NULL, NULL, NULL, &U});
    while (U.disconnects == before && test_now_ms() - t0 < 10000)
        run(all, 3, 1);
    CHECK(U.disconnects == before + 1);
    dx_destroy(U.ctx);
    dx_destroy(P.a.ctx);
    dx_destroy(C.ctx);
}

int main(void)
{
    test_handlers_and_hooks();
    test_tunnel();
    return dx_test_done();
}
