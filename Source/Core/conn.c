/* conn.c - one connection: reading and framing, sending, closing.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "../Codec/endian.h"
#include "dx_internal.h"

/*
 * In plain words: TCP is a stream of bytes with no packet boundaries. This file cuts that
 * stream back into packets (framing): it collects bytes until a whole header, then a whole
 * payload, are present, checks them, and hands the packet to the application queue. It also
 * writes outgoing packets into a per-connection send buffer and pushes them to the socket.
 * Any invalid byte closes the connection, with the precise reason.
 */

static uint32_t handle_of(const dx_ctx* ctx, const dx_conn* c)
{
    uint32_t index = (uint32_t)(c - ctx->conn_arr);
    return ((c->gen & 0xFFFFu) << 16) | (index + 1);
}

/* Maps a handle to its connection, or NULL if it is unknown, freed, or from an older use. */
static dx_conn* lookup(const dx_ctx* ctx, uint32_t handle)
{
    uint32_t index = (handle & 0xFFFFu);
    if (index == 0 || index > ctx->conns.count) return NULL;
    dx_conn* c = &ctx->conn_arr[index - 1];
    if (c->state == DX_C_FREE || (c->gen & 0xFFFFu) != (handle >> 16)) return NULL;
    return c;
}

/* Tells the backend what to watch: read unless closing, write only if data is waiting. */
static void update_interest(dx_ctx* ctx, dx_conn* c)
{
    uint8_t want = (uint8_t)((c->state == DX_C_CLOSING || c->eof ? 0u : DX_IO_R) |
                             ((c->tx_len > 0 || c->state == DX_C_CONNECTING) ? DX_IO_W : 0u));
    if (want != c->io_events) {
        c->io_events = want;
        (void)ctx->ops->mod(ctx->io, c->fd, c, want);
    }
}

void dx_conn_close_now(dx_ctx* ctx, dx_conn* c, int reason)
{
    uint32_t handle = handle_of(ctx, c);
    ctx->ops->del(ctx->io, c->fd);
    dx_net_close(c->fd);
    if (c->stalled) ctx->stalled_count--;
    c->state = DX_C_FREE;
    c->gen = (c->gen + 1) & 0xFFFFu;
    if (c->gen == 0) c->gen = 1; /* 0 would make a zero handle possible */
    dx_pool_put(&ctx->conns, (uint32_t)(c - ctx->conn_arr));
    dx_ctx_event(ctx, handle, DX_SYS_DISCONNECTED, reason);
}

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
    *handle = handle_of(ctx, c);
    if (!connecting) dx_ctx_event(ctx, *handle, DX_SYS_CONNECTED, 0);
    return DX_OK;
}

/* ---- sending ---- */

/* Writes as much of the send buffer as the socket takes. May close the connection on error. */
static void flush(dx_ctx* ctx, dx_conn* c)
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
    update_interest(ctx, c);
}

/* Copies n bytes into the circular send buffer (caller checked there is room). */
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
    dx_conn* c = ctx ? lookup(ctx, conn) : NULL;
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

    if (c->state == DX_C_OPEN) flush(ctx, c); /* while CONNECTING the data just waits */
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

int dx_close(dx_ctx* ctx, uint32_t conn)
{
    dx_conn* c = ctx ? lookup(ctx, conn) : NULL;
    if (!c) return DX_ERR_BAD_CONN;
    if (c->state == DX_C_CLOSING) return DX_OK;
    if (c->state == DX_C_CONNECTING) {
        dx_conn_close_now(ctx, c, 0);
        return DX_OK;
    }
    c->state = DX_C_CLOSING; /* stop reading, drain what is queued, then close */
    flush(ctx, c);
    return DX_OK;
}

/* ---- receiving ---- */

