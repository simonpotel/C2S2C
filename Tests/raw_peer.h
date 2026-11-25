/* raw_peer.h - a plain POSIX socket peer that writes exactly the bytes a test chooses.
 * SPDX-License-Identifier: MIT */
#ifndef DX_TEST_RAW_PEER_H
#define DX_TEST_RAW_PEER_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_test.h"

static inline void put16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put32(uint8_t* p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}

/* Writes a raw 14-byte header with any field values, valid or not. */
static inline void raw_header(uint8_t* out, unsigned magic, unsigned ver, unsigned flags,
                              unsigned id, uint32_t len, uint32_t seq)
{
    put16(out, magic);
    out[2] = (uint8_t)ver;
    out[3] = (uint8_t)flags;
    put16(out + 4, id);
    put32(out + 6, len);
    put32(out + 10, seq);
}

/* A valid CTOS ping frame (header + u32 nonce) with the given sequence number. */
static inline size_t ping_frame(uint8_t* out, uint32_t nonce, uint32_t seq)
{
    raw_header(out, DX_MAGIC, 1, 0, DX_PKT_PING, 4, seq);
    put32(out + 14, nonce);
    return 18;
}

static inline int raw_connect(dx_ctx* server)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)dx_listen_port(server));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) {
        perror("raw connect");
        return -1;
    }
    return fd;
}

#endif
