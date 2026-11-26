/* net.c - thin portable layer over BSD sockets.
 * SPDX-License-Identifier: MIT */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "C2S2C/c2s2c_errors.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SIGPIPE is disabled per socket with SO_NOSIGPIPE instead */
#endif

int c2s2c_net_init(void)
{
    return C2S2C_OK;
}

void c2s2c_net_shutdown(void) {}

/* Turns "127.0.0.1" or "::1" into a socket address. Names are not resolved on purpose: a DNS
 * lookup can block for seconds, which an event loop must never do. */
static int parse_addr(const char* text, uint16_t port, struct sockaddr_storage* ss, socklen_t* len)
{
    memset(ss, 0, sizeof *ss);
    struct sockaddr_in* v4 = (struct sockaddr_in*)ss;
    struct sockaddr_in6* v6 = (struct sockaddr_in6*)ss;
    if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        *len = sizeof *v4;
        return C2S2C_OK;
    }
    if (inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        *len = sizeof *v6;
        return C2S2C_OK;
    }
    return C2S2C_ERR_INVALID_ARG;
}

/* Common settings: non-blocking, close-on-exec, no Nagle delay, no SIGPIPE. */
static int tune(int fd, int is_stream)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return C2S2C_ERR_IO;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (is_stream) {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }
    return C2S2C_OK;
}

int c2s2c_net_listen(const char* addr, uint16_t port, uint16_t* bound_port)
{
    struct sockaddr_storage ss;
    socklen_t len;
    int rc = parse_addr(addr, port, &ss, &len);
    if (rc != C2S2C_OK) return rc;

    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return C2S2C_ERR_IO;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr*)&ss, len) < 0 || listen(fd, 1024) < 0 ||
        tune(fd, 0) != C2S2C_OK) {
        close(fd);
        return C2S2C_ERR_IO;
    }
    /* Ask the system which port it picked (needed when port 0 was requested). */
    struct sockaddr_storage bound;
    socklen_t blen = sizeof bound;
    if (getsockname(fd, (struct sockaddr*)&bound, &blen) < 0) {
        close(fd);
        return C2S2C_ERR_IO;
    }
    *bound_port = ntohs(bound.ss_family == AF_INET ? ((struct sockaddr_in*)&bound)->sin_port
                                                   : ((struct sockaddr_in6*)&bound)->sin6_port);
    return fd;
}

int c2s2c_net_connect(const char* addr, uint16_t port)
{
    struct sockaddr_storage ss;
    socklen_t len;
    int rc = parse_addr(addr, port, &ss, &len);
    if (rc != C2S2C_OK) return rc;

    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return C2S2C_ERR_IO;
    if (tune(fd, 1) != C2S2C_OK) {
        close(fd);
        return C2S2C_ERR_IO;
    }
    /* EINPROGRESS is the normal answer of a non-blocking connect: completion is reported later
     * as "writable", and c2s2c_net_connect_result then tells whether it worked. */
    if (connect(fd, (struct sockaddr*)&ss, len) < 0 && errno != EINPROGRESS) {
        close(fd);
        return C2S2C_ERR_IO;
    }
    return fd;
}

int c2s2c_net_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? C2S2C_ERR_WOULD_BLOCK : C2S2C_ERR_IO;
    if (tune(fd, 1) != C2S2C_OK) {
        close(fd);
        return C2S2C_ERR_IO;
    }
    return fd;
}

int c2s2c_net_connect_result(int fd)
{
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) return C2S2C_ERR_IO;
    return C2S2C_OK;
}

long c2s2c_net_read(int fd, uint8_t* buf, size_t n)
{
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r >= 0) return (long)r;
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? C2S2C_ERR_WOULD_BLOCK : C2S2C_ERR_IO;
    }
}

long c2s2c_net_write(int fd, const uint8_t* buf, size_t n)
{
    for (;;) {
        ssize_t r = send(fd, buf, n, MSG_NOSIGNAL);
        if (r >= 0) return (long)r;
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? C2S2C_ERR_WOULD_BLOCK : C2S2C_ERR_IO;
    }
}

void c2s2c_net_close(int fd)
{
    close(fd);
}

uint64_t c2s2c_net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

long c2s2c_net_raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) return -1;
    /* Ask for the hard limit; macOS reports "unlimited" but refuses it, so fall back to
     * progressively smaller values until the system accepts one. */
    rlim_t wanted[] = {rl.rlim_max, 1048576, 65536, 20000, 10240};
    for (size_t i = 0; i < sizeof wanted / sizeof wanted[0]; i++) {
        struct rlimit try = {wanted[i] > rl.rlim_max ? rl.rlim_max : wanted[i], rl.rlim_max};
        if (try.rlim_cur > rl.rlim_cur && setrlimit(RLIMIT_NOFILE, &try) == 0) break;
    }
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) return -1;
    return (long)rl.rlim_cur;
}
