/* context.c - creating and destroying a context, and its settings.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "dx_internal.h"

/*
 * In plain words: a context is one self-contained network engine. It reserves every byte of
 * memory it will ever need when it is created (connections, buffers, the ring of received
 * packets), then runs without allocating.
 */

#define DEFAULT_MAX_CONNS 256u
#define DEFAULT_MAX_PAYLOAD 65536u
#define DEFAULT_RING_BYTES (4u * 1024u * 1024u)
#define DEFAULT_QUEUE 4096u
#define DEFAULT_IDLE_MS 60000u

static void* default_alloc(size_t size, void* user)
{
    (void)user;
    return malloc(size);
}

static void default_release(void* ptr, void* user)
{
    (void)user;
    free(ptr);
}

void dx_config_default(dx_config* cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->size = sizeof *cfg;
    cfg->max_conns = DEFAULT_MAX_CONNS;
    cfg->max_payload = DEFAULT_MAX_PAYLOAD;
    cfg->rx_ring_bytes = DEFAULT_RING_BYTES;
    cfg->rx_queue = DEFAULT_QUEUE;
    cfg->tx_bytes = 4u * (DEFAULT_MAX_PAYLOAD + DX_HEADER_SIZE);
    cfg->idle_timeout_ms = DEFAULT_IDLE_MS;
    cfg->backend = DX_BACKEND_AUTO;
    cfg->alloc.alloc = default_alloc;
    cfg->alloc.release = default_release;
}

/* Copies the user's settings over the defaults (a zero field keeps its default) and checks them. */
static int resolve_config(const dx_config* in, dx_config* out)
{
    dx_config_default(out);
    if (!in) return DX_OK;
    if (in->size < offsetof(dx_config, backend)) return DX_ERR_INVALID_ARG; /* too old a layout */
    if (in->max_conns) out->max_conns = in->max_conns;
    if (in->max_payload) out->max_payload = in->max_payload;
    if (in->rx_ring_bytes) out->rx_ring_bytes = in->rx_ring_bytes;
    if (in->rx_queue) out->rx_queue = in->rx_queue;
    if (in->idle_timeout_ms) out->idle_timeout_ms = in->idle_timeout_ms;
    if (in->backend) out->backend = in->backend;
    out->flags = in->flags;
    if (in->alloc.alloc && in->alloc.release) out->alloc = in->alloc;
    /* tx_bytes defaults from the resolved max_payload, not from the default one. */
    out->tx_bytes = in->tx_bytes ? in->tx_bytes : 4u * (out->max_payload + DX_HEADER_SIZE);

    if (out->max_conns > 65535u || out->max_payload > DX_MAX_PAYLOAD) return DX_ERR_INVALID_ARG;
    /* The ring must hold two maximal packets, or a full ring could never accept the next one. */
    if (out->rx_ring_bytes / 2u < out->max_payload) return DX_ERR_INVALID_ARG;
    /* One frame must fit in the send buffer, or dx_send could never succeed. */
    if (out->tx_bytes < out->max_payload + DX_HEADER_SIZE) return DX_ERR_INVALID_ARG;
    return DX_OK;
}

static void release_all(dx_ctx* ctx)
{
    const dx_alloc* a = &ctx->cfg.alloc;
    if (ctx->io) ctx->ops->close(ctx->io, a);
    a->release(ctx->conn_arr, a->user);
    a->release(ctx->rx_mem, a->user);
    a->release(ctx->tx_mem, a->user);
    a->release(ctx->ring_mem, a->user);
    a->release(ctx->q, a->user);
    a->release(ctx, a->user);
}

dx_ctx* dx_create(const dx_config* user_cfg)
{
    dx_config cfg;
    if (resolve_config(user_cfg, &cfg) != DX_OK) return NULL;
    const dx_io_ops* ops = dx_io_get(cfg.backend);
    if (!ops) return NULL;

    /* Sizes are computed in 64 bits and checked, so a huge config cannot wrap around. */
    uint64_t frame = (uint64_t)cfg.max_payload + DX_HEADER_SIZE;
    uint64_t rx_total = frame * cfg.max_conns, tx_total = (uint64_t)cfg.tx_bytes * cfg.max_conns;
    uint32_t q_cap = cfg.rx_queue + 2u * cfg.max_conns; /* + room for connect/disconnect events */
    if (rx_total > SIZE_MAX / 2 || tx_total > SIZE_MAX / 2) return NULL;

    dx_ctx* ctx = cfg.alloc.alloc(sizeof *ctx, cfg.alloc.user);
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg = cfg;
    ctx->ops = ops;
    ctx->listen_fd = -1;
    ctx->frame_max = (uint32_t)frame;
    ctx->q_cap = q_cap;

    const dx_alloc* a = &ctx->cfg.alloc;
    ctx->conn_arr = a->alloc(sizeof(dx_conn) * cfg.max_conns, a->user);
    ctx->rx_mem = a->alloc((size_t)rx_total, a->user);
    ctx->tx_mem = a->alloc((size_t)tx_total, a->user);
    ctx->ring_mem = a->alloc(cfg.rx_ring_bytes, a->user);
    ctx->q = a->alloc(sizeof(dx_entry) * q_cap, a->user);
    ctx->io = ctx->conn_arr && ctx->rx_mem && ctx->tx_mem && ctx->ring_mem && ctx->q
                  ? ops->open(a, cfg.max_conns + 1u)
                  : NULL;
    if (!ctx->io) {
        release_all(ctx);
        return NULL;
    }

    memset(ctx->conn_arr, 0, sizeof(dx_conn) * cfg.max_conns);
    memset(ctx->q, 0, sizeof(dx_entry) * q_cap);
    dx_pool_init(&ctx->conns, (uint8_t*)ctx->conn_arr, sizeof(dx_conn), cfg.max_conns);
    for (uint32_t i = 0; i < cfg.max_conns; i++)
        ctx->conn_arr[i].gen = 1;
    dx_ring_init(&ctx->ring, ctx->ring_mem, cfg.rx_ring_bytes);
    ctx->now_ms = dx_net_now_ms();
    return ctx;
}

void dx_destroy(dx_ctx* ctx)
{
    if (!ctx) return;
    dx_conn_close_all(ctx);
    if (ctx->listen_fd >= 0) dx_net_close(ctx->listen_fd);
    release_all(ctx);
}
