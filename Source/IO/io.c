/* io.c - picks the I/O backend.
 * SPDX-License-Identifier: MIT */
#include "io.h"

#include <stddef.h>

const c2s2c_io_ops* c2s2c_io_get(uint32_t backend)
{
    switch (backend) {
    case C2S2C_BACKEND_AUTO:
#if defined(__linux__)
        return &c2s2c_io_epoll;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        return &c2s2c_io_kqueue;
#else
        return &c2s2c_io_poll;
#endif
    case C2S2C_BACKEND_POLL:
        return &c2s2c_io_poll;
#if defined(__linux__)
    case C2S2C_BACKEND_EPOLL:
        return &c2s2c_io_epoll;
    case C2S2C_BACKEND_URING:
        return &c2s2c_io_uring; /* may still fail at c2s2c_create if the kernel refuses io_uring */
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    case C2S2C_BACKEND_KQUEUE:
        return &c2s2c_io_kqueue;
#endif
    default:
        return NULL; /* not available on this platform */
    }
}
