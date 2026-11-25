/* duplex.h - public C API of Duplex (stable ABI).
 * SPDX-License-Identifier: MIT */
#ifndef DUPLEX_H
#define DUPLEX_H

/*
 * This is the only header users need. It lists every function of the library. Because the
 * functions use plain C types, any language that can call C (Python, Rust, Node, C++...) can
 * load the compiled library and use them: that is what "stable ABI" means in practice.
 * Convention: functions return 0 on success and a negative DX_ERR_* code on failure.
 */
#include <stddef.h>
#include <stdint.h>

#include "dx_errors.h"
#include "dx_export.h"
#include "dx_types.h"
#include "dx_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns a static, NUL-terminated description of @p err. Never NULL. Thread-safe. */
DX_API const char* dx_strerror(int err);

/** Returns DX_ABI_VERSION of the loaded library. Thread-safe. */
DX_API uint32_t dx_abi_version(void);

/**
 * Serializes @p h into @p out (DX_HEADER_SIZE bytes, little-endian). Validates the header.
 * @return DX_OK, DX_ERR_INVALID_ARG (NULL), or the DX_ERR_BAD_* / DX_ERR_TOO_LARGE of the
 *         first invalid field. The caller owns both buffers. Thread-safe.
 */
DX_API int dx_header_encode(const dx_header* h, uint8_t out[DX_HEADER_SIZE]);

/**
 * Parses and validates a header from @p in. Never reads more than @p len bytes.
 * @return DX_OK, DX_ERR_INVALID_ARG, DX_ERR_TRUNCATED (len < DX_HEADER_SIZE), or DX_ERR_BAD_*
 *         / DX_ERR_TOO_LARGE. @p out is written only on success. Thread-safe.
 */
DX_API int dx_header_decode(const uint8_t* in, size_t len, dx_header* out);

#ifdef __cplusplus
}
#endif

#endif
