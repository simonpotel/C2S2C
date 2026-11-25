/* net.h - thin portable layer over BSD sockets (internal).
 * SPDX-License-Identifier: MIT */
#ifndef DX_NET_H
#define DX_NET_H

#include <stddef.h>
#include <stdint.h>

/*
 * Everything that touches the socket API lives in net.c, so the rest of the library never
 * sees errno or platform differences. All sockets are non-blocking. Functions return a value
 * >= 0 on success or a DX_ERR_* code (negative).
 */

/** Prepares the socket layer (Winsock on Windows, nothing elsewhere). Pair with dx_net_shutdown. */
int dx_net_init(void);
void dx_net_shutdown(void);

/** Creates a listening socket on a numeric address; returns its fd, sets *bound_port. */
int dx_net_listen(const char* addr, uint16_t port, uint16_t* bound_port);

/** Starts a non-blocking connect to a numeric address; returns the fd (still connecting). */
int dx_net_connect(const char* addr, uint16_t port);

/** Accepts one pending connection: its fd, or DX_ERR_WOULD_BLOCK when there is none. */
int dx_net_accept(int listen_fd);

/** Result of a finished connect: DX_OK or DX_ERR_IO. */
int dx_net_connect_result(int fd);

/** Reads up to n bytes: count > 0, 0 = peer closed, DX_ERR_WOULD_BLOCK, or DX_ERR_IO. */
long dx_net_read(int fd, uint8_t* buf, size_t n);

/** Writes up to n bytes: count >= 0, DX_ERR_WOULD_BLOCK, or DX_ERR_IO. */
long dx_net_write(int fd, const uint8_t* buf, size_t n);

void dx_net_close(int fd);

/** Monotonic clock in milliseconds (never goes backwards, unrelated to the wall clock). */
uint64_t dx_net_now_ms(void);

/** Raises the process limit of open files towards the hard limit; returns the new limit. */
long dx_net_raise_fd_limit(void);

#endif
