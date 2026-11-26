/* test_header.c - unit tests: header codec, error strings, ABI layout.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../check.h"
#include "C2S2C/c2s2c.h"

/* Compile-time freeze of the public struct layout: if someone reorders or retypes a field,
 * the build breaks here instead of silently breaking every binding. */
_Static_assert(sizeof(c2s2c_header) == 16, "c2s2c_header layout is part of the ABI");
_Static_assert(offsetof(c2s2c_header, packet_id) == 4, "c2s2c_header.packet_id offset");
_Static_assert(offsetof(c2s2c_header, length) == 8, "c2s2c_header.length offset");
_Static_assert(offsetof(c2s2c_header, sequence) == 12, "c2s2c_header.sequence offset");

/* A valid CTOS header; each test copies it and breaks exactly one field. */
static c2s2c_header good(void)
{
    c2s2c_header h = {C2S2C_MAGIC, C2S2C_PROTOCOL_VERSION, 0, 0x0001, 5, 7};
    return h;
}

/* Encode must produce the exact documented bytes, and decode must invert it. */
static void test_roundtrip_and_layout(void)
{
    uint8_t b[C2S2C_HEADER_SIZE];
    c2s2c_header h = good(), o;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_OK);
    const uint8_t want[C2S2C_HEADER_SIZE] = {0xDE, 0xC0, 1, 0, 1, 0, 5, 0, 0, 0, 7, 0, 0, 0};
    CHECK(memcmp(b, want, sizeof b) == 0);
    CHECK(c2s2c_header_decode(b, sizeof b, &o) == C2S2C_OK);
    /* Field by field: comparing the raw struct would also compare its padding bytes. */
    CHECK(o.magic == h.magic && o.version == h.version && o.flags == h.flags &&
          o.packet_id == h.packet_id && o.length == h.length && o.sequence == h.sequence);
}

/* Length limits (0, max, max+1) and every shorter-than-header input. */
static void test_bounds(void)
{
    uint8_t b[C2S2C_HEADER_SIZE];
    c2s2c_header h = good(), o;
    h.length = C2S2C_MAX_PAYLOAD; /* max ok, max+1 rejected */
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_OK);
    h.length = C2S2C_MAX_PAYLOAD + 1;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_ERR_TOO_LARGE);
    h = good();
    h.length = 0;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_OK);
    for (size_t n = 0; n < C2S2C_HEADER_SIZE; n++)
        CHECK(c2s2c_header_decode(b, n, &o) == C2S2C_ERR_TRUNCATED);
}

/* Each error code is triggered by exactly one broken field, plus NULL arguments. */
static void test_errors(void)
{
    uint8_t b[C2S2C_HEADER_SIZE];
    c2s2c_header h = good(), o;
    CHECK(c2s2c_header_encode(NULL, b) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_header_encode(&h, NULL) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_header_decode(NULL, 14, &o) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_header_decode(b, 14, NULL) == C2S2C_ERR_INVALID_ARG);

    h = good();
    h.magic = 0xBEEF;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_ERR_BAD_MAGIC);
    h = good();
    h.version = 2;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_ERR_BAD_VERSION);
    h = good();
    h.flags = 0x04;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_ERR_BAD_FLAGS);
    h = good();
    h.flags = C2S2C_FLAG_STOC; /* STOC flag without STOC id bit */
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_ERR_BAD_ID);
    h = good();
    h.packet_id = 0x8001; /* STOC id bit without STOC flag */
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_ERR_BAD_ID);
    h = good();
    h.flags = C2S2C_FLAG_STOC | C2S2C_FLAG_COMPRESSED;
    h.packet_id = 0x8001;
    CHECK(c2s2c_header_encode(&h, b) == C2S2C_OK);

    /* decode must not touch @out on failure */
    memset(&o, 0xAA, sizeof o);
    b[0] = 0;
    CHECK(c2s2c_header_decode(b, sizeof b, &o) == C2S2C_ERR_BAD_MAGIC && o.magic == 0xAAAA);
}

/* Version accessor, and every error code has a real message (catches a forgotten case). */
static void test_misc(void)
{
    CHECK(c2s2c_abi_version() == C2S2C_ABI_VERSION);
    for (int e = 0; e >= C2S2C_ERR_REJECTED; e--)
        CHECK(strcmp(c2s2c_strerror(e), "unknown error") != 0);
    CHECK(strcmp(c2s2c_strerror(-999), "unknown error") == 0);
}

int main(void)
{
    test_roundtrip_and_layout();
    test_bounds();
    test_errors();
    test_misc();
    return c2s2c_test_done();
}
