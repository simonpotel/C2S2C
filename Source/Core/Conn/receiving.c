/* receiving.c - reading bytes and cutting them into validated packets (framing).
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "dx_internal.h"

/*
 * TCP is a stream of bytes with no packet boundaries. We collect bytes in the
 * connection's receive buffer until a whole header, then a whole payload, are present. Every
 * header is checked as soon as it is complete (before waiting for the payload it announces),
 * so garbage or a huge length is rejected immediately. Any invalid byte closes the connection
 * with the precise reason.
 */

/* Checks one header against the connection's rules; DX_OK or the reason to close. */
static int check_header(const dx_ctx* ctx, const dx_conn* c, const dx_header* h)
{
    /* A server expects CTOS packets (flag clear), a client expects STOC (flag set). */
    if (((h->flags & DX_FLAG_STOC) != 0) == (c->server_side != 0)) return DX_ERR_BAD_DIRECTION;
    if (h->length > ctx->cfg.max_payload) return DX_ERR_TOO_LARGE;
    if (h->sequence != c->rx_seq) return DX_ERR_BAD_SEQUENCE; /* lost, repeated or reordered */
    return DX_OK;
}

/* Closes the connection because of a protocol error found while it was being parsed. */
static void reject(dx_ctx* ctx, dx_conn* c, int was_stalled, int reason)
{
    if (was_stalled) ctx->stalled_count--; /* c->stalled was cleared at the start of parse */
    dx_conn_close_now(ctx, c, reason);
}

/* Cuts complete, valid frames out of the receive buffer and queues them for the application. */
void dx_conn_parse(dx_ctx* ctx, dx_conn* c)
{
    uint32_t off = 0;
    int was_stalled = c->stalled;
    c->stalled = 0;

    while (c->rx_len - off >= DX_HEADER_SIZE) {
        dx_header h;
        int rc = dx_header_decode(c->rx + off, DX_HEADER_SIZE, &h);
        if (rc == DX_OK) rc = check_header(ctx, c, &h);
        if (rc != DX_OK) {
            reject(ctx, c, was_stalled, rc);
            return;
        }

        uint32_t total = DX_HEADER_SIZE + h.length;
        if (c->rx_len - off < total) break; /* payload not complete yet */
        const uint8_t* payload = c->rx + off + DX_HEADER_SIZE;
        if (!(ctx->cfg.flags & DX_CFG_NO_VALIDATE)) {
            rc = dx_payload_validate(h.packet_id, payload, h.length);
            if (rc != DX_OK) {
                reject(ctx, c, was_stalled, rc);
                return;
            }
        }
        /* A relayed connection forwards to its peer; any other hands the packet to the app. */
        rc = c->tunnel ? dx_tunnel_relay(ctx, c, h.packet_id, payload, h.length)
                       : dx_ctx_deliver(ctx, dx_conn_handle(ctx, c), h.packet_id, h.flags,
                                        h.sequence, payload, h.length);
        if (rc == DX_ERR_WOULD_BLOCK) {
            c->stalled = 1; /* the app (or the relay peer) is behind: keep the frame, retry later */
            break;
        }
        if (rc != DX_OK) {
            reject(ctx, c, was_stalled, rc);
            return;
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

/* Reads what the socket has and parses it. When a read fills the buffer there is probably more
 * waiting, so it reads again (at most READ_ROUNDS times, so one busy connection cannot starve
 * the others): with big packets the buffer holds about one frame, and one read per readiness
 * event would make the throughput depend on how often the application polls. */
#define READ_ROUNDS 16

void dx_conn_read(dx_ctx* ctx, dx_conn* c)
{
    for (int round = 0; round < READ_ROUNDS; round++) {
        uint32_t space = ctx->frame_max - c->rx_len;
        long n = 0;
        if (space > 0) {
            n = dx_net_read(c->fd, c->rx + c->rx_len, space);
            if (n > 0) {
                c->rx_len += (uint32_t)n;
                c->last_active_ms = ctx->now_ms;
            } else if (n == 0) {
                c->eof = 1;
                dx_conn_update_interest(ctx, c); /* stop watching reads: EOF would fire forever */
            } else if (n != DX_ERR_WOULD_BLOCK) {
                dx_conn_close_now(ctx, c, DX_ERR_IO);
                return;
            }
        }
        dx_conn_parse(ctx, c);
        if (c->state == DX_C_FREE || c->stalled || c->eof || space == 0 || n < (long)space) return;
    }
}
