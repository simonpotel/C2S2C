/* c2s2c_internal.h - private structures shared by the Core sources (never installed).
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_INTERNAL_H
#define C2S2C_INTERNAL_H

#include "C2S2C/c2s2c.h"
#include "IO/io.h"
#include "IO/net.h"
#include "Memory/pool.h"
#include "Memory/ring.h"

/* Connection life cycle. */
#define C2S2C_C_FREE 0
#define C2S2C_C_CONNECTING 1 /* connect() started, not finished */
#define C2S2C_C_OPEN 2
#define C2S2C_C_CLOSING 3 /* c2s2c_close called: flushing the send buffer, then closing */

/*
 * One connection. All of them live in a fixed pool created by c2s2c_create, with their buffers
 * carved from two big blocks, so opening or closing a connection never allocates.
 */
typedef struct c2s2c_conn {
    uint32_t link; /* used by the pool while the slot is free: do not touch */
    uint32_t gen;  /* generation: changes each time the slot is reused, so old handles go stale */
    int fd;
    uint8_t state;
    uint8_t server_side; /* 1 = we accepted it (we send STOC, expect CTOS) */
    uint8_t io_events;   /* what the I/O backend currently watches (C2S2C_IO_R / C2S2C_IO_W) */
    uint8_t stalled;     /* complete frames wait in rx because the application is behind */
    uint8_t eof;         /* the peer closed its side */
    uint8_t dirty;       /* listed in ctx->dirty: has queued bytes to write */
    uint16_t tunnel;     /* 1 + index of its c2s2c_tunnel slot, or 0 */
    uint32_t tx_seq;     /* sequence number of the next packet we send */
    uint32_t rx_seq;     /* sequence number we expect next */
    uint32_t rx_len;     /* bytes currently in the receive buffer */
    uint32_t tx_head;    /* send buffer is circular: oldest unsent byte */
    uint32_t tx_len;     /* unsent bytes */
    uint64_t last_active_ms;
    uint8_t* rx;
    uint8_t* tx;
} c2s2c_conn;

/* A queue entry: what c2s2c_poll hands out. Events keep their tiny payload inside the entry. */
typedef struct c2s2c_entry {
    c2s2c_packet pkt;
    uint32_t adv;   /* ring bytes to free on release (0 for events) */
    uint8_t inl[4]; /* payload of C2S2C_SYS_DISCONNECTED */
} c2s2c_entry;

/* A relay between two connections (see c2s2c_tunnel). first == 0 means the slot is free. */
typedef struct c2s2c_tunnel_slot {
    uint32_t first, second; /* connection handles */
    uint32_t drop_count;
    uint16_t drop[C2S2C_TUNNEL_MAX_DROP]; /* packet ids that are not relayed */
} c2s2c_tunnel_slot;

struct c2s2c_ctx {
    c2s2c_config cfg;
    const c2s2c_io_ops* ops;
    c2s2c_io* io;
    int listen_fd;
    uint16_t listen_port;
    uint32_t frame_max; /* header + max payload: size of a connection's receive buffer */

    c2s2c_pool conns;
    c2s2c_conn* conn_arr; /* the connection slots (the pool hands out indexes into it) */
    uint8_t* rx_mem;
    uint8_t* tx_mem;

    c2s2c_ring ring; /* storage of delivered payloads */
    uint8_t* ring_mem;
    c2s2c_entry* q; /* queue of entries; head <= handed <= tail (free-running counters) */
    uint32_t q_cap, q_head, q_handed, q_tail;

    c2s2c_tunnel_slot* tunnels;
    uint32_t* dirty; /* connections with queued bytes not yet written (see c2s2c_flush) */
    uint32_t dirty_count;

    uint32_t stalled_count;
    uint64_t now_ms;
    uint64_t next_scan_ms;
};

/* Core/Conn/lifecycle.c: handles, opening and closing connections */
uint32_t c2s2c_conn_handle(const c2s2c_ctx* ctx, const c2s2c_conn* c);
c2s2c_conn* c2s2c_conn_lookup(const c2s2c_ctx* ctx, uint32_t handle);
void c2s2c_conn_update_interest(c2s2c_ctx* ctx, c2s2c_conn* c);
int c2s2c_conn_adopt(c2s2c_ctx* ctx, int fd, int server_side, int connecting, uint32_t* handle);
void c2s2c_conn_close_now(c2s2c_ctx* ctx, c2s2c_conn* c, int reason);
void c2s2c_conn_close_all(c2s2c_ctx* ctx);

/* Core/Conn/sending.c: the send buffer */
void c2s2c_conn_flush(c2s2c_ctx* ctx, c2s2c_conn* c);
int c2s2c_conn_enqueue(c2s2c_ctx* ctx, c2s2c_conn* c, uint16_t id, const uint8_t* data,
                       uint32_t len);
void c2s2c_conn_flush_dirty(c2s2c_ctx* ctx);

/* Core/Conn/receiving.c: reading bytes and cutting them into packets */
void c2s2c_conn_read(c2s2c_ctx* ctx, c2s2c_conn* c);
void c2s2c_conn_parse(c2s2c_ctx* ctx, c2s2c_conn* c);

/* Core/Conn/events.c: reacting to what the I/O backend reports */
void c2s2c_conn_on_event(c2s2c_ctx* ctx, c2s2c_conn* c, const c2s2c_io_event* ev, int can_read);

/* Core/Conn/maintenance.c: periodic work over all connections */
void c2s2c_conn_retry_stalled(c2s2c_ctx* ctx);
void c2s2c_conn_expire_idle(c2s2c_ctx* ctx);

/* Core/Ctx/queue.c: the queue of packets and events handed to the application */
int c2s2c_ctx_has_room(const c2s2c_ctx* ctx);
int c2s2c_ctx_deliver(c2s2c_ctx* ctx, uint32_t conn, uint16_t id, uint8_t flags, uint32_t seq,
                      const uint8_t* payload, uint32_t len);
void c2s2c_ctx_event(c2s2c_ctx* ctx, uint32_t conn, uint16_t sys_id, int32_t reason);

/* Core/Ctx/tunnel.c */
int c2s2c_tunnel_relay(c2s2c_ctx* ctx, c2s2c_conn* c, uint16_t id, const uint8_t* payload,
                       uint32_t len);
void c2s2c_tunnel_on_close(c2s2c_ctx* ctx, c2s2c_conn* c);

/* Core/Ctx/listener.c */
void c2s2c_ctx_accept_all(c2s2c_ctx* ctx);

#endif
