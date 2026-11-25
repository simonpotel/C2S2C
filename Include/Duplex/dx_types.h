/* dx_types.h - public types and wire constants.
 * SPDX-License-Identifier: MIT */
#ifndef DX_TYPES_H
#define DX_TYPES_H

/*
 * Names for the numbers of the protocol (sizes, flag bits, limits) and the dx_header struct.
 * Using names instead of raw numbers keeps the C code readable and lets other languages copy
 * the exact same values. Byte-level layout: DOCUMENTATION.md section 3.
 */
#include <stdint.h>

/* Wire constants. They are macros (not enums) so every binding and the codegen can read them
 * textually, and values are fixed forever once released: they ARE the protocol. */
#define DX_MAGIC 0xC0DEu /* first 2 bytes of every packet; detects non-Duplex/desynced data */
#define DX_HEADER_SIZE 14
/* Upper bound on `length`: protects the receiver from hostile size fields. */
#define DX_MAX_PAYLOAD (16u * 1024u * 1024u)

#define DX_FLAG_STOC 0x01u       /* bit0: direction (0 = CTOS, 1 = STOC) */
#define DX_FLAG_COMPRESSED 0x02u /* bit1 */
#define DX_FLAGS_RESERVED_MASK 0xFCu
#define DX_ID_STOC_BIT 0x8000u

/* Why packet_id also carries the direction (bit 15): a packet's meaning is readable from the id
 * alone, and the codec cross-checks it against the flag to catch corrupt or forged headers. */

/** Decoded header (host representation; the wire format is little-endian, unpadded). */
typedef struct dx_header {
    uint16_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t packet_id;
    uint32_t length;
    uint32_t sequence;
} dx_header;

/* Payload format.
 * A payload is a plain sequence of fields, no padding, little-endian:
 *   integers/floats: fixed size      bool: 1 byte (0 or 1)
 *   string: u16 byte count + UTF-8   bytes: u32 byte count + data
 *   array<T>: u16 element count + elements (T = any type except array)
 * A schema is the ordered list of field types a packet must contain. */
#define DX_T_U8 1
#define DX_T_U16 2
#define DX_T_U32 3
#define DX_T_U64 4
#define DX_T_I8 5
#define DX_T_I16 6
#define DX_T_I32 7
#define DX_T_I64 8
#define DX_T_F32 9
#define DX_T_F64 10
#define DX_T_BOOL 11
#define DX_T_STRING 12
#define DX_T_BYTES 13
#define DX_T_ARRAY 14

/** One field of a schema; `elem` is the element type when `type` is DX_T_ARRAY, else 0. */
typedef struct dx_field {
    uint8_t type;
    uint8_t elem;
} dx_field;

/** Ordered field list describing one packet payload. */
typedef struct dx_schema {
    uint16_t count;
    const dx_field* fields;
} dx_schema;

/** Cursor that appends fields to a caller-owned buffer. The first error sticks in `err`. */
typedef struct dx_writer {
    uint8_t* data;
    uint32_t cap;
    uint32_t len;
    int err;
} dx_writer;

/** Cursor that reads fields from a caller-owned buffer. The first error sticks in `err`. */
typedef struct dx_reader {
    const uint8_t* data;
    uint32_t len;
    uint32_t pos;
    int err;
} dx_reader;

#endif
