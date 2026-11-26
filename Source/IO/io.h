/* io.h - readiness backends: "which sockets can be read or written now?" (internal).
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_IO_H
#define C2S2C_IO_H

#include <stdint.h>

#include "C2S2C/c2s2c_errors.h"
#include "C2S2C/c2s2c_types.h"

/*
 * The operating system can watch thousands of sockets for us and say which
 * ones have data (readable) or room to send (writable). Every OS has its own way of doing so
 * (poll everywhere, epoll on Linux, kqueue on macOS/BSD...). This interface hides the
 * difference: the rest of the library only talks to c2s2c_io_ops.
 */
#define C2S2C_IO_R 1u
#define C2S2C_IO_W 2u

typedef struct c2s2c_io c2s2c_io;

typedef struct c2s2c_io_event {
    void* tag;      /* the pointer given at add time (identifies the connection) */
    uint8_t events; /* C2S2C_IO_R / C2S2C_IO_W bits that are ready */
    uint8_t error;  /* 1 if the peer hung up or the socket failed (a read will tell why) */
} c2s2c_io_event;

typedef struct c2s2c_io_ops {
    const char* name;
    c2s2c_io* (*open)(const c2s2c_alloc* a, uint32_t max_fds);
    void (*close)(c2s2c_io* io, const c2s2c_alloc* a);
    int (*add)(c2s2c_io* io, int fd, void* tag, uint8_t events);
    int (*mod)(c2s2c_io* io, int fd, void* tag, uint8_t events);
    int (*del)(c2s2c_io* io, int fd);
    /* Waits up to timeout_ms (-1 = forever) and fills out[0..max); returns the count or < 0. */
    int (*wait)(c2s2c_io* io, c2s2c_io_event* out, int max, int timeout_ms);
} c2s2c_io_ops;

/** Returns the backend for a C2S2C_BACKEND_* value (AUTO = best for this platform), or NULL. */
const c2s2c_io_ops* c2s2c_io_get(uint32_t backend);

extern const c2s2c_io_ops c2s2c_io_poll;
#if defined(__linux__)
extern const c2s2c_io_ops c2s2c_io_epoll;
extern const c2s2c_io_ops c2s2c_io_uring;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern const c2s2c_io_ops c2s2c_io_kqueue;
#endif

#endif
