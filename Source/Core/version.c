/* version.c - version and error-string accessors.
 * SPDX-License-Identifier: MIT */
#include "Duplex/duplex.h"

/*
 * Bindings in other languages cannot see C macros at runtime, so errors are exposed as
 * static strings through a function. Returning string literals means no allocation and no
 * ownership question: the pointer is valid forever and safe to share between threads.
 * Adding a DX_ERR_* code requires adding its message here (the unit test enforces it).
 */
const char* dx_strerror(int err)
{
    switch (err) {
    case DX_OK:
        return "success";
    case DX_ERR_INVALID_ARG:
        return "invalid argument";
    case DX_ERR_TRUNCATED:
        return "truncated input";
    case DX_ERR_BAD_MAGIC:
        return "bad magic";
    case DX_ERR_BAD_VERSION:
        return "unsupported protocol version";
    case DX_ERR_BAD_FLAGS:
        return "reserved flag bits set";
    case DX_ERR_BAD_ID:
        return "packet id does not match direction";
    case DX_ERR_TOO_LARGE:
        return "payload too large";
    case DX_ERR_BAD_UTF8:
        return "invalid UTF-8";
    case DX_ERR_BAD_VALUE:
        return "invalid value for type";
    case DX_ERR_TRAILING:
        return "unexpected trailing bytes";
    case DX_ERR_BAD_SCHEMA:
        return "invalid schema";
    case DX_ERR_OVERFLOW:
        return "output buffer too small";
    case DX_ERR_UNKNOWN_ID:
        return "unknown packet id";
    case DX_ERR_WOULD_BLOCK:
        return "send buffer full";
    case DX_ERR_IO:
        return "socket error";
    case DX_ERR_BAD_SEQUENCE:
        return "unexpected sequence number";
    case DX_ERR_BAD_DIRECTION:
        return "packet travels the wrong direction";
    case DX_ERR_NO_MEMORY:
        return "out of memory";
    case DX_ERR_LIMIT:
        return "limit reached";
    case DX_ERR_BAD_CONN:
        return "invalid connection";
    case DX_ERR_TIMEOUT:
        return "connection timed out";
    case DX_ERR_REJECTED:
        return "rejected by a hook";
    default:
        return "unknown error"; /* never NULL, so callers can print it without checking */
    }
}

/*
 * Lets a binding compare the library it loaded at runtime against the DX_ABI_VERSION it
 * was compiled against, and refuse to run on a mismatch instead of crashing later.
 */
uint32_t dx_abi_version(void)
{
    return DX_ABI_VERSION;
}
