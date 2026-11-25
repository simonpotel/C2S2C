/* crc32_none.c - no hardware CRC-32C on this architecture: only the portable version is used.
 * SPDX-License-Identifier: MIT */
#include "dx_simd.h"

#if !defined(__x86_64__) && !defined(__aarch64__)
dx_crc32c_fn dx_crc32c_hardware(void)
{
    return NULL;
}
#endif
