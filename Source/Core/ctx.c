/* ctx.c - the context: creation, event loop (dx_poll), delivery queue, listener.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "../Codec/endian.h"
#include "dx_internal.h"

/*
 * In plain words: a context is one self-contained network engine. It reserves every byte of
 * memory it will ever need when it is created (connections, buffers, the ring of received
 * packets), then runs without allocating. dx_poll is its heartbeat: wait for the sockets,
 * move bytes, and hand finished packets to the application as one batch.
 */

#define DEFAULT_MAX_CONNS 256u
#define DEFAULT_MAX_PAYLOAD 65536u
#define DEFAULT_RING_BYTES (4u * 1024u * 1024u)
#define DEFAULT_QUEUE 4096u
#define DEFAULT_IDLE_MS 60000u
#define SCAN_INTERVAL_MS 100u
#define ACCEPT_BURST 64
#define EVENT_BATCH 128

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

/* Copies the user's settings over the defaults: a zero field keeps its default value. */
static int resolve_config(const dx_config* in, dx_config* out)
{
    dx_config_default(out);
    if (!in) return DX_OK;
    if (in->size < offsetof(dx_config, backend)) return DX_ERR_INVALID_ARG; /* too old a layout */
#define TAKE(f)                                                                                    \
    if (in->f) out->f = in->f
    TAKE(max_conns);
    TAKE(max_payload);
    TAKE(rx_ring_bytes);
    TAKE(rx_queue);
    TAKE(idle_timeout_ms);
    TAKE(backend);
#undef TAKE
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

/* ---- queue of packets and events for the application ---- */

static uint32_t queued(const dx_ctx* ctx)
{
    return ctx->q_tail - ctx->q_head;
}

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
    if (len)
        memcpy(dst, payload, len); /* the one copy: out of the connection buffer, into the ring */

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

/* ---- listener ---- */

int dx_listen(dx_ctx* ctx, const char* addr, uint16_t port)
{
    if (!ctx || !addr) return DX_ERR_INVALID_ARG;
    if (ctx->listen_fd >= 0) return DX_ERR_LIMIT;
    uint16_t bound = 0;
    int fd = dx_net_listen(addr, port, &bound);
    if (fd < 0) return fd;
    if (ctx->ops->add(ctx->io, fd, ctx, DX_IO_R) !=
        DX_OK) { /* the context itself tags the listener */
        dx_net_close(fd);
        return DX_ERR_IO;
    }
    ctx->listen_fd = fd;
    ctx->listen_port = bound;
    return DX_OK;
}

int dx_listen_port(const dx_ctx* ctx)
{
    return ctx && ctx->listen_fd >= 0 ? ctx->listen_port : DX_ERR_INVALID_ARG;
}

int dx_connect(dx_ctx* ctx, const char* addr, uint16_t port, uint32_t* out_conn)
{
    if (!ctx || !addr || !out_conn) return DX_ERR_INVALID_ARG;
    int fd = dx_net_connect(addr, port);
    if (fd < 0) return fd;
    int rc = dx_conn_adopt(ctx, fd, 0, 1, out_conn);
    if (rc != DX_OK) dx_net_close(fd);
    return rc;
}

static void accept_all(dx_ctx* ctx)
{
    for (int i = 0; i < ACCEPT_BURST; i++) {
        int fd = dx_net_accept(ctx->listen_fd);
        if (fd < 0) return;
        uint32_t handle;
        if (dx_conn_adopt(ctx, fd, 1, 0, &handle) != DX_OK) dx_net_close(fd); /* table full */
    }
}

/* ---- event loop ---- */

/* One round: wait for the sockets, then process what is ready. */
static void pump(dx_ctx* ctx, int timeout_ms)
{
    if (ctx->stalled_count && dx_ctx_has_room(ctx)) dx_conn_retry_stalled(ctx);

    /* Wake up at least every scan interval so idle connections are noticed on time. */
    if (timeout_ms < 0 || timeout_ms > (int)SCAN_INTERVAL_MS) timeout_ms = (int)SCAN_INTERVAL_MS;

    dx_io_event evs[EVENT_BATCH];
    int n = ctx->ops->wait(ctx->io, evs, EVENT_BATCH, timeout_ms);
    ctx->now_ms = dx_net_now_ms();

    for (int i = 0; i < n; i++) {
        if (evs[i].tag == ctx) {
            accept_all(ctx);
        } else {
            /* Reading pauses while the application has no room: back-pressure. */
            dx_conn_on_event(ctx, (dx_conn*)evs[i].tag, &evs[i], dx_ctx_has_room(ctx));
        }
    }
    if (ctx->now_ms >= ctx->next_scan_ms) {
        ctx->next_scan_ms = ctx->now_ms + SCAN_INTERVAL_MS;
        dx_conn_expire_idle(ctx);
    }
}

int dx_poll(dx_ctx* ctx, dx_packet* out, int max, int timeout_ms)
{
    if (!ctx || !out || max <= 0) return DX_ERR_INVALID_ARG;
    uint64_t deadline = dx_net_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

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
