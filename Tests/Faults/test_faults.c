/* test_faults.c - hostile or broken traffic against a real server, sent with a raw socket.
 * SPDX-License-Identifier: MIT */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../net_test.h"
#include "../raw_peer.h"

/*
 * The peer here is a plain POSIX socket that writes exactly the bytes we choose, so we can
 * send what no well-behaved client would: one byte at a time, corrupt headers, lies about
 * lengths, a hang-up in the middle of a packet, silence, or a flood. For each case the server
 * must either deliver the right packets or close that connection with the precise reason, and
 * keep serving everyone else.
 */

static dx_ctx* make_server(uint32_t idle_ms, uint32_t queue, uint32_t max_payload)
{
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_conns = 4;
    cfg.backend = test_backend();
    cfg.idle_timeout_ms = idle_ms;
    if (queue) cfg.rx_queue = queue;
    if (max_payload) cfg.max_payload = max_payload;
    cfg.rx_ring_bytes = 1u << 20;
    cfg.tx_bytes = 0;
    dx_ctx* c = dx_create(&cfg);
    if (c) dx_listen(c, "127.0.0.1", 0);
    return c;
}

/* Sends bytes on a raw socket, then runs the server until `cond` holds. */
#define SEND_RAW(fd, bytes, n) CHECK(write((fd), (bytes), (n)) == (ssize_t)(n))

/* One connection that sends `bytes`; the server must close it with exactly `reason`. */
static void expect_rejected(const char* what, const uint8_t* bytes, size_t n, int32_t reason)
{
    side S = {0};
    S.ctx = make_server(0, 0, 0);
    int fd = raw_connect(S.ctx);
    SEND_RAW(fd, bytes, n);
    WAIT_SIDE(S, S.disconnected == 1);
    if (!(S.disconnected == 1 && S.reason == reason))
        printf("  case '%s': reason %d, expected %d\n", what, S.reason, reason);
    CHECK(S.disconnected == 1 && S.reason == reason && S.pings == 0);
    close(fd);
    dx_destroy(S.ctx);
}

static void test_invalid_traffic(void)
{
    uint8_t b[64];
    raw_header(b, 0xBEEF, 1, 0, DX_PKT_PING, 0, 0);
    expect_rejected("bad magic", b, 14, DX_ERR_BAD_MAGIC);
    raw_header(b, DX_MAGIC, 9, 0, DX_PKT_PING, 0, 0);
    expect_rejected("bad version", b, 14, DX_ERR_BAD_VERSION);
    raw_header(b, DX_MAGIC, 1, 0x80, DX_PKT_PING, 0, 0);
    expect_rejected("reserved flag bit", b, 14, DX_ERR_BAD_FLAGS);
    raw_header(b, DX_MAGIC, 1, 1, DX_PKT_PING, 0, 0);
    expect_rejected("direction flag vs id", b, 14, DX_ERR_BAD_ID);
    raw_header(b, DX_MAGIC, 1, 1, DX_PKT_PONG, 4, 0);
    expect_rejected("client sends a server packet", b, 14, DX_ERR_BAD_DIRECTION);
    raw_header(b, DX_MAGIC, 1, 0, DX_PKT_PING, 0xFFFFFFFFu, 0);
    expect_rejected("absurd length", b, 14, DX_ERR_TOO_LARGE);
    raw_header(b, DX_MAGIC, 1, 0, DX_PKT_PING, 100000, 0); /* valid header, above max_payload */
    expect_rejected("length over the configured limit", b, 14, DX_ERR_TOO_LARGE);
    ping_frame(b, 1, 5); /* first packet must have sequence 0 */
    expect_rejected("out of order sequence", b, 18, DX_ERR_BAD_SEQUENCE);
    raw_header(b, DX_MAGIC, 1, 0, 0x0100, 0, 0);
    expect_rejected("unknown packet id", b, 14, DX_ERR_UNKNOWN_ID);
    raw_header(b, DX_MAGIC, 1, 0, DX_PKT_LOGIN, 4, 0); /* login = string; bytes 02 00 C0 80 */
    b[14] = 2;
    b[15] = 0;
    b[16] = 0xC0;
    b[17] = 0x80;
    expect_rejected("invalid UTF-8", b, 18, DX_ERR_BAD_UTF8);
    raw_header(b, DX_MAGIC, 1, 0, DX_PKT_PING, 5, 0); /* ping payload is 4 bytes, not 5 */
    expect_rejected("trailing byte in payload", b, 19, DX_ERR_TRAILING);
}

