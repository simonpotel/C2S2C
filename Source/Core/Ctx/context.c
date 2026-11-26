/* context.c - creating and destroying a context, and its settings.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "c2s2c_internal.h"

/*
 * A context is one self-contained network engine. It reserves every byte of
 * memory it will ever need when it is created (connections, buffers, the ring of received
 * packets), then runs without allocating.
 */

#define DEFAULT_MAX_CONNS 256u
#define DEFAULT_MAX_PAYLOAD 65536u
#define DEFAULT_RING_BYTES (4u * 1024u * 1024u)
#define DEFAULT_QUEUE 4096u
#define DEFAULT_IDLE_MS 60000u
#define DEFAULT_TUNNELS 16u

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

void c2s2c_config_default(c2s2c_config* cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->size = sizeof *cfg;
    cfg->max_conns = DEFAULT_MAX_CONNS;
    cfg->max_payload = DEFAULT_MAX_PAYLOAD;
    cfg->rx_ring_bytes = DEFAULT_RING_BYTES;
    cfg->rx_queue = DEFAULT_QUEUE;
    cfg->tx_bytes = 4u * (DEFAULT_MAX_PAYLOAD + C2S2C_HEADER_SIZE);
    cfg->idle_timeout_ms = DEFAULT_IDLE_MS;
    cfg->max_tunnels = DEFAULT_TUNNELS;
    cfg->backend = C2S2C_BACKEND_AUTO;
    cfg->alloc.alloc = default_alloc;
    cfg->alloc.release = default_release;
}

/* Copies the user's settings over the defaults (a zero field keeps its default) and checks them. */
static int resolve_config(const c2s2c_config* in, c2s2c_config* out)
{
    c2s2c_config_default(out);
    if (!in) return C2S2C_OK;
    if (in->size < offsetof(c2s2c_config, backend))
        return C2S2C_ERR_INVALID_ARG; /* too old a layout */
    if (in->max_conns) out->max_conns = in->max_conns;
    if (in->max_payload) out->max_payload = in->max_payload;
    if (in->rx_ring_bytes) out->rx_ring_bytes = in->rx_ring_bytes;
    if (in->rx_queue) out->rx_queue = in->rx_queue;
    if (in->idle_timeout_ms) out->idle_timeout_ms = in->idle_timeout_ms;
    if (in->backend) out->backend = in->backend;
    /* Fields added after the first release exist only if the caller's struct is big enough. */
    if (in->size >= offsetof(c2s2c_config, reserved) && in->max_tunnels)
        out->max_tunnels = in->max_tunnels;
    out->flags = in->flags;
    if (in->alloc.alloc && in->alloc.release) out->alloc = in->alloc;
    /* tx_bytes defaults from the resolved max_payload, not from the default one. */
    out->tx_bytes = in->tx_bytes ? in->tx_bytes : 4u * (out->max_payload + C2S2C_HEADER_SIZE);

    if (out->max_conns > 65535u || out->max_payload > C2S2C_MAX_PAYLOAD)
        return C2S2C_ERR_INVALID_ARG;
    /* The ring must hold two maximal packets, or a full ring could never accept the next one. */
    if (out->rx_ring_bytes / 2u < out->max_payload) return C2S2C_ERR_INVALID_ARG;
    /* One frame must fit in the send buffer, or c2s2c_send could never succeed. */
    if (out->tx_bytes < out->max_payload + C2S2C_HEADER_SIZE) return C2S2C_ERR_INVALID_ARG;
    return C2S2C_OK;
}

static void release_all(c2s2c_ctx* ctx)
{
    const c2s2c_alloc* a = &ctx->cfg.alloc;
    if (ctx->io) ctx->ops->close(ctx->io, a);
    a->release(ctx->conn_arr, a->user);
    a->release(ctx->rx_mem, a->user);
    a->release(ctx->tx_mem, a->user);
    a->release(ctx->ring_mem, a->user);
    a->release(ctx->q, a->user);
    a->release(ctx->dirty, a->user);
    a->release(ctx->tunnels, a->user);
    a->release(ctx, a->user);
    c2s2c_net_shutdown(); /* pairs with c2s2c_net_init in c2s2c_create */
}

