/* crc32_sse42.c - CRC-32C with the x86-64 SSE4.2 crc32 instruction.
 * SPDX-License-Identifier: MIT */
#include "dx_simd.h"

#if defined(__x86_64__)
#include <nmmintrin.h>
#include <string.h>

/*
 * The crc32 instruction folds 8 bytes into the checksum per step, instead of 1 byte per table
 * lookup. The function is compiled for SSE4.2 only here (target attribute), so the rest of the
 * library still runs on older CPUs; dx_crc32c() calls it only after checking the CPU.
 */
__attribute__((target("sse4.2"))) static uint32_t crc32c_sse42(uint32_t crc, const uint8_t* data,
                                                               size_t len)
{
    uint64_t c = (uint32_t)~crc;
    while (len >= 8) {
        uint64_t word;
        memcpy(&word, data, 8); /* memcpy: safe for unaligned data, compiles to one load */
        c = _mm_crc32_u64(c, word);
        data += 8;
        len -= 8;
    }
    uint32_t c32 = (uint32_t)c;
    while (len--)
        c32 = _mm_crc32_u8(c32, *data++);
    return ~c32;
}

dx_crc32c_fn dx_crc32c_hardware(void)
{
    return crc32c_sse42;
}
#endif
