/* bench_codec.c - speed of the codec and the checksum (pure CPU, no sockets).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "C2S2C/c2s2c.h"
#include "c2s2c_packets.h"
#include "c2s2c_simd.h"

/*
 * Output: one line per measure, "<name> <value> <unit>". Throughputs are "higher is better".
 * Each measure runs for about 100 ms and keeps the best of three runs, to ignore interruptions.
 * The volatile sink stops the compiler from deleting the work it can prove is unused.
 */
static volatile uint32_t sink;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef void (*body)(void* ctx, unsigned iterations);

/* Calls body in growing batches until ~100 ms elapsed; returns iterations per second. */
static double rate(body fn, void* ctx)
{
    double best = 0;
    for (int run = 0; run < 3; run++) {
        unsigned n = 1000, total = 0;
        double t0 = now_s(), elapsed = 0;
        while (elapsed < 0.1) {
            fn(ctx, n);
            total += n;
            elapsed = now_s() - t0;
            n *= 2;
        }
        double r = (double)total / elapsed;
        if (r > best) best = r;
    }
    return best;
}

static void header_encode(void* ctx, unsigned n)
{
    (void)ctx;
    c2s2c_header h = {C2S2C_MAGIC, C2S2C_PROTOCOL_VERSION, 0, C2S2C_PKT_PING, 4, 0};
    uint8_t out[C2S2C_HEADER_SIZE];
    for (unsigned i = 0; i < n; i++) {
        h.sequence = i;
        c2s2c_header_encode(&h, out);
        sink += out[10];
    }
}

static void header_decode(void* ctx, unsigned n)
{
    (void)ctx;
    uint8_t in[C2S2C_HEADER_SIZE] = {0xDE, 0xC0, 1, 0, 3, 0, 4, 0, 0, 0, 1, 0, 0, 0};
    c2s2c_header h;
    for (unsigned i = 0; i < n; i++) {
        in[10] = (uint8_t)i;
        c2s2c_header_decode(in, sizeof in, &h);
        sink += h.sequence;
    }
}

static uint8_t chat[64];
static unsigned chat_len;

static void payload_validate(void* ctx, unsigned n)
{
    (void)ctx;
    for (unsigned i = 0; i < n; i++)
        sink += (uint32_t)c2s2c_payload_validate(C2S2C_PKT_CHAT_MESSAGE, chat, chat_len);
}

static uint8_t big[1 << 20];

static void crc_generic(void* ctx, unsigned n)
{
    (void)ctx;
    for (unsigned i = 0; i < n; i++)
        sink += c2s2c_crc32c_generic(i, big, sizeof big);
}

static void crc_dispatch(void* ctx, unsigned n)
{
    (void)ctx;
    for (unsigned i = 0; i < n; i++)
        sink += c2s2c_crc32c(i, big, sizeof big);
}

int main(void)
{
    c2s2c_writer w;
    c2s2c_writer_init(&w, chat, sizeof chat);
    c2s2c_w_u32(&w, 7);
    c2s2c_w_string(&w, "hello world", 11);
    c2s2c_w_u64(&w, 1234);
    chat_len = w.len;
    memset(big, 0xA5, sizeof big);

    printf("header_encode %.1f Mops/s\n", rate(header_encode, NULL) / 1e6);
    printf("header_decode %.1f Mops/s\n", rate(header_decode, NULL) / 1e6);
    printf("payload_validate %.1f Mops/s\n", rate(payload_validate, NULL) / 1e6);
    printf("crc32c_portable %.0f MB/s\n", rate(crc_generic, NULL));
    printf("crc32c_dispatch %.0f MB/s\n", rate(crc_dispatch, NULL));
    printf("# crc32c hardware: %s\n", c2s2c_cpu_has_crc32c() ? "yes" : "no");
    return 0;
}
