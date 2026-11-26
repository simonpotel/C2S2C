/* net_win32.c - the socket layer on Windows (Winsock). Same interface as net.c.
 * SPDX-License-Identifier: MIT */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string.h>

#include "Duplex/dx_errors.h"
#include "net.h"

/*
 * Windows sockets are almost the BSD ones with different names and error
 * reporting. This file mirrors net.c so the rest of the library never notices the difference.
 * Winsock handles are kernel handles whose values fit in 32 bits in practice, so they travel as
 * `int` like POSIX file descriptors do.
 * Windows has no "SO_REUSEADDR" for listeners worth using (it lets another process steal the
 * port), so it is not set.
 */
static unsigned started;

int dx_net_init(void)
{
    /* WSAStartup keeps its own use counter; every successful call needs one WSACleanup. */
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return DX_ERR_IO;
    started++;
    return DX_OK;
}

void dx_net_shutdown(void)
{
    if (started) {
        started--;
        WSACleanup();
    }
}

static int parse_addr(const char* text, uint16_t port, struct sockaddr_storage* ss, int* len)
{
    memset(ss, 0, sizeof *ss);
    struct sockaddr_in* v4 = (struct sockaddr_in*)ss;
    struct sockaddr_in6* v6 = (struct sockaddr_in6*)ss;
    if (InetPtonA(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        *len = (int)sizeof *v4;
        return DX_OK;
    }
    if (InetPtonA(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        *len = (int)sizeof *v6;
        return DX_OK;
    }
    return DX_ERR_INVALID_ARG;
}

/* Non-blocking, no Nagle delay. */
static int tune(SOCKET s, int is_stream)
{
    u_long nonblocking = 1;
    if (ioctlsocket(s, (long)FIONBIO,
                    &nonblocking) /* the macro is unsigned, the parameter signed */
        != 0)
        return DX_ERR_IO;
    if (is_stream) {
        int one = 1;
        (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, (int)sizeof one);
    }
    return DX_OK;
}

int dx_net_listen(const char* addr, uint16_t port, uint16_t* bound_port)
{
    struct sockaddr_storage ss;
    int len;
    int rc = parse_addr(addr, port, &ss, &len);
    if (rc != DX_OK) return rc;

    SOCKET s = socket(ss.ss_family, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return DX_ERR_IO;
    if (bind(s, (struct sockaddr*)&ss, len) != 0 || listen(s, SOMAXCONN) != 0 ||
        tune(s, 0) != DX_OK) {
        closesocket(s);
        return DX_ERR_IO;
    }
    struct sockaddr_storage bound;
    int blen = (int)sizeof bound;
    if (getsockname(s, (struct sockaddr*)&bound, &blen) != 0) {
        closesocket(s);
        return DX_ERR_IO;
    }
    *bound_port = ntohs(bound.ss_family == AF_INET ? ((struct sockaddr_in*)&bound)->sin_port
                                                   : ((struct sockaddr_in6*)&bound)->sin6_port);
    return (int)s;
}

int dx_net_connect(const char* addr, uint16_t port)
{
    struct sockaddr_storage ss;
    int len;
    int rc = parse_addr(addr, port, &ss, &len);
    if (rc != DX_OK) return rc;

    SOCKET s = socket(ss.ss_family, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return DX_ERR_IO;
    if (tune(s, 1) != DX_OK) {
        closesocket(s);
        return DX_ERR_IO;
    }
    /* WSAEWOULDBLOCK is Windows' "connection in progress". */
    if (connect(s, (struct sockaddr*)&ss, len) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return DX_ERR_IO;
    }
    return (int)s;
}

int dx_net_accept(int listen_fd)
{
    SOCKET s = accept((SOCKET)listen_fd, NULL, NULL);
    if (s == INVALID_SOCKET)
        return WSAGetLastError() == WSAEWOULDBLOCK ? DX_ERR_WOULD_BLOCK : DX_ERR_IO;
    if (tune(s, 1) != DX_OK) {
        closesocket(s);
        return DX_ERR_IO;
    }
    return (int)s;
}

int dx_net_connect_result(int fd)
{
    int err = 0;
    int len = (int)sizeof err;
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err != 0)
        return DX_ERR_IO;
    return DX_OK;
}

long dx_net_read(int fd, uint8_t* buf, size_t n)
{
    int r = recv((SOCKET)fd, (char*)buf, (int)n, 0);
    if (r >= 0) return r;
    return WSAGetLastError() == WSAEWOULDBLOCK ? DX_ERR_WOULD_BLOCK : DX_ERR_IO;
}

long dx_net_write(int fd, const uint8_t* buf, size_t n)
{
    int r = send((SOCKET)fd, (const char*)buf, (int)n, 0);
    if (r >= 0) return r;
    return WSAGetLastError() == WSAEWOULDBLOCK ? DX_ERR_WOULD_BLOCK : DX_ERR_IO;
}

void dx_net_close(int fd)
{
    closesocket((SOCKET)fd);
}

uint64_t dx_net_now_ms(void)
{
    return (uint64_t)GetTickCount64();
}

long dx_net_raise_fd_limit(void)
{
    return -1; /* Windows has no per-process descriptor limit of this kind */
}
#endif
