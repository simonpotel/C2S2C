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

/**
 * Validates @p in as the payload of packet @p packet_id, using the layout generated from
 * Registry/protocol.toml (see dx_payload_validate_schema for what "valid" means).
 * @return DX_OK, DX_ERR_UNKNOWN_ID if the id is not in the registry, or any error of
 * dx_payload_validate_schema. Thread-safe, no allocation.
 */
DX_API int dx_payload_validate(uint16_t packet_id, const uint8_t* in, uint32_t len);

/** Name of a registered packet (e.g. "chat_message"), or NULL if unknown. Static string. */
DX_API const char* dx_packet_name(uint16_t packet_id);

/* Runtime: one context = one event loop owning sockets and buffers.
 * Threading: a context must be used by ONE thread at a time (its owner). Several contexts may
 * run in parallel in different threads. Typical loop:
 *     dx_packet pk[64];
 *     int n = dx_poll(ctx, pk, 64, 100);   // wait up to 100 ms, get up to 64 packets at once
 *     for (i < n) handle(pk[i]);           // pk[i].data is valid until dx_release
 *     dx_release(ctx, n);                  // give the memory back, in order
 */

/** Fills @p cfg with the default settings (all limits and the backend). */
DX_API void dx_config_default(dx_config* cfg);

/**
 * Creates a context and reserves ALL its memory (see dx_config). @p cfg may be NULL (defaults).
 * @return the context, or NULL if the settings are invalid or memory is short. The caller owns
 * it and must call dx_destroy. Not thread-safe with other calls on the same context.
 */
DX_API dx_ctx* dx_create(const dx_config* cfg);

/** Closes every socket and frees the context. NULL is allowed. Pointers from dx_poll die. */
DX_API void dx_destroy(dx_ctx* ctx);

/**
 * Starts accepting connections on @p addr (numeric IPv4/IPv6, e.g. "127.0.0.1") and @p port
 * (0 = let the system choose; read it back with dx_listen_port). One listener per context.
 * @return DX_OK, DX_ERR_INVALID_ARG, DX_ERR_LIMIT (already listening) or DX_ERR_IO.
 */
DX_API int dx_listen(dx_ctx* ctx, const char* addr, uint16_t port);

/** Port of the listener, or a negative error if the context is not listening. */
DX_API int dx_listen_port(const dx_ctx* ctx);

/**
 * Starts connecting to @p addr:@p port without blocking; *out_conn gets the handle at once and
 * packets may already be sent (they wait in the buffer). DX_SYS_CONNECTED arrives from dx_poll
 * when the link is up, DX_SYS_DISCONNECTED if it fails.
 * @return DX_OK, DX_ERR_INVALID_ARG, DX_ERR_LIMIT (no free connection slot) or DX_ERR_IO.
 */
DX_API int dx_connect(dx_ctx* ctx, const char* addr, uint16_t port, uint32_t* out_conn);

/**
 * Runs the event loop for up to @p timeout_ms (0 = do not wait, -1 = until something happens)
 * and stores up to @p max packets / events in @p out. All of them stay valid until released.
 * Packets arrive in order per connection; a connection's DX_SYS_DISCONNECTED comes after its
 * last packet. Invalid traffic closes that connection with the reason in the event payload.
 * If the application never releases, the receive buffer fills and reading pauses (the peers'
 * TCP windows fill up): that is back-pressure, not data loss.
 * @return the number of entries stored (0 on timeout) or a negative error (DX_ERR_INVALID_ARG).
 */
DX_API int dx_poll(dx_ctx* ctx, dx_packet* out, int max, int timeout_ms);

/** Releases the @p count OLDEST entries handed out by dx_poll; their pointers become invalid. */
DX_API void dx_release(dx_ctx* ctx, int count);

/**
 * Sends one packet: validates it (id direction and, unless DX_CFG_NO_VALIDATE, its payload),
 * copies it into the connection's send buffer and writes what the socket accepts. The sequence
 * number is assigned by the library. @p data is copied: the caller keeps ownership.
 * @return DX_OK, DX_ERR_BAD_CONN, DX_ERR_BAD_ID (id direction does not match the connection's
 * role: clients send ids without bit 15, servers with it), DX_ERR_TOO_LARGE, a payload error,
 * or DX_ERR_WOULD_BLOCK when the send buffer is full (call dx_poll, then retry).
 */
DX_API int dx_send(dx_ctx* ctx, uint32_t conn, uint16_t packet_id, const uint8_t* data,
                   uint32_t len);

/**
 * Sends @p count packets in order. Stops at the first failure.
 * @return how many were sent, or the error of the first packet if none could be.
 */
DX_API int dx_send_batch(dx_ctx* ctx, const dx_outgoing* pkts, int count);

/**
 * Closes a connection gracefully: queued data is still sent, then the socket is closed and
 * DX_SYS_DISCONNECTED (reason 0) is delivered. @return DX_OK or DX_ERR_BAD_CONN.
 */
DX_API int dx_close(dx_ctx* ctx, uint32_t conn);

#ifdef __cplusplus
}
#endif

#endif
