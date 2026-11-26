/* wasm_glue.c - what the WebAssembly build needs besides the codec sources.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "C2S2C/c2s2c.h"
#include "c2s2c_simd.h"

/*
 * The browser build contains only the codec (headers, payload validation, UTF-8,
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
uint32_t c2s2c_crc32c(uint32_t crc, const uint8_t* data, size_t len)
{
    return c2s2c_crc32c_generic(crc, data, len);
}

/* Scratch area shared with JavaScript: it writes input bytes here and reads results back.
 * (A mutable global is the simplest and safest way to pass bytes in and out of WebAssembly.) */
#define SCRATCH_SIZE (1u << 20)
static uint8_t scratch[SCRATCH_SIZE];

uint8_t* c2s2c_wasm_scratch(void)
{
    return scratch;
}

uint32_t c2s2c_wasm_scratch_size(void)
{
    return SCRATCH_SIZE;
}

/* Parses "u8,string,array<u16>" (the notation of Generated/registry.txt) into fields. */
static const struct {
    const char* name;
    uint8_t type;
} types[] = {{"u8", C2S2C_T_U8},      {"u16", C2S2C_T_U16},   {"u32", C2S2C_T_U32},
             {"u64", C2S2C_T_U64},    {"i8", C2S2C_T_I8},     {"i16", C2S2C_T_I16},
             {"i32", C2S2C_T_I32},    {"i64", C2S2C_T_I64},   {"f32", C2S2C_T_F32},
             {"f64", C2S2C_T_F64},    {"bool", C2S2C_T_BOOL}, {"string", C2S2C_T_STRING},
             {"bytes", C2S2C_T_BYTES}};

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
int c2s2c_wasm_validate_schema(uint32_t payload_len, uint32_t schema_len)
{
    if (payload_len + schema_len > SCRATCH_SIZE) return C2S2C_ERR_INVALID_ARG;
    const char* s = (const char*)scratch + payload_len;
    const char* end = s + schema_len;
    c2s2c_field fields[16];
    uint16_t count = 0;
    while (s < end) {
        if (count == 16) return C2S2C_ERR_BAD_SCHEMA;
        const char* comma = s;
        while (comma < end && *comma != ',')
            comma++;
        size_t len = (size_t)(comma - s);
        if (len > 7 && s[0] == 'a' && s[5] == '<') { /* array<T> */
            fields[count].type = C2S2C_T_ARRAY;
            fields[count].elem = type_of(s + 6, len - 7);
        } else {
            fields[count].type = type_of(s, len);
            fields[count].elem = 0;
        }
        count++;
        s = comma < end ? comma + 1 : comma;
    }
    c2s2c_schema schema = {count, fields};
    return c2s2c_payload_validate_schema(&schema, scratch, payload_len);
}

/** Validates scratch[0..len) as the payload of packet `id` (registry layout). */
int c2s2c_wasm_validate(uint32_t id, uint32_t len)
{
    return c2s2c_payload_validate((uint16_t)id, scratch, len);
}

/**
 * Header encode/decode through the scratch area: the 14 header bytes are at scratch[0..14).
 * c2s2c_wasm_decode_header returns C2S2C_OK or an error and, on success, writes the decoded fields
 * as little-endian words at scratch[16..]: magic, version, flags, packet id, length, sequence.
 */
int c2s2c_wasm_decode_header(uint32_t available)
{
    c2s2c_header h;
    int rc = c2s2c_header_decode(scratch, available, &h);
    if (rc != C2S2C_OK) return rc;
    uint32_t out[6] = {h.magic, h.version, h.flags, h.packet_id, h.length, h.sequence};
    memcpy(scratch + 16, out, sizeof out);
    return C2S2C_OK;
}

int c2s2c_wasm_encode_header(uint32_t flags, uint32_t id, uint32_t length, uint32_t sequence)
{
    c2s2c_header h = {C2S2C_MAGIC, C2S2C_PROTOCOL_VERSION, (uint8_t)flags, (uint16_t)id, length,
                      sequence};
    return c2s2c_header_encode(&h, scratch);
}

uint32_t c2s2c_wasm_crc32c(uint32_t crc, uint32_t len)
{
    return c2s2c_crc32c(crc, scratch, len);
}

/**
 * Registry entry @p index written to scratch as "id name layout" (text, so JavaScript can split
 * it). Returns its length, or 0 when the index is out of range.
 */
uint32_t c2s2c_wasm_registry_get(uint32_t index)
{
    uint16_t id;
    const char *name, *layout;
    if (c2s2c_registry_get(index, &id, &name, &layout) != C2S2C_OK) return 0;
    uint32_t n = 0;
    const char hex[] = "0123456789abcdef";
    for (int shift = 12; shift >= 0; shift -= 4)
        scratch[n++] = (uint8_t)hex[(id >> shift) & 0xFu];
    scratch[n++] = ' ';
    while (*name)
        scratch[n++] = (uint8_t)*name++;
    scratch[n++] = ' ';
    while (*layout)
        scratch[n++] = (uint8_t)*layout++;
    return n;
}

/** Copies the message of an error code to scratch and returns its length. */
uint32_t c2s2c_wasm_strerror(int err)
{
    const char* s = c2s2c_strerror(err);
    uint32_t n = 0;
    while (s[n] && n < 200) {
        scratch[n] = (uint8_t)s[n];
        n++;
    }
    return n;
}
