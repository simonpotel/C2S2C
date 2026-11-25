/* wasm_glue.c - what the WebAssembly build needs besides the codec sources.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "Duplex/duplex.h"
#include "dx_simd.h"

/*
 * In plain words: the browser build contains only the codec (headers, payload validation, UTF-8,
 * registry, CRC-32C): no sockets, no event loop. In a browser the network is a WebSocket owned
 * by JavaScript, and this module checks and builds the bytes that travel on it.
 * It has no C library, so the few functions normally taken from it are written here, and
 * JavaScript exchanges bytes with it through one fixed scratch area in WebAssembly memory.
 */

void* memcpy(void* dst, const void* src, size_t n)
{
    uint8_t* d = dst;
    const uint8_t* s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void* memset(void* dst, int value, size_t n)
{
    uint8_t* d = dst;
    while (n--)
        *d++ = (uint8_t)value;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

/* The portable checksum is the only one in this build (no CPU feature detection in a browser). */
uint32_t dx_crc32c(uint32_t crc, const uint8_t* data, size_t len)
{
    return dx_crc32c_generic(crc, data, len);
}

/* Scratch area shared with JavaScript: it writes input bytes here and reads results back.
 * (A mutable global is the simplest and safest way to pass bytes in and out of WebAssembly.) */
#define SCRATCH_SIZE (1u << 20)
static uint8_t scratch[SCRATCH_SIZE];

uint8_t* dx_wasm_scratch(void)
{
    return scratch;
}

uint32_t dx_wasm_scratch_size(void)
{
    return SCRATCH_SIZE;
}

/* Parses "u8,string,array<u16>" (the notation of Generated/registry.txt) into fields. */
static const struct {
    const char* name;
    uint8_t type;
} types[] = {{"u8", DX_T_U8},      {"u16", DX_T_U16}, {"u32", DX_T_U32},   {"u64", DX_T_U64},
             {"i8", DX_T_I8},      {"i16", DX_T_I16}, {"i32", DX_T_I32},   {"i64", DX_T_I64},
             {"f32", DX_T_F32},    {"f64", DX_T_F64}, {"bool", DX_T_BOOL}, {"string", DX_T_STRING},
             {"bytes", DX_T_BYTES}};

static uint8_t type_of(const char* s, size_t n)
{
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        size_t k = 0;
        while (types[i].name[k] && k < n && types[i].name[k] == s[k])
            k++;
        if (types[i].name[k] == 0 && k == n) return types[i].type;
    }
    return 0;
}

/**
 * Validates scratch[0..payload_len) against the schema text stored right after it
 * (scratch[payload_len .. payload_len + schema_len)). Used by the vector tests.
 */
int dx_wasm_validate_schema(uint32_t payload_len, uint32_t schema_len)
{
    if (payload_len + schema_len > SCRATCH_SIZE) return DX_ERR_INVALID_ARG;
    const char* s = (const char*)scratch + payload_len;
    const char* end = s + schema_len;
    dx_field fields[16];
    uint16_t count = 0;
    while (s < end) {
        if (count == 16) return DX_ERR_BAD_SCHEMA;
        const char* comma = s;
        while (comma < end && *comma != ',')
            comma++;
        size_t len = (size_t)(comma - s);
        if (len > 7 && s[0] == 'a' && s[5] == '<') { /* array<T> */
            fields[count].type = DX_T_ARRAY;
            fields[count].elem = type_of(s + 6, len - 7);
        } else {
            fields[count].type = type_of(s, len);
            fields[count].elem = 0;
        }
        count++;
        s = comma < end ? comma + 1 : comma;
    }
    dx_schema schema = {count, fields};
    return dx_payload_validate_schema(&schema, scratch, payload_len);
}

/** Validates scratch[0..len) as the payload of packet `id` (registry layout). */
int dx_wasm_validate(uint32_t id, uint32_t len)
{
    return dx_payload_validate((uint16_t)id, scratch, len);
}

/**
 * Header encode/decode through the scratch area: the 14 header bytes are at scratch[0..14).
 * dx_wasm_decode_header returns DX_OK or an error and, on success, writes the decoded fields as
 * little-endian words at scratch[16..]: magic, version, flags, packet id, length, sequence.
 */
int dx_wasm_decode_header(uint32_t available)
{
    dx_header h;
    int rc = dx_header_decode(scratch, available, &h);
    if (rc != DX_OK) return rc;
    uint32_t out[6] = {h.magic, h.version, h.flags, h.packet_id, h.length, h.sequence};
    memcpy(scratch + 16, out, sizeof out);
    return DX_OK;
}

int dx_wasm_encode_header(uint32_t flags, uint32_t id, uint32_t length, uint32_t sequence)
{
    dx_header h = {DX_MAGIC, DX_PROTOCOL_VERSION, (uint8_t)flags, (uint16_t)id, length, sequence};
    return dx_header_encode(&h, scratch);
}

uint32_t dx_wasm_crc32c(uint32_t crc, uint32_t len)
{
    return dx_crc32c(crc, scratch, len);
}

/** Copies the message of an error code to scratch and returns its length. */
uint32_t dx_wasm_strerror(int err)
{
    const char* s = dx_strerror(err);
    uint32_t n = 0;
    while (s[n] && n < 200) {
        scratch[n] = (uint8_t)s[n];
        n++;
    }
    return n;
}
