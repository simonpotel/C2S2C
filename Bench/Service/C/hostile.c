/* hostile.c - robustness check of a Duplex-protocol server: hostile and broken traffic.
 * SPDX-License-Identifier: MIT */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * Usage: hostile <port>      Prints one line per case ("PASS"/"FAIL" and why) and a summary.
 * For every case it opens raw connections and sends exactly the bytes it wants, then checks two
 * things: the server did what it should with that connection (closed it, or served it), and the
 * server is still healthy (a fresh connection gets a pong). It knows nothing about the server's
 * language or library: only the wire protocol of ping (id 3) and pong (id 0x8003), documented in
 * DOCUMENTATION.md. The server must close idle connections within 3 seconds.
 */
static uint16_t port;
static int passed, failed;

static void put16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t* p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}

/* A frame with any header values. The payload (bytes 0x01) is written only when the announced
 * length is below 8; the caller's buffer must hold 14 + that many bytes. */
static size_t frame(uint8_t* out, unsigned magic, unsigned ver, unsigned flags, unsigned id,
                    uint32_t len, uint32_t seq)
{
    put16(out, magic);
    out[2] = (uint8_t)ver;
    out[3] = (uint8_t)flags;
    put16(out + 4, id);
    put32(out + 6, len);
    put32(out + 10, seq);
    size_t payload = len < 8 ? len : 0; /* only small payloads are really written */
    memset(out + 14, 1, payload);
    return 14 + payload;
}

static int connect_raw(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Waits up to ms for data or hang-up. Returns bytes read, 0 = closed by the peer, -1 = still open.
 */
static int wait_read(int fd, uint8_t* buf, size_t cap, int ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return -1;
    ssize_t n = recv(fd, buf, cap, 0);
    return n < 0 ? 0 : (int)n; /* a reset counts as closed */
}

/* Is the server alive and correct? A fresh connection must get a pong for a valid ping. */
static int healthy(void)
{
    int fd = connect_raw();
    if (fd < 0) return 0;
    uint8_t f[32], r[64];
    size_t n = frame(f, 0xC0DE, 1, 0, 3, 4, 0);
    ssize_t w = write(fd, f, n);
    int got = w == (ssize_t)n ? wait_read(fd, r, sizeof r, 2000) : -1;
    close(fd);
    return got == 18 && r[4] == 0x03 && r[5] == 0x80; /* pong id 0x8003 */
}

static void verdict(const char* name, int ok, const char* why)
{
    printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, ok ? "" : why);
    ok ? passed++ : failed++;
}

/* The server must close a connection that sent these bytes (and stay healthy). */
static void must_close(const char* name, const uint8_t* bytes, size_t n, int wait_ms)
{
    int fd = connect_raw();
    uint8_t r[64];
    if (fd >= 0 && n) (void)!write(fd, bytes, n);
    int got = fd >= 0 ? wait_read(fd, r, sizeof r, wait_ms) : -1;
    if (fd >= 0) close(fd);
    if (got != 0) {
        verdict(name, 0, got < 0 ? "connection left open" : "server answered");
        return;
    }
    verdict(name, healthy(), "server unhealthy afterwards");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    port = (uint16_t)atoi(argv[1]);
    uint8_t f[256], r[256];

    /* Valid traffic must keep working however it is cut. */
    verdict("valid ping", healthy(), "no pong");
    {
        int fd = connect_raw();
        size_t n = frame(f, 0xC0DE, 1, 0, 3, 4, 0);
        for (size_t i = 0; i < n; i++) {
            (void)!write(fd, f + i, 1);
            usleep(2000);
        }
        int got = wait_read(fd, r, sizeof r, 2000);
        close(fd);
        verdict("valid ping, one byte at a time", got == 18, "no pong");
    }
    {
        int fd = connect_raw();
        uint8_t three[54];
        for (int i = 0; i < 3; i++)
            frame(three + 18 * i, 0xC0DE, 1, 0, 3, 4, (uint32_t)i);
        (void)!write(fd, three, sizeof three);
        int total = 0, got;
        while (total < 54 && (got = wait_read(fd, r, sizeof r, 2000)) > 0)
            total += got;
        close(fd);
        verdict("three frames in one write", total == 54, "wrong number of pongs");
    }

    /* Invalid traffic must close the connection. */
    size_t n;
    n = frame(f, 0xBEEF, 1, 0, 3, 4, 0);
    must_close("bad magic", f, n, 3000);
    n = frame(f, 0xC0DE, 9, 0, 3, 4, 0);
    must_close("bad version", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 0x80, 3, 4, 0);
    must_close("reserved flag bit", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 1, 0x8003, 4, 0);
    must_close("client sends a server-to-client packet", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 0, 3, 0xFFFFFFFFu, 0);
    must_close("length 4 GiB", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 0, 3, 2000, 0);
    must_close("length above the server's limit", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 0, 3, 4, 5);
    must_close("sequence number 5 first", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 0, 0x0100, 4, 0);
    must_close("unknown packet id", f, n, 3000);
    n = frame(f, 0xC0DE, 1, 0, 3, 5, 0);
    must_close("ping with a 5-byte payload", f, n, 3000);
    {
        uint8_t junk[100000];
        for (size_t i = 0; i < sizeof junk; i++)
            junk[i] = (uint8_t)(i * 2654435761u >> 13);
        must_close("100 KB of garbage", junk, sizeof junk, 3000);
    }
    must_close("one byte, then silence (slowloris)", (const uint8_t*)"\xDE", 1, 3500);

    /* Abrupt endings and floods: the server must simply stay healthy. */
    {
        int fd = connect_raw();
        n = frame(f, 0xC0DE, 1, 0, 3, 4, 0);
        (void)!write(fd, f, 16); /* header and half the payload */
        close(fd);
        verdict("hang up in the middle of a packet", healthy(), "server unhealthy afterwards");
    }
    {
        for (int i = 0; i < 300; i++) {
            int fd = connect_raw();
            n = frame(f, 0xBEEF, 1, 0, 3, 4, 0);
            if (fd >= 0) {
                (void)!write(fd, f, n);
                close(fd);
            }
        }
        verdict("300 connections with bad magic", healthy(), "server unhealthy afterwards");
    }
    {
        int fds[600], opened = 0;
        for (int i = 0; i < 600; i++) {
            fds[i] = connect_raw();
            opened += fds[i] >= 0;
        }
        for (int i = 0; i < 600; i++)
            if (fds[i] >= 0) close(fds[i]);
        verdict("600 idle connections opened, then dropped", opened > 500 && healthy(),
                "server unhealthy afterwards");
    }
    printf("robustness %d/%d\n", passed, passed + failed);
    return failed != 0;
}
