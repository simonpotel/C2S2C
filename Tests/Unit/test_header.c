/* test_header.c - unit tests: header codec, error strings, ABI layout.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../check.h"
#include "Duplex/duplex.h"

/* Compile-time freeze of the public struct layout: if someone reorders or retypes a field,
 * the build breaks here instead of silently breaking every binding. */
_Static_assert(sizeof(dx_header) == 16, "dx_header layout is part of the ABI");
_Static_assert(offsetof(dx_header, packet_id) == 4, "dx_header.packet_id offset");
_Static_assert(offsetof(dx_header, length) == 8, "dx_header.length offset");
_Static_assert(offsetof(dx_header, sequence) == 12, "dx_header.sequence offset");

/* A valid CTOS header; each test copies it and breaks exactly one field. */
static dx_header good(void)
{
    dx_header h = {DX_MAGIC, DX_PROTOCOL_VERSION, 0, 0x0001, 5, 7};
    return h;
}

/* Encode must produce the exact documented bytes, and decode must invert it. */
static void test_roundtrip_and_layout(void)
{
    uint8_t b[DX_HEADER_SIZE];
    dx_header h = good(), o;
    CHECK(dx_header_encode(&h, b) == DX_OK);
    const uint8_t want[DX_HEADER_SIZE] = {0xDE, 0xC0, 1, 0, 1, 0, 5, 0, 0, 0, 7, 0, 0, 0};
    CHECK(memcmp(b, want, sizeof b) == 0);
    CHECK(dx_header_decode(b, sizeof b, &o) == DX_OK && memcmp(&o, &h, sizeof h) == 0);
}

/* Length limits (0, max, max+1) and every shorter-than-header input. */
static void test_bounds(void)
{
    uint8_t b[DX_HEADER_SIZE];
    dx_header h = good(), o;
    h.length = DX_MAX_PAYLOAD; /* max ok, max+1 rejected */
    CHECK(dx_header_encode(&h, b) == DX_OK);
    h.length = DX_MAX_PAYLOAD + 1;
    CHECK(dx_header_encode(&h, b) == DX_ERR_TOO_LARGE);
    h = good();
    h.length = 0;
    CHECK(dx_header_encode(&h, b) == DX_OK);
    for (size_t n = 0; n < DX_HEADER_SIZE; n++)
        CHECK(dx_header_decode(b, n, &o) == DX_ERR_TRUNCATED);
}

/* Each error code is triggered by exactly one broken field, plus NULL arguments. */
static void test_errors(void)
{
    uint8_t b[DX_HEADER_SIZE];
    dx_header h = good(), o;
    CHECK(dx_header_encode(NULL, b) == DX_ERR_INVALID_ARG);
    CHECK(dx_header_encode(&h, NULL) == DX_ERR_INVALID_ARG);
    CHECK(dx_header_decode(NULL, 14, &o) == DX_ERR_INVALID_ARG);
    CHECK(dx_header_decode(b, 14, NULL) == DX_ERR_INVALID_ARG);

    h = good();
    h.magic = 0xBEEF;
    CHECK(dx_header_encode(&h, b) == DX_ERR_BAD_MAGIC);
    h = good();
    h.version = 2;
    CHECK(dx_header_encode(&h, b) == DX_ERR_BAD_VERSION);
    h = good();
    h.flags = 0x04;
    CHECK(dx_header_encode(&h, b) == DX_ERR_BAD_FLAGS);
    h = good();
    h.flags = DX_FLAG_STOC; /* STOC flag without STOC id bit */
    CHECK(dx_header_encode(&h, b) == DX_ERR_BAD_ID);
    h = good();
    h.packet_id = 0x8001; /* STOC id bit without STOC flag */
    CHECK(dx_header_encode(&h, b) == DX_ERR_BAD_ID);
    h = good();
    h.flags = DX_FLAG_STOC | DX_FLAG_COMPRESSED;
    h.packet_id = 0x8001;
    CHECK(dx_header_encode(&h, b) == DX_OK);

    /* decode must not touch @out on failure */
    memset(&o, 0xAA, sizeof o);
    b[0] = 0;
    CHECK(dx_header_decode(b, sizeof b, &o) == DX_ERR_BAD_MAGIC && o.magic == 0xAAAA);
}

/* Version accessor, and every error code has a real message (catches a forgotten case). */
static void test_misc(void)
{
    CHECK(dx_abi_version() == DX_ABI_VERSION);
    for (int e = 0; e >= DX_ERR_UNKNOWN_ID; e--)
        CHECK(strcmp(dx_strerror(e), "unknown error") != 0);
    CHECK(strcmp(dx_strerror(-999), "unknown error") == 0);
}

int main(void)
{
    test_roundtrip_and_layout();
    test_bounds();
    test_errors();
    test_misc();
    return dx_test_done();
}
