/* io.h - readiness backends: "which sockets can be read or written now?" (internal).
 * SPDX-License-Identifier: MIT */
#ifndef DX_IO_H
#define DX_IO_H

#include <stdint.h>

#include "Duplex/dx_errors.h"
#include "Duplex/dx_types.h"

/*
 * In plain words: the operating system can watch thousands of sockets for us and say which
 * ones have data (readable) or room to send (writable). Every OS has its own way of doing so
 * (poll everywhere, epoll on Linux, kqueue on macOS/BSD...). This interface hides the
 * difference: the rest of the library only talks to dx_io_ops.
 */
#define DX_IO_R 1u
#define DX_IO_W 2u

typedef struct dx_io dx_io;

typedef struct dx_io_event {
    void* tag;      /* the pointer given at add time (identifies the connection) */
    uint8_t events; /* DX_IO_R / DX_IO_W bits that are ready */
    uint8_t error;  /* 1 if the peer hung up or the socket failed (a read will tell why) */
} dx_io_event;

typedef struct dx_io_ops {
    const char* name;
    dx_io* (*open)(const dx_alloc* a, uint32_t max_fds);
    void (*close)(dx_io* io, const dx_alloc* a);
    int (*add)(dx_io* io, int fd, void* tag, uint8_t events);
    int (*mod)(dx_io* io, int fd, void* tag, uint8_t events);
    int (*del)(dx_io* io, int fd);
    /* Waits up to timeout_ms (-1 = forever) and fills out[0..max); returns the count or < 0. */
    int (*wait)(dx_io* io, dx_io_event* out, int max, int timeout_ms);
} dx_io_ops;

/** Returns the backend for a DX_BACKEND_* value (AUTO = best for this platform), or NULL. */
const dx_io_ops* dx_io_get(uint32_t backend);

extern const dx_io_ops dx_io_poll;
#if defined(__linux__)
extern const dx_io_ops dx_io_epoll;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern const dx_io_ops dx_io_kqueue;
#endif

#endif