/* Cuts complete, valid frames out of the receive buffer and queues them for the application. */
static void parse(dx_ctx* ctx, dx_conn* c)
{
    uint32_t off = 0;
    int was_stalled = c->stalled;
    c->stalled = 0;

    while (c->rx_len - off >= DX_HEADER_SIZE) {
        dx_header h;
        /* Header first: reject garbage before waiting for a payload it announces. */
        int rc = dx_header_decode(c->rx + off, DX_HEADER_SIZE, &h);
        if (rc == DX_OK && ((h.flags & DX_FLAG_STOC) != 0) == (c->server_side != 0))
            rc = DX_ERR_BAD_DIRECTION; /* e.g. a client sending a server-to-client packet */
        if (rc == DX_OK && h.length > ctx->cfg.max_payload) rc = DX_ERR_TOO_LARGE;
        if (rc == DX_OK && h.sequence != c->rx_seq) rc = DX_ERR_BAD_SEQUENCE;
        if (rc != DX_OK) {
            if (was_stalled) ctx->stalled_count--;
            dx_conn_close_now(ctx, c, rc);
            return;
        }

        uint32_t total = DX_HEADER_SIZE + h.length;
        if (c->rx_len - off < total) break; /* payload not complete yet */
        const uint8_t* payload = c->rx + off + DX_HEADER_SIZE;
        if (!(ctx->cfg.flags & DX_CFG_NO_VALIDATE)) {
            rc = dx_payload_validate(h.packet_id, payload, h.length);
            if (rc != DX_OK) {
                if (was_stalled) ctx->stalled_count--;
                dx_conn_close_now(ctx, c, rc);
                return;
            }
        }
        if (dx_ctx_deliver(ctx, handle_of(ctx, c), h.packet_id, h.flags, h.sequence, payload,
                           h.length) != DX_OK) {
            c->stalled = 1; /* the application is behind: keep the frame, retry after release */
            break;
        }
        c->rx_seq++;
        off += total;
    }

    /* Move the unconsumed tail (partial frame or stalled frames) to the buffer start. */
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
    if (c->stalled && !was_stalled) ctx->stalled_count++;
    if (!c->stalled && was_stalled) ctx->stalled_count--;

    /* Peer closed: nothing left means a clean end; a partial frame means it died mid-packet. */
    if (c->eof && !c->stalled) dx_conn_close_now(ctx, c, c->rx_len ? DX_ERR_TRUNCATED : 0);
}

static void read_some(dx_ctx* ctx, dx_conn* c)
{
    if (c->rx_len < ctx->frame_max) {
        long n = dx_net_read(c->fd, c->rx + c->rx_len, ctx->frame_max - c->rx_len);
        if (n > 0) {
            c->rx_len += (uint32_t)n;
            c->last_active_ms = ctx->now_ms;
        } else if (n == 0) {
            c->eof = 1;
            update_interest(ctx, c); /* stop watching reads: EOF would fire forever */
        } else if (n != DX_ERR_WOULD_BLOCK) {
            dx_conn_close_now(ctx, c, DX_ERR_IO);
            return;
        }
    }
    parse(ctx, c);
}

void dx_conn_on_event(dx_ctx* ctx, dx_conn* c, const dx_io_event* ev, int can_read)
{
    if (c->state == DX_C_FREE) return; /* closed earlier in this same batch of events */

    if (c->state == DX_C_CONNECTING) {
        if (!(ev->events & DX_IO_W) && !ev->error) return;
        if (dx_net_connect_result(c->fd) != DX_OK) {
            dx_conn_close_now(ctx, c, DX_ERR_IO);
            return;
        }
        c->state = DX_C_OPEN;
        dx_ctx_event(ctx, handle_of(ctx, c), DX_SYS_CONNECTED, 0);
        flush(ctx, c); /* send what was queued while connecting */
        return;
    }
    if (ev->events & DX_IO_W) {
        flush(ctx, c);
        if (c->state == DX_C_FREE) return;
    }
    if (c->state == DX_C_CLOSING) {
        if (ev->error) dx_conn_close_now(ctx, c, DX_ERR_IO); /* peer is gone: nothing to drain */
        return;
    }
    if ((ev->events & DX_IO_R) && can_read) read_some(ctx, c);
}

void dx_conn_retry_stalled(dx_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count && ctx->stalled_count > 0; i++) {
        dx_conn* c = &ctx->conn_arr[i];
        if (c->state != DX_C_FREE && c->stalled && dx_ctx_has_room(ctx)) parse(ctx, c);
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

void dx_conn_close_all(dx_ctx* ctx)
{
    for (uint32_t i = 0; i < ctx->conns.count; i++) {
        dx_conn* c = &ctx->conn_arr[i];
        if (c->state != DX_C_FREE) dx_net_close(c->fd);
    }
}
