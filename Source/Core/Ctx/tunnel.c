/* tunnel.c - relaying packets between two connections inside the library (dx_tunnel).
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"

/*
 * A tunnel glues two connections together, like a proxy, without the application seeing the
 * packets: a frame that arrives on one side is checked as usual, then queued on the other side
 * with a fresh sequence number. The only rule is a short list of packet ids to drop. If the far
 * side's buffer is full, the frame stays where it is and reading from this side pauses (the same
 * "stalled" state used when the application is slow), so nothing is lost and nothing grows.
 */

int dx_tunnel(dx_ctx* ctx, uint32_t first, uint32_t second, const uint16_t* drop_ids,
              uint32_t drop_count)
{
    if (!ctx || first == second || drop_count > DX_TUNNEL_MAX_DROP || (drop_count && !drop_ids))
        return DX_ERR_INVALID_ARG;
    dx_conn* a = dx_conn_lookup(ctx, first);
    dx_conn* b = dx_conn_lookup(ctx, second);
    if (!a || !b) return DX_ERR_BAD_CONN;
    /* One accepted and one outgoing connection, or relayed packets would travel the wrong way. */
    if (a->server_side == b->server_side || a->tunnel || b->tunnel) return DX_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < ctx->cfg.max_tunnels; i++) {
        dx_tunnel_slot* t = &ctx->tunnels[i];
        if (t->first != 0) continue;
        t->first = first;
        t->second = second;
        t->drop_count = drop_count;
        for (uint32_t k = 0; k < drop_count; k++)
            t->drop[k] = drop_ids[k];
        a->tunnel = b->tunnel = (uint16_t)(i + 1);
        return DX_OK;
    }
    return DX_ERR_LIMIT;
}

/* Forwards one checked frame of c to its peer. DX_OK = done (or dropped by the list),
 * DX_ERR_WOULD_BLOCK = peer full (retry later), anything else = the relay is broken. */
int dx_tunnel_relay(dx_ctx* ctx, dx_conn* c, uint16_t id, const uint8_t* payload, uint32_t len)
{
    const dx_tunnel_slot* t = &ctx->tunnels[c->tunnel - 1];
    for (uint32_t k = 0; k < t->drop_count; k++)
        if (t->drop[k] == id) return DX_OK;
    uint32_t self = dx_conn_handle(ctx, c);
    dx_conn* peer = dx_conn_lookup(ctx, t->first == self ? t->second : t->first);
    if (!peer || peer->state == DX_C_CLOSING) return DX_ERR_BAD_CONN;
    return dx_conn_enqueue(ctx, peer, id, payload, len);
}

/* c is closing: forget the relay and close the other end gracefully. */
void dx_tunnel_on_close(dx_ctx* ctx, dx_conn* c)
{
    dx_tunnel_slot* t = &ctx->tunnels[c->tunnel - 1];
    uint32_t self = dx_conn_handle(ctx, c), other = t->first == self ? t->second : t->first;
    dx_conn* peer = dx_conn_lookup(ctx, other);
    t->first = 0;
    c->tunnel = 0;
    if (peer) {
        peer->tunnel = 0;
        (void)dx_close(ctx, other);
    }
}
