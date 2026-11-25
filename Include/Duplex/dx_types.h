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

#endif
