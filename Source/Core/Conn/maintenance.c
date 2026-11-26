/* maintenance.c - periodic work over all connections.
 * SPDX-License-Identifier: MIT */
#include "c2s2c_internal.h"

/* After the application released packets, parse again the connections that were held back. */
void c2s2c_conn_retry_stalled(c2s2c_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count && ctx->stalled_count > 0; i++) {
        c2s2c_conn* c = &ctx->conn_arr[i];
        if (c->state != C2S2C_C_FREE && c->stalled && c2s2c_ctx_has_room(ctx))
            c2s2c_conn_parse(ctx, c);
    }
}

/* Closes connections that have been silent too long: defeats slowloris and dead peers. */
void c2s2c_conn_expire_idle(c2s2c_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count; i++) {
        c2s2c_conn* c = &ctx->conn_arr[i];
        if (c->state != C2S2C_C_FREE && ctx->now_ms - c->last_active_ms > ctx->cfg.idle_timeout_ms)
            c2s2c_conn_close_now(ctx, c, C2S2C_ERR_TIMEOUT);
    }
}
