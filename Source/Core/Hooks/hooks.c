/* hooks.c - installing hooks and per-packet handlers.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "dx_internal.h"
#include "dx_packets.h"

/*
 * In plain words: instead of looping over dx_poll yourself, you can say "when a packet of this
 * kind arrives, call this function" (a handler) and "tell me when something happens" (hooks).
 * Both are stored in fixed tables made at dx_create, so registering never allocates.
 */

int dx_set_hooks(dx_ctx* ctx, const dx_hooks* hooks)
{
    if (!ctx) return DX_ERR_INVALID_ARG;
    memset(&ctx->hooks, 0, sizeof ctx->hooks);
    if (!hooks) return DX_OK; /* NULL removes every hook */
    if (hooks->size < sizeof *hooks) return DX_ERR_INVALID_ARG;
    ctx->hooks = *hooks;
    return DX_OK;
}

int dx_on_packet(dx_ctx* ctx, uint16_t packet_id, dx_handler fn, void* user)
{
    if (!ctx) return DX_ERR_INVALID_ARG;
    /* Slot dx_registry_count is the default handler (packet id 0). */
    int index = packet_id == 0 ? (int)dx_registry_count : dx_registry_index(packet_id);
    if (index < 0) return DX_ERR_UNKNOWN_ID;
    ctx->handlers[index] = (dx_handler_slot){fn, fn ? user : NULL};
    return DX_OK;
}
