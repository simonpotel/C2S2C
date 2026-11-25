/* sending.c - the per-connection send buffer: dx_send, dx_send_batch and flushing.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "dx_internal.h"

/*
 * In plain words: dx_send builds the packet (header + payload) inside a circular buffer owned
 * by the connection, then writes as much as the socket accepts. If the socket is full, the rest
 * waits in the buffer and is written when the backend says "writable". If the buffer itself is
 * full, dx_send says DX_ERR_WOULD_BLOCK instead of blocking or allocating.
 */

/* Writes as much of the send buffer as the socket takes. May close the connection on error. */
void dx_conn_flush(dx_ctx* ctx, dx_conn* c)
{
    while (c->tx_len > 0) {
        uint32_t seg = ctx->cfg.tx_bytes - c->tx_head; /* contiguous part before the wrap */
        if (seg > c->tx_len) seg = c->tx_len;
        long n = dx_net_write(c->fd, c->tx + c->tx_head, seg);
        if (n == DX_ERR_WOULD_BLOCK) break;
        if (n < 0) {
            dx_conn_close_now(ctx, c, DX_ERR_IO);
            return;
        }
        c->tx_head = (c->tx_head + (uint32_t)n) % ctx->cfg.tx_bytes;
        c->tx_len -= (uint32_t)n;
        c->last_active_ms = ctx->now_ms;
    }
    if (c->state == DX_C_CLOSING && c->tx_len == 0) {
        dx_conn_close_now(ctx, c, 0); /* graceful close: everything queued has been written */
        return;
    }
    dx_conn_update_interest(ctx, c);
}

/* Copies n bytes into the circular send buffer (the caller checked there is room). */
static void tx_append(dx_ctx* ctx, dx_conn* c, const uint8_t* src, uint32_t n)
{
    uint32_t cap = ctx->cfg.tx_bytes;
    uint32_t pos = (c->tx_head + c->tx_len) % cap;
    uint32_t first = cap - pos < n ? cap - pos : n;
    memcpy(c->tx + pos, src, first);
    if (first < n) memcpy(c->tx, src + first, n - first);
    c->tx_len += n;
}

int dx_send(dx_ctx* ctx, uint32_t conn, uint16_t packet_id, const uint8_t* data, uint32_t len)
{
    dx_conn* c = ctx ? dx_conn_lookup(ctx, conn) : NULL;
    if (!c || c->state == DX_C_CLOSING) return DX_ERR_BAD_CONN;
    if (len > 0 && !data) return DX_ERR_INVALID_ARG;
    if (len > ctx->cfg.max_payload) return DX_ERR_TOO_LARGE;

    /* Servers send ids with bit 15 set (STOC), clients without it (CTOS). */
    if (((packet_id & DX_ID_STOC_BIT) != 0) != (c->server_side != 0)) return DX_ERR_BAD_ID;
    if (!(ctx->cfg.flags & DX_CFG_NO_VALIDATE)) {
        int rc = dx_payload_validate(packet_id, data, len);
        if (rc != DX_OK) return rc; /* refuse to send what the receiver would reject */
    }
    uint32_t frame = DX_HEADER_SIZE + len;
    if (ctx->cfg.tx_bytes - c->tx_len < frame) return DX_ERR_WOULD_BLOCK;

    dx_header h = {
        DX_MAGIC, DX_PROTOCOL_VERSION, c->server_side ? DX_FLAG_STOC : 0u, packet_id, len,
        c->tx_seq};
    uint8_t head[DX_HEADER_SIZE];
    int rc = dx_header_encode(&h, head);
    if (rc != DX_OK) return rc;
    tx_append(ctx, c, head, DX_HEADER_SIZE);
    if (len) tx_append(ctx, c, data, len);
    c->tx_seq++;

    if (c->state == DX_C_OPEN) dx_conn_flush(ctx, c); /* while CONNECTING the data just waits */
    return DX_OK;
}

int dx_send_batch(dx_ctx* ctx, const dx_outgoing* pkts, int count)
{
    if (!pkts || count < 0) return DX_ERR_INVALID_ARG;
    for (int i = 0; i < count; i++) {
        int rc = dx_send(ctx, pkts[i].conn, pkts[i].packet_id, pkts[i].data, pkts[i].length);
        if (rc != DX_OK) return i > 0 ? i : rc;
    }
    return count;
}
