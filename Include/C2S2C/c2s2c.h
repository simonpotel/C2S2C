/* c2s2c.h - public C API of C2S2C (stable ABI).
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_H
#define C2S2C_H

/*
 * This is the only header users need. It lists every function of the library. Because the
 * functions use plain C types, any language that can call C (Python, Rust, Node, C++...) can
 * load the compiled library and use them: that is what "stable ABI" means in practice.
 * Convention: functions return 0 on success and a negative C2S2C_ERR_* code on failure.
 */
#include <stddef.h>
#include <stdint.h>

#include "c2s2c_errors.h"
#include "c2s2c_export.h"
#include "c2s2c_types.h"
#include "c2s2c_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns a static, NUL-terminated description of @p err. Never NULL. Thread-safe. */
C2S2C_API const char* c2s2c_strerror(int err);

/** Returns C2S2C_ABI_VERSION of the loaded library. Thread-safe. */
C2S2C_API uint32_t c2s2c_abi_version(void);

/**
 * Serializes @p h into @p out (C2S2C_HEADER_SIZE bytes, little-endian). Validates the header.
 * @return C2S2C_OK, C2S2C_ERR_INVALID_ARG (NULL), or the C2S2C_ERR_BAD_* / C2S2C_ERR_TOO_LARGE of
 * the first invalid field. The caller owns both buffers. Thread-safe.
 */
C2S2C_API int c2s2c_header_encode(const c2s2c_header* h, uint8_t out[C2S2C_HEADER_SIZE]);

/**
 * Parses and validates a header from @p in. Never reads more than @p len bytes.
 * @return C2S2C_OK, C2S2C_ERR_INVALID_ARG, C2S2C_ERR_TRUNCATED (len < C2S2C_HEADER_SIZE), or
 * C2S2C_ERR_BAD_* / C2S2C_ERR_TOO_LARGE. @p out is written only on success. Thread-safe.
 */
C2S2C_API int c2s2c_header_decode(const uint8_t* in, size_t len, c2s2c_header* out);

/**
 * CRC-32C (Castagnoli) checksum, for applications that want to verify data (for example a file
 * sent in `upload` chunks). Pass 0 as @p crc for a new checksum, or the previous result to
 * continue over more data: c2s2c_crc32c(c2s2c_crc32c(0, a, n), b, m) equals the checksum of a then
 * b. c2s2c_crc32c(0, "123456789", 9) is 0xE3069283. Uses the CPU's CRC instructions when it has
 * them (chosen at run time), the portable code otherwise; both give identical results. Thread-safe,
 * no allocation. @p data may be NULL only if @p len is 0.
 */
C2S2C_API uint32_t c2s2c_crc32c(uint32_t crc, const uint8_t* data, size_t len);

/* Payload writing.
 * Pattern: init, call c2s2c_w_* for each field, then check w.err once and use w.len as the size.
 * After the first error every later call does nothing, so there is no need to test each call.
 * Signed integers and bool: write with the unsigned function of the same size.
 * Memory: the writer never allocates; the caller owns the buffer. Thread-safe per writer. */

/** Starts writing into @p buf (capacity @p cap). */
C2S2C_API void c2s2c_writer_init(c2s2c_writer* w, uint8_t* buf, uint32_t cap);
C2S2C_API void c2s2c_w_u8(c2s2c_writer* w, uint8_t v);
C2S2C_API void c2s2c_w_u16(c2s2c_writer* w, uint16_t v);
C2S2C_API void c2s2c_w_u32(c2s2c_writer* w, uint32_t v);
C2S2C_API void c2s2c_w_u64(c2s2c_writer* w, uint64_t v);
C2S2C_API void c2s2c_w_f32(c2s2c_writer* w, float v);
C2S2C_API void c2s2c_w_f64(c2s2c_writer* w, double v);
/** Writes a string of @p n bytes (no NUL needed). Errors: C2S2C_ERR_BAD_UTF8, C2S2C_ERR_OVERFLOW.
 */
C2S2C_API void c2s2c_w_string(c2s2c_writer* w, const char* s, uint16_t n);
/** Writes @p n raw bytes with their u32 length prefix. Error: C2S2C_ERR_OVERFLOW. */
C2S2C_API void c2s2c_w_bytes(c2s2c_writer* w, const uint8_t* p, uint32_t n);

/* Payload reading.
 * Same sticky-error pattern. Reads are bounds-checked: reading past the end sets
 * C2S2C_ERR_TRUNCATED and returns 0/NULL. Strings and bytes are returned as pointers INTO the
 * input buffer (no copy): they are valid only as long as that buffer is. Strings are NOT
 * NUL-terminated; use the returned length. */

