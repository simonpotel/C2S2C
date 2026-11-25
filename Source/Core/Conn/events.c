/* events.c - reacting to what the I/O backend reports for one connection.
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"

/*
 * The backend says "this socket is readable / writable / failed". What that means depends on
 * the connection's state: while connecting, "writable" means the connect finished; once open it
 * means room to send; while closing we only care about draining the send buffer.
 */
void dx_conn_on_event(dx_ctx* ctx, dx_conn* c, const dx_io_event* ev, int can_read)
{
    if (c->state == DX_C_FREE) return; /* closed earlier in this same batch of events */

    if (c->state == DX_C_CONNECTING) {
        if (!(ev->events & DX_IO_W) && !ev->error) return;
        if (dx_net_connect_result(c->fd) != DX_OK) {
            dx_conn_close_now(ctx, c, DX_ERR_IO);
            return;
        }
        c->state = DX_C_OPEN;
        dx_ctx_event(ctx, dx_conn_handle(ctx, c), DX_SYS_CONNECTED, 0);
        dx_conn_flush(ctx, c); /* send what was queued while connecting */
        return;
    }
    if (ev->events & DX_IO_W) {
        dx_conn_flush(ctx, c);
        if (c->state == DX_C_FREE) return;
    }
    if (c->state == DX_C_CLOSING) {
        if (ev->error) dx_conn_close_now(ctx, c, DX_ERR_IO); /* peer is gone: nothing to drain */
        return;
    }
    /* Reading pauses while the application has no room: back-pressure. */
    if ((ev->events & DX_IO_R) && can_read) dx_conn_read(ctx, c);
}
