/* receiving.c - reading bytes and cutting them into validated packets (framing).
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "c2s2c_internal.h"

/*
 * TCP is a stream of bytes with no packet boundaries. We collect bytes in the
 * connection's receive buffer until a whole header, then a whole payload, are present. Every
 * header is checked as soon as it is complete (before waiting for the payload it announces),
 * so garbage or a huge length is rejected immediately. Any invalid byte closes the connection
 * with the precise reason.
 */

/* Checks one header against the connection's rules; C2S2C_OK or the reason to close. */
static int check_header(const c2s2c_ctx* ctx, const c2s2c_conn* c, const c2s2c_header* h)
{
    /* A server expects CTOS packets (flag clear), a client expects STOC (flag set). */
    if (((h->flags & C2S2C_FLAG_STOC) != 0) == (c->server_side != 0))
        return C2S2C_ERR_BAD_DIRECTION;
    if (h->length > ctx->cfg.max_payload) return C2S2C_ERR_TOO_LARGE;
    if (h->sequence != c->rx_seq) return C2S2C_ERR_BAD_SEQUENCE; /* lost, repeated or reordered */
    return C2S2C_OK;
}

/* Closes the connection because of a protocol error found while it was being parsed. */
static void reject(c2s2c_ctx* ctx, c2s2c_conn* c, int was_stalled, int reason)
{
    if (was_stalled) ctx->stalled_count--; /* c->stalled was cleared at the start of parse */
    c2s2c_conn_close_now(ctx, c, reason);
}

/* Cuts complete, valid frames out of the receive buffer and queues them for the application. */
void c2s2c_conn_parse(c2s2c_ctx* ctx, c2s2c_conn* c)
{
    uint32_t off = 0;
    int was_stalled = c->stalled;
    c->stalled = 0;

    while (c->rx_len - off >= C2S2C_HEADER_SIZE) {
        c2s2c_header h;
        int rc = c2s2c_header_decode(c->rx + off, C2S2C_HEADER_SIZE, &h);
        if (rc == C2S2C_OK) rc = check_header(ctx, c, &h);
        if (rc != C2S2C_OK) {
            reject(ctx, c, was_stalled, rc);
            return;
        }

        uint32_t total = C2S2C_HEADER_SIZE + h.length;
        if (c->rx_len - off < total) break; /* payload not complete yet */
        const uint8_t* payload = c->rx + off + C2S2C_HEADER_SIZE;
        if (!(ctx->cfg.flags & C2S2C_CFG_NO_VALIDATE)) {
            rc = c2s2c_payload_validate(h.packet_id, payload, h.length);
            if (rc != C2S2C_OK) {
                reject(ctx, c, was_stalled, rc);
                return;
            }
        }
        /* A relayed connection forwards to its peer; any other hands the packet to the app. */
        rc = c->tunnel ? c2s2c_tunnel_relay(ctx, c, h.packet_id, payload, h.length)
                       : c2s2c_ctx_deliver(ctx, c2s2c_conn_handle(ctx, c), h.packet_id, h.flags,
                                           h.sequence, payload, h.length);
        if (rc == C2S2C_ERR_WOULD_BLOCK) {
            c->stalled = 1; /* the app (or the relay peer) is behind: keep the frame, retry later */
            break;
        }
        if (rc != C2S2C_OK) {
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
    if (c->eof && !c->stalled) c2s2c_conn_close_now(ctx, c, c->rx_len ? C2S2C_ERR_TRUNCATED : 0);
}

/* Reads what the socket has and parses it. When a read fills the buffer there is probably more
 * waiting, so it reads again (at most READ_ROUNDS times, so one busy connection cannot starve
 * the others): with big packets the buffer holds about one frame, and one read per readiness
 * event would make the throughput depend on how often the application polls. */
#define READ_ROUNDS 16

void c2s2c_conn_read(c2s2c_ctx* ctx, c2s2c_conn* c)
{
    for (int round = 0; round < READ_ROUNDS; round++) {
        uint32_t space = ctx->frame_max - c->rx_len;
        long n = 0;
        if (space > 0) {
            n = c2s2c_net_read(c->fd, c->rx + c->rx_len, space);
            if (n > 0) {
                c->rx_len += (uint32_t)n;
                c->last_active_ms = ctx->now_ms;
            } else if (n == 0) {
                c->eof = 1;
                c2s2c_conn_update_interest(ctx,
                                           c); /* stop watching reads: EOF would fire forever */
            } else if (n != C2S2C_ERR_WOULD_BLOCK) {
                c2s2c_conn_close_now(ctx, c, C2S2C_ERR_IO);
                return;
            }
        }
        c2s2c_conn_parse(ctx, c);
        if (c->state == C2S2C_C_FREE || c->stalled || c->eof || space == 0 || n < (long)space)
            return;
    }
}
