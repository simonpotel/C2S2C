/* dx_internal.h - private structures shared by the Core sources (never installed).
 * SPDX-License-Identifier: MIT */
#ifndef DX_INTERNAL_H
#define DX_INTERNAL_H

#include "Duplex/duplex.h"
#include "IO/io.h"
#include "IO/net.h"
#include "Memory/pool.h"
#include "Memory/ring.h"

/* Connection life cycle. */
#define DX_C_FREE 0
#define DX_C_CONNECTING 1 /* connect() started, not finished */
#define DX_C_OPEN 2
#define DX_C_CLOSING 3 /* dx_close called: flushing the send buffer, then closing */

/*
 * One connection. All of them live in a fixed pool created by dx_create, with their buffers
 * carved from two big blocks, so opening or closing a connection never allocates.
 */
typedef struct dx_conn {
    uint32_t link; /* used by the pool while the slot is free: do not touch */
    uint32_t gen;  /* generation: changes each time the slot is reused, so old handles go stale */
    int fd;
    uint8_t state;
    uint8_t server_side; /* 1 = we accepted it (we send STOC, expect CTOS) */
    uint8_t io_events;   /* what the I/O backend currently watches (DX_IO_R / DX_IO_W) */
    uint8_t stalled;     /* complete frames wait in rx because the application is behind */
    uint8_t eof;         /* the peer closed its side */
    uint32_t tx_seq;     /* sequence number of the next packet we send */
    uint32_t rx_seq;     /* sequence number we expect next */
    uint32_t rx_len;     /* bytes currently in the receive buffer */
    uint32_t tx_head;    /* send buffer is circular: oldest unsent byte */
    uint32_t tx_len;     /* unsent bytes */
    uint64_t last_active_ms;
    uint8_t* rx;
    uint8_t* tx;
} dx_conn;

/* A queue entry: what dx_poll hands out. Events keep their tiny payload inside the entry. */
typedef struct dx_entry {
    dx_packet pkt;
    uint32_t adv;   /* ring bytes to free on release (0 for events) */
    uint8_t inl[4]; /* payload of DX_SYS_DISCONNECTED */
} dx_entry;

/* One registered handler (see dx_on_packet). */
typedef struct dx_handler_slot {
    dx_handler fn;
    void* user;
} dx_handler_slot;

/* A relay between two connections (see dx_tunnel). first == 0 means the slot is free. */
typedef struct dx_tunnel_slot {
    uint32_t first, second;
    dx_tunnel_filter filter;
    void* user;
} dx_tunnel_slot;

struct dx_ctx {
    dx_config cfg;
    const dx_io_ops* ops;
    dx_io* io;
    int listen_fd;
    uint16_t listen_port;
    uint32_t frame_max; /* header + max payload: size of a connection's receive buffer */

    dx_pool conns;
    dx_conn* conn_arr; /* the connection slots (the pool hands out indexes into it) */
    uint8_t* rx_mem;
    uint8_t* tx_mem;

    dx_ring ring; /* storage of delivered payloads */
    uint8_t* ring_mem;
    dx_entry* q; /* queue of entries; head <= handed <= tail (free-running counters) */
    uint32_t q_cap, q_head, q_handed, q_tail;

    dx_hooks hooks;
    int in_before_send;        /* guards against a before_send hook calling dx_send */
    dx_handler_slot* handlers; /* one per registry packet, plus the default at the end */
    dx_tunnel_slot* tunnels;

    uint32_t stalled_count;
    uint64_t now_ms;
    uint64_t next_scan_ms;
};

/* Core/Conn/lifecycle.c: handles, opening and closing connections */
uint32_t dx_conn_handle(const dx_ctx* ctx, const dx_conn* c);
dx_conn* dx_conn_lookup(const dx_ctx* ctx, uint32_t handle);
void dx_conn_update_interest(dx_ctx* ctx, dx_conn* c);
int dx_conn_adopt(dx_ctx* ctx, int fd, int server_side, int connecting, uint32_t* handle);
void dx_conn_close_now(dx_ctx* ctx, dx_conn* c, int reason);
void dx_conn_close_all(dx_ctx* ctx);

/* Core/Conn/sending.c: the send buffer */
void dx_conn_flush(dx_ctx* ctx, dx_conn* c);

/* Core/Conn/receiving.c: reading bytes and cutting them into packets */
void dx_conn_read(dx_ctx* ctx, dx_conn* c);
void dx_conn_parse(dx_ctx* ctx, dx_conn* c);

/* Core/Conn/events.c: reacting to what the I/O backend reports */
void dx_conn_on_event(dx_ctx* ctx, dx_conn* c, const dx_io_event* ev, int can_read);

/* Core/Conn/maintenance.c: periodic work over all connections */
void dx_conn_retry_stalled(dx_ctx* ctx);
void dx_conn_expire_idle(dx_ctx* ctx);

/* Core/Ctx/queue.c: the queue of packets and events handed to the application */
int dx_ctx_has_room(const dx_ctx* ctx);
int dx_ctx_deliver(dx_ctx* ctx, uint32_t conn, uint16_t id, uint8_t flags, uint32_t seq,
                   const uint8_t* payload, uint32_t len);
void dx_ctx_event(dx_ctx* ctx, uint32_t conn, uint16_t sys_id, int32_t reason);

/* Core/registry.c: position of a packet in dx_registry, or -1 */
int dx_registry_index(uint16_t packet_id);

/* Core/Hooks/tunnel.c */
int dx_tunnel_forward(dx_ctx* ctx, const dx_packet* p);
void dx_tunnel_on_disconnect(dx_ctx* ctx, uint32_t conn);

/* Core/Ctx/listener.c */
void dx_ctx_accept_all(dx_ctx* ctx);

#endif
