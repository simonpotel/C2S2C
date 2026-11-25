/* io.c - picks the I/O backend.
 * SPDX-License-Identifier: MIT */
#include "io.h"

#include <stddef.h>

const dx_io_ops* dx_io_get(uint32_t backend)
{
    switch (backend) {
    case DX_BACKEND_AUTO:
#if defined(__linux__)
        return &dx_io_epoll;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        return &dx_io_kqueue;
#else
        return &dx_io_poll;
#endif
    case DX_BACKEND_POLL:
        return &dx_io_poll;
#if defined(__linux__)
    case DX_BACKEND_EPOLL:
        return &dx_io_epoll;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    case DX_BACKEND_KQUEUE:
        return &dx_io_kqueue;
#endif
    default:
        return NULL; /* not available on this platform */
    }
}
