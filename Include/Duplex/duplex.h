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

/* Payload writing.
 * Pattern: init, call dx_w_* for each field, then check w.err once and use w.len as the size.
 * After the first error every later call does nothing, so there is no need to test each call.
 * Signed integers and bool: write with the unsigned function of the same size.
 * Memory: the writer never allocates; the caller owns the buffer. Thread-safe per writer. */

/** Starts writing into @p buf (capacity @p cap). */
DX_API void dx_writer_init(dx_writer* w, uint8_t* buf, uint32_t cap);
DX_API void dx_w_u8(dx_writer* w, uint8_t v);
DX_API void dx_w_u16(dx_writer* w, uint16_t v);
DX_API void dx_w_u32(dx_writer* w, uint32_t v);
DX_API void dx_w_u64(dx_writer* w, uint64_t v);
DX_API void dx_w_f32(dx_writer* w, float v);
DX_API void dx_w_f64(dx_writer* w, double v);
/** Writes a string of @p n bytes (no NUL needed). Errors: DX_ERR_BAD_UTF8, DX_ERR_OVERFLOW. */
DX_API void dx_w_string(dx_writer* w, const char* s, uint16_t n);
/** Writes @p n raw bytes with their u32 length prefix. Error: DX_ERR_OVERFLOW. */
DX_API void dx_w_bytes(dx_writer* w, const uint8_t* p, uint32_t n);

/* Payload reading.
 * Same sticky-error pattern. Reads are bounds-checked: reading past the end sets
 * DX_ERR_TRUNCATED and returns 0/NULL. Strings and bytes are returned as pointers INTO the
 * input buffer (no copy): they are valid only as long as that buffer is. Strings are NOT
 * NUL-terminated; use the returned length. */

/** Starts reading @p len bytes at @p data. */
DX_API void dx_reader_init(dx_reader* r, const uint8_t* data, uint32_t len);
DX_API uint8_t dx_r_u8(dx_reader* r);
DX_API uint16_t dx_r_u16(dx_reader* r);
DX_API uint32_t dx_r_u32(dx_reader* r);
DX_API uint64_t dx_r_u64(dx_reader* r);
DX_API float dx_r_f32(dx_reader* r);
DX_API double dx_r_f64(dx_reader* r);
/** Reads a string; sets *len. Errors: DX_ERR_TRUNCATED, DX_ERR_BAD_UTF8. */
DX_API const char* dx_r_string(dx_reader* r, uint16_t* len);
/** Reads a byte blob; sets *len. Error: DX_ERR_TRUNCATED. */
DX_API const uint8_t* dx_r_bytes(dx_reader* r, uint32_t* len);

/**
 * Checks that @p in (@p len bytes) is exactly one valid payload for @p schema: every field
 * present and well-formed, valid UTF-8, bool 0/1, nothing left over. Never reads out of
 * bounds. @return DX_OK, DX_ERR_INVALID_ARG, DX_ERR_BAD_SCHEMA, DX_ERR_TRUNCATED,
 * DX_ERR_BAD_UTF8, DX_ERR_BAD_VALUE or DX_ERR_TRAILING. Thread-safe, no allocation.
 */
DX_API int dx_payload_validate_schema(const dx_schema* schema, const uint8_t* in, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
