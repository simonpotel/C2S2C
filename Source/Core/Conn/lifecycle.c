/* lifecycle.c - connection handles, opening and closing.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "dx_internal.h"

/*
 * In plain words: a connection is a slot in a fixed table. The application never sees the slot
 * itself, only a "handle" made of the slot number and a generation counter. When a slot is
 * reused the generation changes, so an old handle can never be mistaken for the new connection.
 */

uint32_t dx_conn_handle(const dx_ctx* ctx, const dx_conn* c)
{
    uint32_t index = (uint32_t)(c - ctx->conn_arr);
    return ((c->gen & 0xFFFFu) << 16) | (index + 1); /* +1 so that a handle is never 0 */
}

/* Maps a handle to its connection, or NULL if it is unknown, freed, or from an older use. */
dx_conn* dx_conn_lookup(const dx_ctx* ctx, uint32_t handle)
{
    uint32_t index = handle & 0xFFFFu;
    if (index == 0 || index > ctx->conns.count) return NULL;
    dx_conn* c = &ctx->conn_arr[index - 1];
    if (c->state == DX_C_FREE || (c->gen & 0xFFFFu) != (handle >> 16)) return NULL;
    return c;
}

/* Tells the backend what to watch: read unless closing, write only if data is waiting. */
void dx_conn_update_interest(dx_ctx* ctx, dx_conn* c)
{
    uint8_t want = (uint8_t)((c->state == DX_C_CLOSING || c->eof ? 0u : DX_IO_R) |
                             ((c->tx_len > 0 || c->state == DX_C_CONNECTING) ? DX_IO_W : 0u));
    if (want != c->io_events) {
        c->io_events = want;
        (void)ctx->ops->mod(ctx->io, c->fd, c, want);
    }
}

/* Takes ownership of a socket and registers it. Announces DX_SYS_CONNECTED unless connecting. */
int dx_conn_adopt(dx_ctx* ctx, int fd, int server_side, int connecting, uint32_t* handle)
{
    uint32_t index = dx_pool_get(&ctx->conns);
    if (index == DX_POOL_NONE) return DX_ERR_LIMIT;
    dx_conn* c = &ctx->conn_arr[index];
    uint32_t gen = c->gen; /* survives reuse */

    /* Reset everything except the generation; buffers are fixed slices of the big blocks. */
    memset(c, 0, sizeof *c);
    c->gen = gen;
    c->fd = fd;
    c->state = connecting ? DX_C_CONNECTING : DX_C_OPEN;
    c->server_side = server_side != 0;
    c->last_active_ms = ctx->now_ms;
    c->rx = ctx->rx_mem + (size_t)index * ctx->frame_max;
    c->tx = ctx->tx_mem + (size_t)index * ctx->cfg.tx_bytes;
    c->io_events = (uint8_t)(DX_IO_R | (connecting ? DX_IO_W : 0u));

    if (ctx->ops->add(ctx->io, fd, c, c->io_events) != DX_OK) {
        c->state = DX_C_FREE;
        dx_pool_put(&ctx->conns, index);
        return DX_ERR_IO;
    }
    *handle = dx_conn_handle(ctx, c);
    if (!connecting) dx_ctx_event(ctx, *handle, DX_SYS_CONNECTED, 0);
    return DX_OK;
}

/* Frees the slot immediately and tells the application why (0 = normal close). */
void dx_conn_close_now(dx_ctx* ctx, dx_conn* c, int reason)
{
    uint32_t handle = dx_conn_handle(ctx, c);
    ctx->ops->del(ctx->io, c->fd);
    dx_net_close(c->fd);
    if (c->stalled) ctx->stalled_count--;
    c->state = DX_C_FREE;
    c->gen = (c->gen + 1) & 0xFFFFu;
    if (c->gen == 0) c->gen = 1; /* 0 would make a zero handle possible */
    dx_pool_put(&ctx->conns, (uint32_t)(c - ctx->conn_arr));
    dx_ctx_event(ctx, handle, DX_SYS_DISCONNECTED, reason);
}

int dx_close(dx_ctx* ctx, uint32_t conn)
{
    dx_conn* c = ctx ? dx_conn_lookup(ctx, conn) : NULL;
    if (!c) return DX_ERR_BAD_CONN;
    if (c->state == DX_C_CLOSING) return DX_OK;
    if (c->state == DX_C_CONNECTING) {
        dx_conn_close_now(ctx, c, 0);
        return DX_OK;
    }
    c->state = DX_C_CLOSING; /* stop reading, drain what is queued, then close */
    dx_conn_flush(ctx, c);
    return DX_OK;
}

void dx_conn_close_all(dx_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count; i++)
        if (ctx->conn_arr[i].state != DX_C_FREE) dx_net_close(ctx->conn_arr[i].fd);
}
