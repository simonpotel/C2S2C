/* server.c - the benchmark service written directly against libc2s2c (C reference).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>

#include "C2S2C/c2s2c.h"
#include "c2s2c_packets.h"

/* Answers every ping with a pong. Prints its port first. This is the best case: no language layer.
 */
int main(void)
{
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_conns = 4096;
    cfg.max_payload = 1024;
    cfg.idle_timeout_ms = 1000;
    c2s2c_ctx* ctx = c2s2c_create(&cfg);
    if (!ctx || c2s2c_listen(ctx, "127.0.0.1", 0) != C2S2C_OK) return 1;
    printf("%d\n", c2s2c_listen_port(ctx));
    fflush(stdout);
    c2s2c_packet pk[256];
    for (;;) {
        int n = c2s2c_poll(ctx, pk, 256, 100);
        for (int i = 0; i < n; i++)
            if (pk[i].packet_id == C2S2C_PKT_PING)
                c2s2c_send(ctx, pk[i].conn, C2S2C_PKT_PONG, pk[i].data, pk[i].length);
        c2s2c_release(ctx, n);
    }
}
