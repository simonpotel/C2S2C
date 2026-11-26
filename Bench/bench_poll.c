/* bench_poll.c - round trips over loopback TCP: latency (p50/p99) and pipelined throughput.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "C2S2C/c2s2c.h"
#include "c2s2c_packets.h"

/*
 * A server context answers every ping with a pong; a client context measures. Both run in this
 * one thread, polled in turn, so the numbers include the library plus the kernel's loopback,
 * and no thread switching. Output lines are "<name> <value> <unit>".
 */
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static int cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* Serves the server side once: answers every ping with the same payload. */
static void serve(c2s2c_ctx* s)
{
    c2s2c_packet pk[64];
    int n = c2s2c_poll(s, pk, 64, 0);
    for (int i = 0; i < n; i++)
        if (pk[i].packet_id == C2S2C_PKT_PING)
            c2s2c_send(s, pk[i].conn, C2S2C_PKT_PONG, pk[i].data, pk[i].length);
    c2s2c_release(s, n);
}

/* Receives pongs on the client side; returns how many arrived. */
static int collect(c2s2c_ctx* c, uint32_t* conn_up)
{
    c2s2c_packet pk[64];
    int n = c2s2c_poll(c, pk, 64, 0), pongs = 0;
    for (int i = 0; i < n; i++) {
        if (pk[i].packet_id == C2S2C_PKT_PONG) pongs++;
        if (pk[i].packet_id == C2S2C_SYS_CONNECTED) (*conn_up)++;
    }
    c2s2c_release(c, n);
    return pongs;
}

int main(void)
{
    c2s2c_ctx *s = c2s2c_create(NULL), *c = c2s2c_create(NULL);
    if (!s || !c || c2s2c_listen(s, "127.0.0.1", 0) != C2S2C_OK) return 1;
    uint32_t conn, up = 0;
    c2s2c_connect(c, "127.0.0.1", (uint16_t)c2s2c_listen_port(s), &conn);
    while (!up) {
        serve(s);
        collect(c, &up);
    }

    uint8_t ping[4] = {1, 2, 3, 4};
    enum { SAMPLES = 20000 };
    static double lat[SAMPLES];
    for (int i = 0; i < SAMPLES + 500; i++) { /* the first 500 are warm-up and not recorded */
        double t0 = now_us();
        c2s2c_send(c, conn, C2S2C_PKT_PING, ping, 4);
        int got = 0;
        while (!got) {
            serve(s);
            got = collect(c, &up);
        }
        if (i >= 500) lat[i - 500] = now_us() - t0;
    }
    qsort(lat, SAMPLES, sizeof lat[0], cmp);
    printf("roundtrip_p50 %.1f us\n", lat[SAMPLES / 2]);
    printf("roundtrip_p99 %.1f us\n", lat[SAMPLES * 99 / 100]);

    /* Pipelined: keep 32 pings in flight for one second. */
    unsigned pongs = 0, inflight = 0;
    double t0 = now_us();
    while (now_us() - t0 < 1e6) {
        while (inflight < 32 && c2s2c_send(c, conn, C2S2C_PKT_PING, ping, 4) == C2S2C_OK)
            inflight++;
        serve(s);
        int g = collect(c, &up);
        pongs += (unsigned)g;
        inflight -= (unsigned)g;
    }
    printf("pipelined_roundtrips %.0f packets/s\n", (double)pongs / ((now_us() - t0) / 1e6));
    c2s2c_destroy(c);
    c2s2c_destroy(s);
    return 0;
}
