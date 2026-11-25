/* dispatch.c - dx_dispatch: poll, then call hooks, tunnels and handlers.
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"
#include "dx_packets.h"

#define DISPATCH_BATCH 64

/* Reason carried by a DX_SYS_DISCONNECTED event (a little-endian 32-bit integer). */
static int32_t reason_of(const dx_packet* p)
{
    return (int32_t)((uint32_t)p->data[0] | (uint32_t)p->data[1] << 8 | (uint32_t)p->data[2] << 16 |
                     (uint32_t)p->data[3] << 24);
}

static void handle_event(dx_ctx* ctx, const dx_packet* p)
{
    const dx_hooks* h = &ctx->hooks;
    if (p->packet_id == DX_SYS_CONNECTED) {
        if (h->on_connect) h->on_connect(ctx, p->conn, h->user);
        return;
    }
    int reason = (int)reason_of(p);
    dx_tunnel_on_disconnect(ctx, p->conn); /* the other end of a tunnel goes down too */
    if (reason < 0 && h->on_error) h->on_error(ctx, p->conn, reason, h->user);
    if (h->on_disconnect) h->on_disconnect(ctx, p->conn, reason, h->user);
}

/* Order for a data packet: after_recv hook (may drop), then tunnel (may relay), then handler. */
static void handle_packet(dx_ctx* ctx, const dx_packet* p)
{
    if (ctx->hooks.after_recv && ctx->hooks.after_recv(ctx, p, ctx->hooks.user)) return;
    if (dx_tunnel_forward(ctx, p)) return;
    int index = dx_registry_index(p->packet_id);
    dx_handler_slot* slot = &ctx->handlers[index >= 0 ? (uint32_t)index : dx_registry_count];
    if (!slot->fn) slot = &ctx->handlers[dx_registry_count]; /* fall back to the default handler */
    if (slot->fn) slot->fn(ctx, p, slot->user);
}

int dx_dispatch(dx_ctx* ctx, int timeout_ms)
{
    dx_packet pk[DISPATCH_BATCH];
    int n = dx_poll(ctx, pk, DISPATCH_BATCH, timeout_ms);
    for (int i = 0; i < n; i++) {
        if (pk[i].packet_id == DX_SYS_CONNECTED || pk[i].packet_id == DX_SYS_DISCONNECTED)
            handle_event(ctx, &pk[i]);
        else
            handle_packet(ctx, &pk[i]);
    }
    if (n > 0) dx_release(ctx, n);
    return n;
}
