/* version.c - version and error-string accessors.
 * SPDX-License-Identifier: MIT */
#include "C2S2C/c2s2c.h"

/*
 * Bindings in other languages cannot see C macros at runtime, so errors are exposed as
 * static strings through a function. Returning string literals means no allocation and no
 * ownership question: the pointer is valid forever and safe to share between threads.
 * Adding a C2S2C_ERR_* code requires adding its message here (the unit test enforces it).
 */
const char* c2s2c_strerror(int err)
{
    switch (err) {
    case C2S2C_OK:
        return "success";
    case C2S2C_ERR_INVALID_ARG:
        return "invalid argument";
    case C2S2C_ERR_TRUNCATED:
        return "truncated input";
    case C2S2C_ERR_BAD_MAGIC:
        return "bad magic";
    case C2S2C_ERR_BAD_VERSION:
        return "unsupported protocol version";
    case C2S2C_ERR_BAD_FLAGS:
        return "reserved flag bits set";
    case C2S2C_ERR_BAD_ID:
        return "packet id does not match direction";
    case C2S2C_ERR_TOO_LARGE:
        return "payload too large";
    case C2S2C_ERR_BAD_UTF8:
        return "invalid UTF-8";
    case C2S2C_ERR_BAD_VALUE:
        return "invalid value for type";
    case C2S2C_ERR_TRAILING:
        return "unexpected trailing bytes";
    case C2S2C_ERR_BAD_SCHEMA:
        return "invalid schema";
    case C2S2C_ERR_OVERFLOW:
        return "output buffer too small";
    case C2S2C_ERR_UNKNOWN_ID:
        return "unknown packet id";
    case C2S2C_ERR_WOULD_BLOCK:
        return "send buffer full";
    case C2S2C_ERR_IO:
        return "socket error";
    case C2S2C_ERR_BAD_SEQUENCE:
        return "unexpected sequence number";
    case C2S2C_ERR_BAD_DIRECTION:
        return "packet travels the wrong direction";
    case C2S2C_ERR_NO_MEMORY:
        return "out of memory";
    case C2S2C_ERR_LIMIT:
        return "limit reached";
    case C2S2C_ERR_BAD_CONN:
        return "invalid connection";
    case C2S2C_ERR_TIMEOUT:
        return "connection timed out";
    case C2S2C_ERR_REJECTED:
        return "rejected by a hook";
    default:
        return "unknown error"; /* never NULL, so callers can print it without checking */
    }
}

/*
 * Lets a binding compare the library it loaded at runtime against the C2S2C_ABI_VERSION it
 * was compiled against, and refuse to run on a mismatch instead of crashing later.
 */
uint32_t c2s2c_abi_version(void)
{
    return C2S2C_ABI_VERSION;
}
