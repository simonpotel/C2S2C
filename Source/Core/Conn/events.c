/* events.c - reacting to what the I/O backend reports for one connection.
 * SPDX-License-Identifier: MIT */
#include "c2s2c_internal.h"

/*
 * The backend says "this socket is readable / writable / failed". What that means depends on
 * the connection's state: while connecting, "writable" means the connect finished; once open it
 * means room to send; while closing we only care about draining the send buffer.
 */
void c2s2c_conn_on_event(c2s2c_ctx* ctx, c2s2c_conn* c, const c2s2c_io_event* ev, int can_read)
{
    if (c->state == C2S2C_C_FREE) return; /* closed earlier in this same batch of events */

    if (c->state == C2S2C_C_CONNECTING) {
        if (!(ev->events & C2S2C_IO_W) && !ev->error) return;
        if (c2s2c_net_connect_result(c->fd) != C2S2C_OK) {
            c2s2c_conn_close_now(ctx, c, C2S2C_ERR_IO);
            return;
        }
        c->state = C2S2C_C_OPEN;
        c2s2c_ctx_event(ctx, c2s2c_conn_handle(ctx, c), C2S2C_SYS_CONNECTED, 0);
        c2s2c_conn_flush(ctx, c); /* send what was queued while connecting */
        return;
    }
    if (ev->events & C2S2C_IO_W) {
        c2s2c_conn_flush(ctx, c);
        if (c->state == C2S2C_C_FREE) return;
    }
    if (c->state == C2S2C_C_CLOSING) {
        if (ev->error)
            c2s2c_conn_close_now(ctx, c, C2S2C_ERR_IO); /* peer is gone: nothing to drain */
        return;
    }
    /* Reading pauses while the application has no room: back-pressure. */
    if ((ev->events & C2S2C_IO_R) && can_read) c2s2c_conn_read(ctx, c);
}
