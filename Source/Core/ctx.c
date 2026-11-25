/* ctx.c - version and error-string accessors.
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
