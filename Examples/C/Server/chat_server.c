/* chat_server.c - a small chat room server written with Duplex.
 * SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dx_packets.h"
#include <Duplex/duplex.h>

/*
 * What it does: clients send `login` with a nickname, then `chat_send` messages. The server
 * answers `login_ok` and broadcasts every message to all logged-in clients as `chat_message`.
 * `ping` is answered with `pong`. Usage: chat_server [port]   (0 or none = any free port;
 * the chosen port is printed on the first line so scripts can find it).
 *
 * How it uses the library: handlers (dx_on_packet) say what to do per packet kind, one hook
 * forgets users who disconnect, and the main loop is just dx_dispatch.
 */
#define MAX_USERS 64

typedef struct {
    uint32_t conn; /* 0 = free slot */
    char nick[32];
} user;

static user users[MAX_USERS];
static volatile sig_atomic_t stop;

static user* find_user(uint32_t conn)
{
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].conn == conn) return &users[i];
    return NULL;
}

static void on_login(dx_ctx* ctx, const dx_packet* p, void* arg)
{
    (void)arg;
    dx_reader r;
    uint16_t n;
    dx_reader_init(&r, p->data, p->length);
    const char* nick = dx_r_string(&r, &n);
    user* u = find_user(0); /* take a free slot */
    if (!u || n >= sizeof u->nick) return;
    u->conn = p->conn;
    memcpy(u->nick, nick, n);
    u->nick[n] = 0;

    uint8_t buf[4];
    dx_writer w;
    dx_writer_init(&w, buf, sizeof buf);
    dx_w_u32(&w, p->conn); /* the handle doubles as the user id */
    dx_send(ctx, p->conn, DX_PKT_LOGIN_OK, buf, w.len);
}

static void on_chat(dx_ctx* ctx, const dx_packet* p, void* arg)
{
    (void)arg;
    if (!find_user(p->conn)) return; /* must log in first */
    dx_reader r;
    uint16_t n;
    dx_reader_init(&r, p->data, p->length);
    const char* text = dx_r_string(&r, &n);

    uint8_t buf[512];
    dx_writer w;
    dx_writer_init(&w, buf, sizeof buf);
    dx_w_u32(&w, p->conn);
    dx_w_string(&w, text, n);
    dx_w_u64(&w, (uint64_t)time(NULL));
    if (w.err) return;
    for (int i = 0; i < MAX_USERS; i++) /* broadcast to everybody who is logged in */
        if (users[i].conn) dx_send(ctx, users[i].conn, DX_PKT_CHAT_MESSAGE, buf, w.len);
}

static void on_ping(dx_ctx* ctx, const dx_packet* p, void* arg)
{
    (void)arg;
    dx_send(ctx, p->conn, DX_PKT_PONG, p->data, p->length); /* same nonce back */
}

static void on_disconnect(dx_ctx* ctx, uint32_t conn, int reason, void* arg)
{
    (void)ctx;
    (void)reason;
    (void)arg;
    user* u = find_user(conn);
    if (u) u->conn = 0;
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char** argv)
{
    dx_ctx* ctx = dx_create(NULL);
    if (!ctx || dx_listen(ctx, "127.0.0.1", argc > 1 ? (uint16_t)atoi(argv[1]) : 0) != DX_OK) {
        fprintf(stderr, "cannot start the server\n");
        return 1;
    }
    dx_on_packet(ctx, DX_PKT_LOGIN, on_login, NULL);
    dx_on_packet(ctx, DX_PKT_CHAT_SEND, on_chat, NULL);
    dx_on_packet(ctx, DX_PKT_PING, on_ping, NULL);
    dx_hooks hooks = {.size = sizeof hooks, .on_disconnect = on_disconnect};
    dx_set_hooks(ctx, &hooks);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("%d\n", dx_listen_port(ctx));
    fflush(stdout);
    while (!stop)
        dx_dispatch(ctx, 200);
    dx_destroy(ctx);
    return 0;
}