/* The same valid packets give the same result however the bytes are cut. */
static void test_partial_reads(void)
{
    side S = {0};
    S.ctx = make_server(0, 0, 0);
    int fd = raw_connect(S.ctx);
    uint8_t f[18];
    ping_frame(f, 0, 0);
    for (size_t i = 0; i < 18; i++) { /* one byte at a time, serving in between */
        SEND_RAW(fd, f + i, 1);
        pump_side(&S, 1);
        if (i < 17) CHECK(S.pings == 0); /* nothing is delivered before the last byte */
    }
    WAIT_SIDE(S, S.pings == 1);
    CHECK(S.pings == 1 && S.bad == 0);

    uint8_t two[36]; /* two frames in a single write */
    ping_frame(two, 1, 1);
    ping_frame(two + 18, 2, 2);
    SEND_RAW(fd, two, 36);
    WAIT_SIDE(S, S.pings == 3);
    CHECK(S.pings == 3 && S.bad == 0);

    ping_frame(f, 3, 3); /* a frame split in two halves */
    SEND_RAW(fd, f, 9);
    pump_side(&S, 5);
    CHECK(S.pings == 3);
    SEND_RAW(fd, f + 9, 9);
    WAIT_SIDE(S, S.pings == 4);
    CHECK(S.pings == 4 && S.bad == 0 && S.disconnected == 0);
    close(fd);
    dx_destroy(S.ctx);
}

/* A hang-up between packets is clean; in the middle of one it is a truncation. */
static void test_disconnects(void)
{
    side S = {0};
    S.ctx = make_server(0, 0, 0);
    uint8_t f[18];
    int fd = raw_connect(S.ctx);
    close(fd); /* nothing sent */
    WAIT_SIDE(S, S.disconnected == 1);
    CHECK(S.reason == 0);

    S.disconnected = 0;
    fd = raw_connect(S.ctx);
    ping_frame(f, 0, 0);
    SEND_RAW(fd, f, 16); /* header + half of the payload */
    pump_side(&S, 5);
    close(fd);
    WAIT_SIDE(S, S.disconnected == 1);
    CHECK(S.reason == DX_ERR_TRUNCATED && S.pings == 0);

    S.disconnected = 0;
    fd = raw_connect(S.ctx); /* a whole packet, then hang up: the packet must still arrive */
    ping_frame(f, 0, 0);
    SEND_RAW(fd, f, 18);
    close(fd);
    WAIT_SIDE(S, S.disconnected == 1);
    CHECK(S.reason == 0 && S.pings == 1);
    dx_destroy(S.ctx);
}

/* slowloris: a peer that sends one byte and then nothing is dropped after the idle timeout,
 * while a healthy connection keeps working. */
static void test_slowloris(void)
{
    side S = {0};
    S.ctx = make_server(200, 0, 0);
    int slow = raw_connect(S.ctx);
    uint8_t one = 0xDE;
    SEND_RAW(slow, &one, 1);
    WAIT_SIDE(S, S.disconnected == 1);
    CHECK(S.disconnected == 1 && S.reason == DX_ERR_TIMEOUT);
    close(slow);
    dx_destroy(S.ctx);
}

/* flood: 20 000 packets as fast as the socket allows against a tiny queue. Nothing is lost,
 * order is kept, and while the application holds packets the server just stops reading. */
static void test_flood(void)
{
    enum { N = 20000, QUEUE = 64 };
    side S = {0};
    S.ctx = make_server(0, QUEUE, 4096);
    int fd = raw_connect(S.ctx);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    /* Back-pressure: hold every packet without releasing; the queue fills and polling stops. */
    uint8_t burst[18 * 200];
    for (int i = 0; i < 200; i++)
        ping_frame(burst + 18 * i, (uint32_t)i, (uint32_t)i);
    SEND_RAW(fd, burst, sizeof burst);
    dx_packet pk[256];
    int held = 0;
    uint64_t t0 = test_now_ms();
    while (held < QUEUE + 1 && test_now_ms() - t0 < 3000) {
        int n = dx_poll(S.ctx, pk + held, 256 - held, 1);
        held += n;
    }
    CHECK(held >= QUEUE);                                 /* includes the connected event */
    CHECK(dx_poll(S.ctx, pk + held, 256 - held, 1) == 0); /* full: nothing more is handed out */
    dx_release(S.ctx, held);
    for (int i = 0; i < held; i++)
        on_packet(&S, &pk[i]); /* count them (pointers valid until released) */

    /* Then the rest of the flood with normal serving. */
    uint32_t sent = 200;
    uint8_t f[18];
    t0 = test_now_ms();
    while (S.pings < N && test_now_ms() - t0 < 20000) {
        for (int k = 0; k < 50 && sent < N; k++) {
            ping_frame(f, sent, sent);
            ssize_t w = write(fd, f, 18);
            if (w == 18)
                sent++;
            else
                break; /* socket full: serve, then retry */
        }
        pump_side(&S, 1);
    }
    CHECK(S.pings == N && S.bad == 0 && S.disconnected == 0);
    close(fd);
    dx_destroy(S.ctx);
}

int main(void)
{
    test_invalid_traffic();
    test_partial_reads();
    test_disconnects();
    test_slowloris();
    test_flood();
    return dx_test_done();
}
