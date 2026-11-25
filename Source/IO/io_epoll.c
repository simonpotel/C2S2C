/* io_epoll.c - Linux backend built on epoll.
 * SPDX-License-Identifier: MIT */
#if defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>

#include "io.h"

/*
 * epoll keeps the watched list inside the kernel, so a wait costs time proportional to the
 * sockets that are READY, not to all of them. That is what makes thousands of idle
 * connections cheap. We use level-triggered mode: simpler and safe (an event repeats until
 * the data is consumed).
 */
struct dx_io {
    int fd;
};

static dx_io* epoll_open_(const dx_alloc* a, uint32_t max_fds)
{
    (void)max_fds;
    dx_io* io = a->alloc(sizeof *io, a->user);
    if (!io) return NULL;
    io->fd = epoll_create1(EPOLL_CLOEXEC);
    if (io->fd < 0) {
        a->release(io, a->user);
        return NULL;
    }
    return io;
}

static void epoll_close_(dx_io* io, const dx_alloc* a)
{
    close(io->fd);
    a->release(io, a->user);
}

static int ctl(dx_io* io, int op, int fd, void* tag, uint8_t events)
{
    struct epoll_event ev = {0};
    ev.events = ((events & DX_IO_R) ? EPOLLIN : 0u) | ((events & DX_IO_W) ? EPOLLOUT : 0u);
    ev.data.ptr = tag;
    return epoll_ctl(io->fd, op, fd, &ev) == 0 ? DX_OK : DX_ERR_IO;
}

static int epoll_add_(dx_io* io, int fd, void* tag, uint8_t events)
{
    return ctl(io, EPOLL_CTL_ADD, fd, tag, events);
}

static int epoll_mod_(dx_io* io, int fd, void* tag, uint8_t events)
{
    return ctl(io, EPOLL_CTL_MOD, fd, tag, events);
}

static int epoll_del_(dx_io* io, int fd)
{
    return epoll_ctl(io->fd, EPOLL_CTL_DEL, fd, NULL) == 0 ? DX_OK : DX_ERR_IO;
}

static int epoll_wait_(dx_io* io, dx_io_event* out, int max, int timeout_ms)
{
    struct epoll_event evs[128];
    int n = epoll_wait(io->fd, evs, max < 128 ? max : 128, timeout_ms);
    if (n < 0) return 0; /* interrupted */
    for (int i = 0; i < n; i++) {
        uint32_t e = evs[i].events;
        out[i].tag = evs[i].data.ptr;
        out[i].events = (uint8_t)(((e & EPOLLIN) ? DX_IO_R : 0) | ((e & EPOLLOUT) ? DX_IO_W : 0));
        out[i].error = (e & (EPOLLERR | EPOLLHUP)) != 0;
        if (out[i].error) out[i].events |= DX_IO_R; /* a read reveals EOF or the error */
    }
    return n;
}

const dx_io_ops dx_io_epoll = {"epoll",    epoll_open_, epoll_close_, epoll_add_,
                               epoll_mod_, epoll_del_,  epoll_wait_};
#endif