c2s2c_ctx* c2s2c_create(const c2s2c_config* user_cfg)
{
    c2s2c_config cfg;
    if (resolve_config(user_cfg, &cfg) != C2S2C_OK) return NULL;
    const c2s2c_io_ops* ops = c2s2c_io_get(cfg.backend);
    if (!ops) return NULL;

    /* Sizes are computed in 64 bits and checked, so a huge config cannot wrap around. */
    uint64_t frame = (uint64_t)cfg.max_payload + C2S2C_HEADER_SIZE;
    uint64_t rx_total = frame * cfg.max_conns, tx_total = (uint64_t)cfg.tx_bytes * cfg.max_conns;
    uint32_t q_cap = cfg.rx_queue + 2u * cfg.max_conns; /* + room for connect/disconnect events */
    if (rx_total > SIZE_MAX / 2 || tx_total > SIZE_MAX / 2) return NULL;

    if (c2s2c_net_init() != C2S2C_OK) return NULL;
    c2s2c_ctx* ctx = cfg.alloc.alloc(sizeof *ctx, cfg.alloc.user);
    if (!ctx) {
        c2s2c_net_shutdown();
        return NULL;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg = cfg;
    ctx->ops = ops;
    ctx->listen_fd = -1;
    ctx->frame_max = (uint32_t)frame;
    ctx->q_cap = q_cap;

    const c2s2c_alloc* a = &ctx->cfg.alloc;
    ctx->conn_arr = a->alloc(sizeof(c2s2c_conn) * cfg.max_conns, a->user);
    ctx->rx_mem = a->alloc((size_t)rx_total, a->user);
    ctx->tx_mem = a->alloc((size_t)tx_total, a->user);
    ctx->ring_mem = a->alloc(cfg.rx_ring_bytes, a->user);
    ctx->q = a->alloc(sizeof(c2s2c_entry) * q_cap, a->user);
    ctx->dirty = a->alloc(sizeof(uint32_t) * cfg.max_conns, a->user);
    ctx->tunnels = a->alloc(sizeof(c2s2c_tunnel_slot) * cfg.max_tunnels, a->user);
    ctx->io = ctx->conn_arr && ctx->rx_mem && ctx->tx_mem && ctx->ring_mem && ctx->q &&
                      ctx->dirty && ctx->tunnels
                  ? ops->open(a, cfg.max_conns + 1u)
                  : NULL;
    if (!ctx->io) {
        release_all(ctx);
        return NULL;
    }

    memset(ctx->conn_arr, 0, sizeof(c2s2c_conn) * cfg.max_conns);
    memset(ctx->q, 0, sizeof(c2s2c_entry) * q_cap);
    memset(ctx->tunnels, 0, sizeof(c2s2c_tunnel_slot) * cfg.max_tunnels);
    c2s2c_pool_init(&ctx->conns, (uint8_t*)ctx->conn_arr, sizeof(c2s2c_conn), cfg.max_conns);
    for (uint32_t i = 0; i < cfg.max_conns; i++)
        ctx->conn_arr[i].gen = 1;
    c2s2c_ring_init(&ctx->ring, ctx->ring_mem, cfg.rx_ring_bytes);
    ctx->now_ms = c2s2c_net_now_ms();
    return ctx;
}

void c2s2c_destroy(c2s2c_ctx* ctx)
{
    if (!ctx) return;
    c2s2c_conn_flush_dirty(ctx); /* best effort: what the sockets accept right now */
    c2s2c_conn_close_all(ctx);
    if (ctx->listen_fd >= 0) c2s2c_net_close(ctx->listen_fd);
    release_all(ctx);
}
