/* io_kqueue.c - macOS / BSD backend built on kqueue.
 * SPDX-License-Identifier: MIT */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "io.h"

/*
 * kqueue is the BSD/macOS counterpart of epoll. Reading and writing are two separate
 * "filters". We always register both and simply enable or disable each one, which keeps
 * add/mod/del trivial.
 */
struct c2s2c_io {
    int fd;
};

static c2s2c_io* kq_open(const c2s2c_alloc* a, uint32_t max_fds)
{
    (void)max_fds;
    c2s2c_io* io = a->alloc(sizeof *io, a->user);
    if (!io) return NULL;
    io->fd = kqueue();
    if (io->fd < 0) {
        a->release(io, a->user);
        return NULL;
    }
    return io;
}

static void kq_close(c2s2c_io* io, const c2s2c_alloc* a)
{
    close(io->fd);
    a->release(io, a->user);
}

/* Sets both filters of fd: enabled if requested in `events`, disabled otherwise. */
static int kq_set(c2s2c_io* io, int fd, void* tag, uint8_t events)
{
    struct kevent ch[2];
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ,
           EV_ADD | ((events & C2S2C_IO_R) ? EV_ENABLE : EV_DISABLE), 0, 0, tag);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE,
           EV_ADD | ((events & C2S2C_IO_W) ? EV_ENABLE : EV_DISABLE), 0, 0, tag);
    return kevent(io->fd, ch, 2, NULL, 0, NULL) == 0 ? C2S2C_OK : C2S2C_ERR_IO;
}

static int kq_del(c2s2c_io* io, int fd)
{
    struct kevent ch[2];
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    (void)kevent(io->fd, ch, 2, NULL, 0, NULL); /* closing the fd would also drop them */
    return C2S2C_OK;
}

static int kq_wait(c2s2c_io* io, c2s2c_io_event* out, int max, int timeout_ms)
{
    struct kevent evs[128];
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
    int n = kevent(io->fd, NULL, 0, evs, max < 128 ? max : 128, timeout_ms < 0 ? NULL : &ts);
    if (n < 0) return 0; /* interrupted */

    /* kqueue reports read and write readiness as separate events: merge them per socket. */
    int got = 0;
    for (int i = 0; i < n; i++) {
        int k = got - 1;
        if (k >= 0 && out[k].tag == evs[i].udata) {
            out[k].events |= evs[i].filter == EVFILT_WRITE ? C2S2C_IO_W : C2S2C_IO_R;
        } else {
            out[got].tag = evs[i].udata;
            out[got].events = evs[i].filter == EVFILT_WRITE ? C2S2C_IO_W : C2S2C_IO_R;
            out[got].error = 0;
            k = got++;
        }
        if (evs[i].flags & (EV_EOF | EV_ERROR)) {
            out[k].error = 1;
            out[k].events |= C2S2C_IO_R; /* a read reveals EOF or the error */
        }
    }
    return got;
}

const c2s2c_io_ops c2s2c_io_kqueue = {"kqueue", kq_open, kq_close, kq_set, kq_set, kq_del, kq_wait};
#endif
