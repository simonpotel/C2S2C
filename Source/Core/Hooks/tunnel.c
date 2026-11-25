/* tunnel.c - relaying packets between two connections (dx_tunnel).
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"

/*
 * In plain words: a tunnel glues two connections together, like a proxy. A packet that arrives
 * on one is re-sent on the other, after an optional filter looked at it. Nothing is buffered
 * beyond the normal send buffer: if the far side cannot keep up, the tunnel is torn down rather
 * than growing without limit.
 */

static dx_tunnel_slot* find(dx_ctx* ctx, uint32_t conn)
{
    for (uint32_t i = 0; i < ctx->cfg.max_tunnels; i++) {
        dx_tunnel_slot* t = &ctx->tunnels[i];
        if (t->first && (t->first == conn || t->second == conn)) return t;
    }
    return NULL;
}

int dx_tunnel(dx_ctx* ctx, uint32_t first, uint32_t second, dx_tunnel_filter filter, void* user)
{
    if (!ctx || first == 0 || second == 0 || first == second) return DX_ERR_INVALID_ARG;
    if (!dx_conn_lookup(ctx, first) || !dx_conn_lookup(ctx, second)) return DX_ERR_BAD_CONN;
    if (find(ctx, first) || find(ctx, second)) return DX_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < ctx->cfg.max_tunnels; i++) {
        if (ctx->tunnels[i].first == 0) {
            ctx->tunnels[i] = (dx_tunnel_slot){first, second, filter, user};
            return DX_OK;
        }
    }
    return DX_ERR_LIMIT;
}

/* Returns 1 if the packet belongs to a tunnel (relayed or dropped), 0 if it is not tunnelled. */
int dx_tunnel_forward(dx_ctx* ctx, const dx_packet* p)
{
    dx_tunnel_slot* t = find(ctx, p->conn);
    if (!t) return 0;
    int from_first = t->first == p->conn;
    if (t->filter && t->filter(ctx, p, from_first, t->user)) return 1; /* filtered out */

    int rc = dx_send(ctx, from_first ? t->second : t->first, p->packet_id, p->data, p->length);
    if (rc != DX_OK) { /* far side too slow or gone: tear the tunnel down */
        uint32_t a = t->first, b = t->second;
        t->first = 0;
        (void)dx_close(ctx, a);
        (void)dx_close(ctx, b);
    }
    return 1;
}

/* One end disconnected: forget the tunnel and close the other end. */
void dx_tunnel_on_disconnect(dx_ctx* ctx, uint32_t conn)
{
    dx_tunnel_slot* t = find(ctx, conn);
    if (!t) return;
    uint32_t other = t->first == conn ? t->second : t->first;
    t->first = 0;
    (void)dx_close(ctx, other);
}
