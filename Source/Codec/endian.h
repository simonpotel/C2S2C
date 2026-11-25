/* endian.h - the only place where wire integers are read or written (little-endian).
 * SPDX-License-Identifier: MIT */
#ifndef DX_ENDIAN_H
#define DX_ENDIAN_H

#include <stdint.h>

/*
 * Why byte-by-byte shifts instead of casting the buffer to a wider integer pointer:
 *  - a cast breaks on big-endian CPUs (the wire is always little-endian);
 *  - a cast on a misaligned address is undefined behaviour in C (UBSan flags it);
 *  - compilers turn these shifts into a single load on little-endian CPUs anyway,
 *    so portability costs nothing.
 * Keeping every access in this file means the rule is enforced in one place.
 */

/* Reads 2 bytes at p as a little-endian integer. Caller guarantees p[0..1] is readable. */
static inline uint16_t dx_rd16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Reads 4 bytes at p as a little-endian integer. Caller guarantees p[0..3] is readable. */
static inline uint32_t dx_rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Writes v as 2 little-endian bytes. Caller guarantees p[0..1] is writable. */
static inline void dx_wr16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Writes v as 4 little-endian bytes. Caller guarantees p[0..3] is writable. */
static inline void dx_wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#endif
