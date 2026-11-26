/* loadgen.c - load generator for the service benchmark: many connections, pipelined pings.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Duplex/duplex.h"
#include "dx_packets.h"

/*
 * Usage: loadgen <port> <connections> <seconds> <in-flight per connection>
 * Every connection keeps a fixed number of pings in flight; each pong triggers the next ping.
 * It measures completed round trips per second and the latency of each one (microseconds,
 * from a histogram, so memory does not depend on the duration). The same program drives every
 * server implementation, so the comparison only depends on the server.
 */
#define MAX_INFLIGHT 16
#define BUCKETS 200000 /* 1 microsecond each: latencies above 200 ms land in the last bucket */

typedef struct {
    uint32_t handle;
    uint64_t sent_at[MAX_INFLIGHT]; /* send times, oldest first (pongs come back in order) */
    unsigned head, count;
} link;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static link* links;
static uint32_t* hist;
static int by_handle[65536]; /* low 16 bits of a handle -> index in links */
static unsigned lost, inflight;
static uint64_t done_total, measuring_from;
static dx_ctx* ctx;

static void fire(link* l)
{
    uint8_t nonce[4] = {1, 2, 3, 4};
    if (l->count < inflight && dx_send(ctx, l->handle, DX_PKT_PING, nonce, 4) == DX_OK) {
        l->sent_at[(l->head + l->count++) % MAX_INFLIGHT] = now_us();
    }
}

static unsigned pump(void)
{
    dx_packet pk[256];
    int n = dx_poll(ctx, pk, 256, 0);
    unsigned pongs = 0;
    uint64_t t = now_us();
    for (int i = 0; i < n; i++) {
        if (pk[i].packet_id == DX_SYS_DISCONNECTED) {
            lost++;
        } else if (pk[i].packet_id == DX_PKT_PONG) {
            link* l = &links[by_handle[pk[i].conn & 0xFFFF]];
            if (l->count) {
                uint64_t lat = t - l->sent_at[l->head];
                l->head = (l->head + 1) % MAX_INFLIGHT;
                l->count--;
                if (t >= measuring_from) {
                    hist[lat < BUCKETS ? lat : BUCKETS - 1]++;
                    done_total++;
                }
            }
            fire(l);
            pongs++;
        }
    }
    dx_release(ctx, n);
    return pongs;
}

static double percentile(double p)
{
    uint64_t want = (uint64_t)((double)done_total * p), seen = 0;
    for (unsigned i = 0; i < BUCKETS; i++)
        if ((seen += hist[i]) >= want) return (double)i;
    return BUCKETS;
}

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    uint16_t port = (uint16_t)atoi(argv[1]);
    unsigned conns = (unsigned)atoi(argv[2]), seconds = (unsigned)atoi(argv[3]);
    inflight = (unsigned)atoi(argv[4]);
    if (inflight > MAX_INFLIGHT || conns > 60000) return 2;

    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_conns = conns + 1;
    cfg.max_payload = 256;
    cfg.rx_ring_bytes = 1u << 20;
    cfg.rx_queue = 16384;
    cfg.tx_bytes = 4096;
    cfg.idle_timeout_ms = 3600000;
    ctx = dx_create(&cfg);
    links = calloc(conns, sizeof *links);
    hist = calloc(BUCKETS, sizeof *hist);
    if (!ctx || !links || !hist) return 1;

    /* Connect in bursts (the kernel's accept queue is short) and wait until all are up. */
    unsigned up = 0;
    for (unsigned i = 0; i < conns; i++) {
        if (dx_connect(ctx, "127.0.0.1", port, &links[i].handle) != DX_OK) return 1;
        by_handle[links[i].handle & 0xFFFF] = (int)i;
        if (i % 32 == 31) {
            dx_packet pk[256];
            int n = dx_poll(ctx, pk, 256, 1);
            for (int k = 0; k < n; k++)
                up += pk[k].packet_id == DX_SYS_CONNECTED;
            dx_release(ctx, n);
        }
    }
    uint64_t deadline = now_us() + 10000000u;
    while (up < conns && now_us() < deadline) {
        dx_packet pk[256];
        int n = dx_poll(ctx, pk, 256, 10);
        for (int k = 0; k < n; k++)
            up += pk[k].packet_id == DX_SYS_CONNECTED;
        dx_release(ctx, n);
    }
    if (up < conns) {
        printf("connections %u of %u\n", up, conns);
        return 1;
    }

    /* One second of warm-up, then the measured period. */
    uint64_t start = now_us();
    measuring_from = start + 1000000u;
    for (unsigned i = 0; i < conns; i++)
        for (unsigned k = 0; k < inflight; k++)
            fire(&links[i]);
    while (now_us() < measuring_from + (uint64_t)seconds * 1000000u)
        pump();

    printf("throughput %.0f round_trips/s\n", (double)done_total / seconds);
    printf("p50 %.0f us\n", percentile(0.50));
    printf("p99 %.0f us\n", percentile(0.99));
    printf("lost_connections %u\n", lost);
    dx_destroy(ctx);
    return 0;
}
