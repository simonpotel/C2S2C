/* server.c - the benchmark service written directly against libduplex (C reference).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>

#include "Duplex/duplex.h"
#include "dx_packets.h"

/* Answers every ping with a pong. Prints its port first. This is the best case: no language layer.
 */
int main(void)
{
    dx_config cfg;
    dx_config_default(&cfg);
    cfg.max_conns = 4096;
    cfg.max_payload = 1024;
    cfg.idle_timeout_ms = 1000;
    dx_ctx* ctx = dx_create(&cfg);
    if (!ctx || dx_listen(ctx, "127.0.0.1", 0) != DX_OK) return 1;
    printf("%d\n", dx_listen_port(ctx));
    fflush(stdout);
    dx_packet pk[256];
    for (;;) {
        int n = dx_poll(ctx, pk, 256, 100);
        for (int i = 0; i < n; i++)
            if (pk[i].packet_id == DX_PKT_PING)
                dx_send(ctx, pk[i].conn, DX_PKT_PONG, pk[i].data, pk[i].length);
        dx_release(ctx, n);
    }
}
