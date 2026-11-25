/* crc32_armv8.c - CRC-32C with the ARMv8 CRC32C instructions.
 * SPDX-License-Identifier: MIT */
#include "dx_simd.h"

#if defined(__aarch64__)
#include <arm_acle.h>
#include <string.h>

/*
 * Same idea as the x86 version: the CPU folds 8 bytes per instruction. The function is compiled
 * with the "crc" extension enabled, and is used only after the CPU was checked.
 */
__attribute__((target("+crc"))) static uint32_t crc32c_armv8(uint32_t crc, const uint8_t* data,
                                                             size_t len)
{
    uint32_t c = ~crc;
    while (len >= 8) {
        uint64_t word;
        memcpy(&word, data, 8);
        c = __crc32cd(c, word);
        data += 8;
        len -= 8;
    }
    while (len--)
        c = __crc32cb(c, *data++);
    return ~c;
}

dx_crc32c_fn dx_crc32c_hardware(void)
{
    return crc32c_armv8;
}
#endif
