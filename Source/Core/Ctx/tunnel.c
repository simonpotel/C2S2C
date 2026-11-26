/* tunnel.c - relaying packets between two connections inside the library (c2s2c_tunnel).
 * SPDX-License-Identifier: MIT */
#include "c2s2c_internal.h"

/*
 * A tunnel glues two connections together, like a proxy, without the application seeing the
 * packets: a frame that arrives on one side is checked as usual, then queued on the other side
 * with a fresh sequence number. The only rule is a short list of packet ids to drop. If the far
 * side's buffer is full, the frame stays where it is and reading from this side pauses (the same
 * "stalled" state used when the application is slow), so nothing is lost and nothing grows.
 */

int c2s2c_tunnel(c2s2c_ctx* ctx, uint32_t first, uint32_t second, const uint16_t* drop_ids,
                 uint32_t drop_count)
{
    if (!ctx || first == second || drop_count > C2S2C_TUNNEL_MAX_DROP || (drop_count && !drop_ids))
        return C2S2C_ERR_INVALID_ARG;
    c2s2c_conn* a = c2s2c_conn_lookup(ctx, first);
    c2s2c_conn* b = c2s2c_conn_lookup(ctx, second);
    if (!a || !b) return C2S2C_ERR_BAD_CONN;
    /* One accepted and one outgoing connection, or relayed packets would travel the wrong way. */
    if (a->server_side == b->server_side || a->tunnel || b->tunnel) return C2S2C_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < ctx->cfg.max_tunnels; i++) {
        c2s2c_tunnel_slot* t = &ctx->tunnels[i];
        if (t->first != 0) continue;
        t->first = first;
        t->second = second;
        t->drop_count = drop_count;
        for (uint32_t k = 0; k < drop_count; k++)
            t->drop[k] = drop_ids[k];
        a->tunnel = b->tunnel = (uint16_t)(i + 1);
        return C2S2C_OK;
    }
    return C2S2C_ERR_LIMIT;
}

/* Forwards one checked frame of c to its peer. C2S2C_OK = done (or dropped by the list),
 * C2S2C_ERR_WOULD_BLOCK = peer full (retry later), anything else = the relay is broken. */
int c2s2c_tunnel_relay(c2s2c_ctx* ctx, c2s2c_conn* c, uint16_t id, const uint8_t* payload,
                       uint32_t len)
{
    const c2s2c_tunnel_slot* t = &ctx->tunnels[c->tunnel - 1];
    for (uint32_t k = 0; k < t->drop_count; k++)
        if (t->drop[k] == id) return C2S2C_OK;
    uint32_t self = c2s2c_conn_handle(ctx, c);
    c2s2c_conn* peer = c2s2c_conn_lookup(ctx, t->first == self ? t->second : t->first);
    if (!peer || peer->state == C2S2C_C_CLOSING) return C2S2C_ERR_BAD_CONN;
    return c2s2c_conn_enqueue(ctx, peer, id, payload, len);
}

/* c is closing: forget the relay and close the other end gracefully. */
void c2s2c_tunnel_on_close(c2s2c_ctx* ctx, c2s2c_conn* c)
{
    c2s2c_tunnel_slot* t = &ctx->tunnels[c->tunnel - 1];
    uint32_t self = c2s2c_conn_handle(ctx, c), other = t->first == self ? t->second : t->first;
    c2s2c_conn* peer = c2s2c_conn_lookup(ctx, other);
    t->first = 0;
    c->tunnel = 0;
    if (peer) {
        peer->tunnel = 0;
        (void)c2s2c_close(ctx, other);
    }
}
