/* listener.c - accepting connections (c2s2c_listen) and starting them (c2s2c_connect).
 * SPDX-License-Identifier: MIT */
#include "c2s2c_internal.h"

#define ACCEPT_BURST                                                                               \
    64 /* accept at most this many per event, so one flood cannot starve the rest */

int c2s2c_listen(c2s2c_ctx* ctx, const char* addr, uint16_t port)
{
    if (!ctx || !addr) return C2S2C_ERR_INVALID_ARG;
    if (ctx->listen_fd >= 0) return C2S2C_ERR_LIMIT;
    uint16_t bound = 0;
    int fd = c2s2c_net_listen(addr, port, &bound);
    if (fd < 0) return fd;
    /* The context pointer itself tags the listener, to tell it from connection slots. */
    if (ctx->ops->add(ctx->io, fd, ctx, C2S2C_IO_R) != C2S2C_OK) {
        c2s2c_net_close(fd);
        return C2S2C_ERR_IO;
    }
    ctx->listen_fd = fd;
    ctx->listen_port = bound;
    return C2S2C_OK;
}

int c2s2c_listen_port(const c2s2c_ctx* ctx)
{
    return ctx && ctx->listen_fd >= 0 ? ctx->listen_port : C2S2C_ERR_INVALID_ARG;
}

int c2s2c_connect(c2s2c_ctx* ctx, const char* addr, uint16_t port, uint32_t* out_conn)
{
    if (!ctx || !addr || !out_conn) return C2S2C_ERR_INVALID_ARG;
    int fd = c2s2c_net_connect(addr, port);
    if (fd < 0) return fd;
    int rc = c2s2c_conn_adopt(ctx, fd, 0, 1, out_conn);
    if (rc != C2S2C_OK) c2s2c_net_close(fd);
    return rc;
}

void c2s2c_ctx_accept_all(c2s2c_ctx* ctx)
{
    for (int i = 0; i < ACCEPT_BURST; i++) {
        int fd = c2s2c_net_accept(ctx->listen_fd);
        if (fd < 0) return;
        uint32_t handle;
        if (c2s2c_conn_adopt(ctx, fd, 1, 0, &handle) != C2S2C_OK)
            c2s2c_net_close(fd); /* table full */
    }
}
