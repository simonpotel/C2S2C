/* dx_simd.h - CPU-specific implementations behind the public checksum (internal).
 * SPDX-License-Identifier: MIT */
#ifndef DX_SIMD_H
#define DX_SIMD_H

#include <stddef.h>
#include <stdint.h>

/*
 * dx_crc32c() picks the fastest implementation the running CPU supports. The portable C one
 * always exists and is the reference: the hardware ones are tested against it.
 * Convention for all of them: they take and return the "finished" CRC (already inverted), so
 * chaining calls works as in the public function.
 */
typedef uint32_t (*dx_crc32c_fn)(uint32_t crc, const uint8_t* data, size_t len);

/** Portable table-driven implementation (always available). */
uint32_t dx_crc32c_generic(uint32_t crc, const uint8_t* data, size_t len);

/** Hardware implementation for this CPU, or NULL if the CPU (or build) has none. */
dx_crc32c_fn dx_crc32c_hardware(void);

/** 1 if the running CPU has CRC32C instructions. */
int dx_cpu_has_crc32c(void);

#endif
