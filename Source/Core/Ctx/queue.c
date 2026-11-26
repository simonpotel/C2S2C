/* queue.c - the queue of packets and events handed to the application.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "../../Codec/endian.h"
#include "dx_internal.h"

/*
 * Finished packets are copied into the ring (one contiguous block each) and
 * described by an entry in a fixed queue. dx_poll hands entries out; dx_release frees them in
 * the same order, which also frees their bytes in the ring. Three free-running counters
 * describe the queue: head (oldest not released) <= handed (next to hand out) <= tail (next
 * free slot).
 */

static uint32_t queued(const dx_ctx* ctx)
{
    return ctx->q_tail - ctx->q_head;
}

/* Is there room for one more maximal packet? If not, reading from sockets pauses. */
int dx_ctx_has_room(const dx_ctx* ctx)
{
    return queued(ctx) < ctx->cfg.rx_queue && dx_ring_free(&ctx->ring) >= ctx->cfg.max_payload;
}

int dx_ctx_deliver(dx_ctx* ctx, uint32_t conn, uint16_t id, uint8_t flags, uint32_t seq,
                   const uint8_t* payload, uint32_t len)
{
    if (queued(ctx) >= ctx->cfg.rx_queue) return DX_ERR_WOULD_BLOCK;
    uint32_t adv = 0;
    uint8_t* dst = dx_ring_push(&ctx->ring, len, &adv);
    if (!dst) return DX_ERR_WOULD_BLOCK;
    if (len) memcpy(dst, payload, len); /* the one copy: connection buffer to ring */

    dx_entry* e = &ctx->q[ctx->q_tail % ctx->q_cap];
    e->pkt = (dx_packet){conn, seq, dst, len, id, flags, 0};
    e->adv = adv;
    ctx->q_tail++;
    return DX_OK;
}

void dx_ctx_event(dx_ctx* ctx, uint32_t conn, uint16_t sys_id, int32_t reason)
{
    if (queued(ctx) >= ctx->q_cap) return; /* application never released: the event is dropped */
    dx_entry* e = &ctx->q[ctx->q_tail % ctx->q_cap];
    e->adv = 0;
    e->pkt = (dx_packet){conn, 0, e->inl, 0, sys_id, 0, 0};
    if (sys_id == DX_SYS_DISCONNECTED) {
        dx_wr32(e->inl, (uint32_t)reason);
        e->pkt.length = 4;
    }
    ctx->q_tail++;
}

void dx_release(dx_ctx* ctx, int count)
{
    if (!ctx || count <= 0) return;
    uint32_t n = (uint32_t)count, handed = ctx->q_handed - ctx->q_head;
    if (n > handed) n = handed; /* never release what was not handed out */
    while (n--) {
        dx_ring_pop(&ctx->ring, ctx->q[ctx->q_head % ctx->q_cap].adv);
        ctx->q_head++;
    }
}