/** Starts reading @p len bytes at @p data. */
C2S2C_API void c2s2c_reader_init(c2s2c_reader* r, const uint8_t* data, uint32_t len);
C2S2C_API uint8_t c2s2c_r_u8(c2s2c_reader* r);
C2S2C_API uint16_t c2s2c_r_u16(c2s2c_reader* r);
C2S2C_API uint32_t c2s2c_r_u32(c2s2c_reader* r);
C2S2C_API uint64_t c2s2c_r_u64(c2s2c_reader* r);
C2S2C_API float c2s2c_r_f32(c2s2c_reader* r);
C2S2C_API double c2s2c_r_f64(c2s2c_reader* r);
/** Reads a string; sets *len. Errors: C2S2C_ERR_TRUNCATED, C2S2C_ERR_BAD_UTF8. */
C2S2C_API const char* c2s2c_r_string(c2s2c_reader* r, uint16_t* len);
/** Reads a byte blob; sets *len. Error: C2S2C_ERR_TRUNCATED. */
C2S2C_API const uint8_t* c2s2c_r_bytes(c2s2c_reader* r, uint32_t* len);

/**
 * Checks that @p in (@p len bytes) is exactly one valid payload for @p schema: every field
 * present and well-formed, valid UTF-8, bool 0/1, nothing left over. Never reads out of
 * bounds. @return C2S2C_OK, C2S2C_ERR_INVALID_ARG, C2S2C_ERR_BAD_SCHEMA, C2S2C_ERR_TRUNCATED,
 * C2S2C_ERR_BAD_UTF8, C2S2C_ERR_BAD_VALUE or C2S2C_ERR_TRAILING. Thread-safe, no allocation.
 */
C2S2C_API int c2s2c_payload_validate_schema(const c2s2c_schema* schema, const uint8_t* in,
                                            uint32_t len);

/**
 * Validates @p in as the payload of packet @p packet_id, using the layout generated from
 * Registry/protocol.toml (see c2s2c_payload_validate_schema for what "valid" means).
 * @return C2S2C_OK, C2S2C_ERR_UNKNOWN_ID if the id is not in the registry, or any error of
 * c2s2c_payload_validate_schema. Thread-safe, no allocation.
 */
C2S2C_API int c2s2c_payload_validate(uint16_t packet_id, const uint8_t* in, uint32_t len);

/** Name of a registered packet (e.g. "chat_message"), or NULL if unknown. Static string. */
C2S2C_API const char* c2s2c_packet_name(uint16_t packet_id);

/* Runtime: one context = one event loop owning sockets and buffers.
 * Threading: a context must be used by ONE thread at a time (its owner). Several contexts may
 * run in parallel in different threads. Typical loop:
 *     c2s2c_packet pk[64];
 *     int n = c2s2c_poll(ctx, pk, 64, 100);   // wait up to 100 ms, get up to 64 packets at once
 *     for (i < n) handle(pk[i]);           // pk[i].data is valid until c2s2c_release
 *     c2s2c_release(ctx, n);                  // give the memory back, in order
 */

/** Fills @p cfg with the default settings (all limits and the backend). */
C2S2C_API void c2s2c_config_default(c2s2c_config* cfg);

/**
 * Creates a context and reserves ALL its memory (see c2s2c_config). @p cfg may be NULL (defaults).
 * @return the context, or NULL if the settings are invalid or memory is short. The caller owns
 * it and must call c2s2c_destroy. Not thread-safe with other calls on the same context.
 */
C2S2C_API c2s2c_ctx* c2s2c_create(const c2s2c_config* cfg);

/** Closes every socket and frees the context. NULL is allowed. Pointers from c2s2c_poll die. */
C2S2C_API void c2s2c_destroy(c2s2c_ctx* ctx);

/**
 * Starts accepting connections on @p addr (numeric IPv4/IPv6, e.g. "127.0.0.1") and @p port
 * (0 = let the system choose; read it back with c2s2c_listen_port). One listener per context.
 * @return C2S2C_OK, C2S2C_ERR_INVALID_ARG, C2S2C_ERR_LIMIT (already listening) or C2S2C_ERR_IO.
 */
C2S2C_API int c2s2c_listen(c2s2c_ctx* ctx, const char* addr, uint16_t port);

/** Port of the listener, or a negative error if the context is not listening. */
C2S2C_API int c2s2c_listen_port(const c2s2c_ctx* ctx);

