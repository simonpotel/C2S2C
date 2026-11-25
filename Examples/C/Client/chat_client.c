/* chat_client.c - a command-line chat client written with Duplex.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dx_packets.h"
#include <Duplex/duplex.h>

/*
 * What it does: connects to a chat server, logs in with a nickname, sends each message given on
 * the command line, then prints the chat messages it receives until it has seen its own
 * messages come back from the broadcast.
 * Usage: chat_client <port> <nickname> <message>...
 */
static dx_ctx* ctx;
static uint32_t conn;
static int logged_in, connected, received;

static void on_login_ok(dx_ctx* c, const dx_packet* p, void* arg)
{
    (void)c;
    (void)p;
    (void)arg;
    logged_in = 1;
}

static void on_message(dx_ctx* c, const dx_packet* p, void* arg)
{
    (void)c;
    (void)arg;
    dx_reader r;
    uint16_t n;
    dx_reader_init(&r, p->data, p->length);
    uint32_t id = dx_r_u32(&r);
    const char* text = dx_r_string(&r, &n);
    printf("[%u] %.*s\n", id, (int)n, text);
    fflush(stdout);
    received++;
}

static void on_connect(dx_ctx* c, uint32_t handle, void* arg)
{
    (void)c;
    (void)handle;
    (void)arg;
    connected = 1;
}

/* Runs the event loop until `*flag` becomes true (or the server goes away). */
static int wait_for(const int* flag, int want)
{
    for (int i = 0; i < 200 && *flag < want; i++)
        dx_dispatch(ctx, 50);
    return *flag >= want;
}

static void send_string(uint16_t id, const char* s)
{
    uint8_t buf[512];
    dx_writer w;
    dx_writer_init(&w, buf, sizeof buf);
    dx_w_string(&w, s, (uint16_t)strlen(s));
    if (w.err || dx_send(ctx, conn, id, buf, w.len) != DX_OK) {
        fprintf(stderr, "cannot send\n");
        exit(1);
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: chat_client <port> <nickname> <message>...\n");
        return 2;
    }
    ctx = dx_create(NULL);
    if (!ctx || dx_connect(ctx, "127.0.0.1", (uint16_t)atoi(argv[1]), &conn) != DX_OK) return 1;
    dx_on_packet(ctx, DX_PKT_LOGIN_OK, on_login_ok, NULL);
    dx_on_packet(ctx, DX_PKT_CHAT_MESSAGE, on_message, NULL);
    dx_hooks hooks = {.size = sizeof hooks, .on_connect = on_connect};
    dx_set_hooks(ctx, &hooks);

    send_string(DX_PKT_LOGIN, argv[2]);
    if (!wait_for(&connected, 1) || !wait_for(&logged_in, 1)) {
        fprintf(stderr, "login failed\n");
        return 1;
    }
    for (int i = 3; i < argc; i++)
        send_string(DX_PKT_CHAT_SEND, argv[i]);
    int ok = wait_for(&received, argc - 3);

    dx_close(ctx, conn);
    for (int i = 0; i < 20; i++)
        dx_dispatch(ctx, 10); /* let the close go out */
    dx_destroy(ctx);
    return ok ? 0 : 1;
}
