/* listener.c - accepting connections (dx_listen) and starting them (dx_connect).
 * SPDX-License-Identifier: MIT */
#include "dx_internal.h"

#define ACCEPT_BURST                                                                               \
    64 /* accept at most this many per event, so one flood cannot starve the rest */

int dx_listen(dx_ctx* ctx, const char* addr, uint16_t port)
{
    if (!ctx || !addr) return DX_ERR_INVALID_ARG;
    if (ctx->listen_fd >= 0) return DX_ERR_LIMIT;
    uint16_t bound = 0;
    int fd = dx_net_listen(addr, port, &bound);
    if (fd < 0) return fd;
    /* The context pointer itself tags the listener, to tell it from connection slots. */
    if (ctx->ops->add(ctx->io, fd, ctx, DX_IO_R) != DX_OK) {
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

void dx_ctx_accept_all(dx_ctx* ctx)
{
    for (int i = 0; i < ACCEPT_BURST; i++) {
        int fd = dx_net_accept(ctx->listen_fd);
        if (fd < 0) return;
        uint32_t handle;
        if (dx_conn_adopt(ctx, fd, 1, 0, &handle) != DX_OK) dx_net_close(fd); /* table full */
    }
}
