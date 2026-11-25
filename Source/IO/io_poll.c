/* io_poll.c - portable backend built on poll(2).
 * SPDX-License-Identifier: MIT */
#include <poll.h>
#include <string.h>

#include "io.h"

/*
 * poll(2) exists on every Unix and is the simplest backend. It scans the whole list at each
 * call, so it is slower than epoll/kqueue with thousands of sockets; it is the fallback and
 * the reference the other backends are tested against.
 */
struct dx_io {
    struct pollfd* fds;
    void** tags;
    uint32_t count, cap;
    uint32_t
        next; /* where the next scan starts, so busy sockets at the front cannot starve others */
};

static dx_io* poll_open(const dx_alloc* a, uint32_t max_fds)
{
    dx_io* io = a->alloc(sizeof *io, a->user);
    if (!io) return NULL;
    io->fds = a->alloc(sizeof(struct pollfd) * max_fds, a->user);
    io->tags = a->alloc(sizeof(void*) * max_fds, a->user);
    if (!io->fds || !io->tags) {
        if (io->fds) a->release(io->fds, a->user);
        if (io->tags) a->release(io->tags, a->user);
        a->release(io, a->user);
        return NULL;
    }
    io->count = io->next = 0;
    io->cap = max_fds;
    return io;
}

static void poll_close(dx_io* io, const dx_alloc* a)
{
    a->release(io->fds, a->user);
    a->release(io->tags, a->user);
    a->release(io, a->user);
}

static short to_poll(uint8_t events)
{
    return (short)(((events & DX_IO_R) ? POLLIN : 0) | ((events & DX_IO_W) ? POLLOUT : 0));
}

static int find(const dx_io* io, int fd)
{
    for (uint32_t i = 0; i < io->count; i++)
        if (io->fds[i].fd == fd) return (int)i;
    return -1;
}

static int poll_add(dx_io* io, int fd, void* tag, uint8_t events)
{
    if (io->count == io->cap) return DX_ERR_LIMIT;
    io->fds[io->count] = (struct pollfd){fd, to_poll(events), 0};
    io->tags[io->count++] = tag;
    return DX_OK;
}

static int poll_mod(dx_io* io, int fd, void* tag, uint8_t events)
{
    int i = find(io, fd);
    if (i < 0) return DX_ERR_INVALID_ARG;
    io->fds[i].events = to_poll(events);
    io->tags[i] = tag;
    return DX_OK;
}

static int poll_del(dx_io* io, int fd)
{
    int i = find(io, fd);
    if (i < 0) return DX_ERR_INVALID_ARG;
    io->count--; /* move the last entry into the hole */
    io->fds[i] = io->fds[io->count];
    io->tags[i] = io->tags[io->count];
    return DX_OK;
}

static int poll_wait(dx_io* io, dx_io_event* out, int max, int timeout_ms)
{
    int n = poll(io->fds, io->count, timeout_ms);
    if (n < 0) return 0; /* interrupted: report "nothing" and let the caller loop */
    int got = 0;
    for (uint32_t k = 0; k < io->count && got < max; k++) {
        uint32_t i = (io->next + k) % io->count;
        short re = io->fds[i].revents;
        if (!re) continue;
        out[got].tag = io->tags[i];
        out[got].events = (uint8_t)(((re & POLLIN) ? DX_IO_R : 0) | ((re & POLLOUT) ? DX_IO_W : 0));
        out[got].error = (re & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (out[got].error) out[got].events |= DX_IO_R; /* a read reveals EOF or the error */
        got++;
    }
    if (io->count) io->next = (io->next + 1) % io->count;
    return got;
}

const dx_io_ops dx_io_poll = {"poll",   poll_open, poll_close, poll_add,
                              poll_mod, poll_del,  poll_wait};
