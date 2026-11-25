/* maintenance.c - periodic work over all connections.
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"

/* After the application released packets, parse again the connections that were held back. */
void dx_conn_retry_stalled(dx_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count && ctx->stalled_count > 0; i++) {
        dx_conn* c = &ctx->conn_arr[i];
        if (c->state != DX_C_FREE && c->stalled && dx_ctx_has_room(ctx)) dx_conn_parse(ctx, c);
    }
}

/* Closes connections that have been silent too long: defeats slowloris and dead peers. */
void dx_conn_expire_idle(dx_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count; i++) {
        dx_conn* c = &ctx->conn_arr[i];
        if (c->state != DX_C_FREE && ctx->now_ms - c->last_active_ms > ctx->cfg.idle_timeout_ms)
            dx_conn_close_now(ctx, c, DX_ERR_TIMEOUT);
    }
}
