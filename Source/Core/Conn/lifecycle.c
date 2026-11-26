/* lifecycle.c - connection handles, opening and closing.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "c2s2c_internal.h"

/*
 * A connection is a slot in a fixed table. The application never sees the slot
 * itself, only a "handle" made of the slot number and a generation counter. When a slot is
 * reused the generation changes, so an old handle can never be mistaken for the new connection.
 */

uint32_t c2s2c_conn_handle(const c2s2c_ctx* ctx, const c2s2c_conn* c)
{
    uint32_t index = (uint32_t)(c - ctx->conn_arr);
    return ((c->gen & 0xFFFFu) << 16) | (index + 1); /* +1 so that a handle is never 0 */
}

/* Maps a handle to its connection, or NULL if it is unknown, freed, or from an older use. */
c2s2c_conn* c2s2c_conn_lookup(const c2s2c_ctx* ctx, uint32_t handle)
{
    uint32_t index = handle & 0xFFFFu;
    if (index == 0 || index > ctx->conns.count) return NULL;
    c2s2c_conn* c = &ctx->conn_arr[index - 1];
    if (c->state == C2S2C_C_FREE || (c->gen & 0xFFFFu) != (handle >> 16)) return NULL;
    return c;
}

/* Tells the backend what to watch: read unless closing, write only if data is waiting. */
void c2s2c_conn_update_interest(c2s2c_ctx* ctx, c2s2c_conn* c)
{
    uint8_t want = (uint8_t)((c->state == C2S2C_C_CLOSING || c->eof ? 0u : C2S2C_IO_R) |
                             ((c->tx_len > 0 || c->state == C2S2C_C_CONNECTING) ? C2S2C_IO_W : 0u));
    if (want != c->io_events) {
        c->io_events = want;
        (void)ctx->ops->mod(ctx->io, c->fd, c, want);
    }
}

/* Takes ownership of a socket and registers it. Announces C2S2C_SYS_CONNECTED unless connecting. */
int c2s2c_conn_adopt(c2s2c_ctx* ctx, int fd, int server_side, int connecting, uint32_t* handle)
{
    uint32_t index = c2s2c_pool_get(&ctx->conns);
    if (index == C2S2C_POOL_NONE) return C2S2C_ERR_LIMIT;
    c2s2c_conn* c = &ctx->conn_arr[index];
    uint32_t gen = c->gen; /* survives reuse */

    /* Reset everything except the generation; buffers are fixed slices of the big blocks. */
    memset(c, 0, sizeof *c);
    c->gen = gen;
    c->fd = fd;
    c->state = connecting ? C2S2C_C_CONNECTING : C2S2C_C_OPEN;
    c->server_side = server_side != 0;
    c->last_active_ms = ctx->now_ms;
    c->rx = ctx->rx_mem + (size_t)index * ctx->frame_max;
    c->tx = ctx->tx_mem + (size_t)index * ctx->cfg.tx_bytes;
    c->io_events = (uint8_t)(C2S2C_IO_R | (connecting ? C2S2C_IO_W : 0u));

    if (ctx->ops->add(ctx->io, fd, c, c->io_events) != C2S2C_OK) {
        c->state = C2S2C_C_FREE;
        c2s2c_pool_put(&ctx->conns, index);
        return C2S2C_ERR_IO;
    }
    *handle = c2s2c_conn_handle(ctx, c);
    if (!connecting) c2s2c_ctx_event(ctx, *handle, C2S2C_SYS_CONNECTED, 0);
    return C2S2C_OK;
}

/* Frees the slot immediately and tells the application why (0 = normal close). */
void c2s2c_conn_close_now(c2s2c_ctx* ctx, c2s2c_conn* c, int reason)
{
    if (c->tunnel) c2s2c_tunnel_on_close(ctx, c); /* the other end of a relay goes down too */
    uint32_t handle = c2s2c_conn_handle(ctx, c);
    ctx->ops->del(ctx->io, c->fd);
    c2s2c_net_close(c->fd);
    if (c->stalled) ctx->stalled_count--;
    c->state = C2S2C_C_FREE;
    c->gen = (c->gen + 1) & 0xFFFFu;
    if (c->gen == 0) c->gen = 1; /* 0 would make a zero handle possible */
    c2s2c_pool_put(&ctx->conns, (uint32_t)(c - ctx->conn_arr));
    c2s2c_ctx_event(ctx, handle, C2S2C_SYS_DISCONNECTED, reason);
}

int c2s2c_close(c2s2c_ctx* ctx, uint32_t conn)
{
    c2s2c_conn* c = ctx ? c2s2c_conn_lookup(ctx, conn) : NULL;
    if (!c) return C2S2C_ERR_BAD_CONN;
    if (c->state == C2S2C_C_CLOSING) return C2S2C_OK;
    if (c->state == C2S2C_C_CONNECTING) {
        c2s2c_conn_close_now(ctx, c, 0);
        return C2S2C_OK;
    }
    c->state = C2S2C_C_CLOSING; /* stop reading, drain what is queued, then close */
    c2s2c_conn_flush(ctx, c);
    return C2S2C_OK;
}

void c2s2c_conn_close_all(c2s2c_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count; i++)
        if (ctx->conn_arr[i].state != C2S2C_C_FREE) c2s2c_net_close(ctx->conn_arr[i].fd);
}
