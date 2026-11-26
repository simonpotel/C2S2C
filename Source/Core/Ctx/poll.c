/* poll.c - the event loop: dx_poll.
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"

#define SCAN_INTERVAL_MS 100u /* how often idle connections are looked for */
#define EVENT_BATCH 128

/*
 * Dx_poll is the heartbeat. Each round it waits for the sockets, moves bytes
 * (accept, read, write), and then hands finished packets to the application as one batch.
 */

/* One round: wait for the sockets, then process what is ready. */
static void pump(dx_ctx* ctx, int timeout_ms)
{
    /* Replies queued since the last round leave now, one write per connection. */
    if (ctx->dirty_count) dx_conn_flush_dirty(ctx);
    if (ctx->stalled_count && dx_ctx_has_room(ctx)) dx_conn_retry_stalled(ctx);

    /* Wake up at least every scan interval so idle connections are noticed on time. */
    if (timeout_ms < 0 || timeout_ms > (int)SCAN_INTERVAL_MS) timeout_ms = (int)SCAN_INTERVAL_MS;

    dx_io_event evs[EVENT_BATCH];
    int n = ctx->ops->wait(ctx->io, evs, EVENT_BATCH, timeout_ms);
    ctx->now_ms = dx_net_now_ms();

    for (int i = 0; i < n; i++) {
        if (evs[i].tag == ctx)
            dx_ctx_accept_all(ctx);
        else
            dx_conn_on_event(ctx, (dx_conn*)evs[i].tag, &evs[i], dx_ctx_has_room(ctx));
    }
    if (ctx->now_ms >= ctx->next_scan_ms) {
        ctx->next_scan_ms = ctx->now_ms + SCAN_INTERVAL_MS;
        dx_conn_expire_idle(ctx);
    }
}

int dx_poll(dx_ctx* ctx, dx_packet* out, int max, int timeout_ms)
{
    if (!ctx || !out || max <= 0) return DX_ERR_INVALID_ARG;
    /* Refresh the clock first: the remaining time below is computed from ctx->now_ms, which may
     * date from the previous call (a stale value made a 1 ms poll wait up to 100 ms). */
    ctx->now_ms = dx_net_now_ms();
    uint64_t deadline = ctx->now_ms + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    for (;;) {
        int pending = ctx->q_tail != ctx->q_handed;
        int remaining =
            timeout_ms < 0 ? -1 : (int)(deadline > ctx->now_ms ? deadline - ctx->now_ms : 0);
        pump(ctx, pending ? 0 : remaining);
        if (ctx->q_tail != ctx->q_handed) break; /* something to hand out */
        if (timeout_ms == 0 || (timeout_ms > 0 && ctx->now_ms >= deadline)) break;
        if (!dx_ctx_has_room(ctx)) break; /* nothing can arrive until the application releases */
    }

    int n = 0;
    while (n < max && ctx->q_handed != ctx->q_tail) {
        out[n++] = ctx->q[ctx->q_handed % ctx->q_cap].pkt;
        ctx->q_handed++;
    }
    return n;
}
