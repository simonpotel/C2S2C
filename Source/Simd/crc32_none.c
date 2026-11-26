/* crc32_none.c - no hardware CRC-32C on this architecture: only the portable version is used.
 * SPDX-License-Identifier: MIT */
#include "c2s2c_simd.h"

#if !defined(__x86_64__) && !defined(__aarch64__)
c2s2c_crc32c_fn c2s2c_crc32c_hardware(void)
{
    return NULL;
}
#endif