/**
 * Starts connecting to @p addr:@p port without blocking; *out_conn gets the handle at once and
 * packets may already be sent (they wait in the buffer). C2S2C_SYS_CONNECTED arrives from
 * c2s2c_poll when the link is up, C2S2C_SYS_DISCONNECTED if it fails.
 * @return C2S2C_OK, C2S2C_ERR_INVALID_ARG, C2S2C_ERR_LIMIT (no free connection slot) or
 * C2S2C_ERR_IO.
 */
C2S2C_API int c2s2c_connect(c2s2c_ctx* ctx, const char* addr, uint16_t port, uint32_t* out_conn);

/**
 * Runs the event loop for up to @p timeout_ms (0 = do not wait, -1 = until something happens)
 * and stores up to @p max packets / events in @p out. All of them stay valid until released.
 * Packets arrive in order per connection; a connection's C2S2C_SYS_DISCONNECTED comes after its
 * last packet. Invalid traffic closes that connection with the reason in the event payload.
 * If the application never releases, the receive buffer fills and reading pauses (the peers'
 * TCP windows fill up): that is back-pressure, not data loss.
 * @return the number of entries stored (0 on timeout) or a negative error (C2S2C_ERR_INVALID_ARG).
 */
C2S2C_API int c2s2c_poll(c2s2c_ctx* ctx, c2s2c_packet* out, int max, int timeout_ms);

/** Releases the @p count OLDEST entries handed out by c2s2c_poll; their pointers become invalid. */
C2S2C_API void c2s2c_release(c2s2c_ctx* ctx, int count);

/**
 * Sends one packet: validates it (id direction and, unless C2S2C_CFG_NO_VALIDATE, its payload),
 * copies it into the connection's send buffer and writes what the socket accepts. The sequence
 * number is assigned by the library. @p data is copied: the caller keeps ownership.
 * @return C2S2C_OK, C2S2C_ERR_BAD_CONN, C2S2C_ERR_BAD_ID (id direction does not match the
 * connection's role: clients send ids without bit 15, servers with it), C2S2C_ERR_TOO_LARGE, a
 * payload error, or C2S2C_ERR_WOULD_BLOCK when the send buffer is full (call c2s2c_poll, then
 * retry).
 */
C2S2C_API int c2s2c_send(c2s2c_ctx* ctx, uint32_t conn, uint16_t packet_id, const uint8_t* data,
                         uint32_t len);

/**
 * Sends @p count packets in order. Stops at the first failure.
 * @return how many were sent, or the error of the first packet if none could be.
 */
C2S2C_API int c2s2c_send_batch(c2s2c_ctx* ctx, const c2s2c_outgoing* pkts, int count);

/**
 * Closes a connection gracefully: queued data is still sent, then the socket is closed and
 * C2S2C_SYS_DISCONNECTED (reason 0) is delivered. @return C2S2C_OK or C2S2C_ERR_BAD_CONN.
 */
C2S2C_API int c2s2c_close(c2s2c_ctx* ctx, uint32_t conn);

/**
 * Writes what c2s2c_send queued. c2s2c_send only copies packets into the connection's buffer; the
 * bytes leave at the next c2s2c_poll (so a whole batch of replies goes out in one system call per
 * connection), as soon as a buffer is half full, or when c2s2c_flush is called. Call it when you
 * send without polling right afterwards. @return C2S2C_OK or C2S2C_ERR_INVALID_ARG.
 */
C2S2C_API int c2s2c_flush(c2s2c_ctx* ctx);

/**
 * Relays packets between two connections of this context, both ways, inside the library: they
 * never reach c2s2c_poll. Typically a connection a client opened to us and the one we opened to the
 * real server behind (one accepted, one outgoing). Packets whose id is one of the @p drop_count
 * ids in @p drop_ids (at most C2S2C_TUNNEL_MAX_DROP) are dropped. When one side's send buffer is
 * full, reading from the other side pauses until it drains: nothing is lost and memory stays
 * bounded. When either side disconnects the other is closed; both C2S2C_SYS_DISCONNECTED events
 * are still delivered.
 * @return C2S2C_OK, C2S2C_ERR_BAD_CONN, C2S2C_ERR_INVALID_ARG (same connection, both accepted or
 * both outgoing, already relayed, too many ids) or C2S2C_ERR_LIMIT (c2s2c_config.max_tunnels
 * reached).
 */
C2S2C_API int c2s2c_tunnel(c2s2c_ctx* ctx, uint32_t first, uint32_t second,
                           const uint16_t* drop_ids, uint32_t drop_count);

#ifdef __cplusplus
}
#endif

#endif
