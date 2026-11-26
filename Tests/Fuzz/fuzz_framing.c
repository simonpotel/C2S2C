/* fuzz_framing.c - libFuzzer target: arbitrary bytes arriving on a live connection.
 * SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "c2s2c_internal.h"

/*
 * What this does: each input is written into one end of a socket pair whose other end is a
 * server-side connection of a real context, then the context is run until that connection is
 * gone. This exercises framing, header checks, sequence checks and payload validation exactly
 * as a network peer would, without needing real networking. Sanitizers report any memory error;
 * leaks are reported when the process ends.
 */
static c2s2c_ctx* ctx;

static void drain(uint32_t handle, int* gone)
{
    c2s2c_packet pk[32];
    int n;
    while ((n = c2s2c_poll(ctx, pk, 32, 0)) > 0) {
        for (int i = 0; i < n; i++)
            if (pk[i].packet_id == C2S2C_SYS_DISCONNECTED && pk[i].conn == handle) *gone = 1;
        c2s2c_release(ctx, n);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!ctx) {
        c2s2c_config cfg;
        c2s2c_config_default(&cfg);
        cfg.max_conns = 2;
        cfg.max_payload = 4096;
        cfg.rx_ring_bytes = 1u << 16;
        ctx = c2s2c_create(&cfg);
        if (!ctx) abort();
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    uint32_t handle;
    if (c2s2c_conn_adopt(ctx, sv[0], 1, 0, &handle) != C2S2C_OK) {
        close(sv[0]);
        close(sv[1]);
        return 0;
    }
    size_t off = 0;
    int gone = 0;
    for (int round = 0; round < 2000 && !gone; round++) {
        if (off < size) { /* feed what the socket takes, then serve */
            ssize_t w = write(sv[1], data + off, size - off);
            if (w > 0) off += (size_t)w;
            if (off == size) close(sv[1]), sv[1] = -1; /* then hang up */
        }
        drain(handle, &gone);
    }
    if (!gone) (void)c2s2c_close(ctx, handle), drain(handle, &gone);
    if (sv[1] >= 0) close(sv[1]);
    return 0;
}
