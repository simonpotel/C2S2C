/* c2s2c_types.h - public types and wire constants.
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_TYPES_H
#define C2S2C_TYPES_H

/*
 * Names for the numbers of the protocol (sizes, flag bits, limits) and the c2s2c_header struct.
 * Using names instead of raw numbers keeps the C code readable and lets other languages copy
 * the exact same values. Byte-level layout: DOCUMENTATION.md section 3.
 */
#include <stddef.h>
#include <stdint.h>

/* Wire constants. They are macros (not enums) so every binding and the codegen can read them
 * textually, and values are fixed forever once released: they ARE the protocol. */
#define C2S2C_MAGIC 0xC0DEu /* first 2 bytes of every packet; detects non-C2S2C/desynced data */
#define C2S2C_HEADER_SIZE 14
/* Upper bound on `length`: protects the receiver from hostile size fields. */
#define C2S2C_MAX_PAYLOAD (16u * 1024u * 1024u)

#define C2S2C_FLAG_STOC 0x01u       /* bit0: direction (0 = CTOS, 1 = STOC) */
#define C2S2C_FLAG_COMPRESSED 0x02u /* bit1 */
#define C2S2C_FLAGS_RESERVED_MASK 0xFCu
#define C2S2C_ID_STOC_BIT 0x8000u

/* Why packet_id also carries the direction (bit 15): a packet's meaning is readable from the id
 * alone, and the codec cross-checks it against the flag to catch corrupt or forged headers. */

/** Decoded header (host representation; the wire format is little-endian, unpadded). */
typedef struct c2s2c_header {
    uint16_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t packet_id;
    uint32_t length;
    uint32_t sequence;
} c2s2c_header;

/* Payload format.
 * A payload is a plain sequence of fields, no padding, little-endian:
 *   integers/floats: fixed size      bool: 1 byte (0 or 1)
 *   string: u16 byte count + UTF-8   bytes: u32 byte count + data
 *   array<T>: u16 element count + elements (T = any type except array)
 * A schema is the ordered list of field types a packet must contain. */
#define C2S2C_T_U8 1
#define C2S2C_T_U16 2
#define C2S2C_T_U32 3
#define C2S2C_T_U64 4
#define C2S2C_T_I8 5
#define C2S2C_T_I16 6
#define C2S2C_T_I32 7
#define C2S2C_T_I64 8
#define C2S2C_T_F32 9
#define C2S2C_T_F64 10
#define C2S2C_T_BOOL 11
#define C2S2C_T_STRING 12
#define C2S2C_T_BYTES 13
#define C2S2C_T_ARRAY 14

/** One field of a schema; `elem` is the element type when `type` is C2S2C_T_ARRAY, else 0. */
typedef struct c2s2c_field {
    uint8_t type;
    uint8_t elem;
} c2s2c_field;

/** Ordered field list describing one packet payload. */
typedef struct c2s2c_schema {
    uint16_t count;
    const c2s2c_field* fields;
} c2s2c_schema;

/** Cursor that appends fields to a caller-owned buffer. The first error sticks in `err`. */
typedef struct c2s2c_writer {
    uint8_t* data;
    uint32_t cap;
    uint32_t len;
    int err;
} c2s2c_writer;

/** Cursor that reads fields from a caller-owned buffer. The first error sticks in `err`. */
typedef struct c2s2c_reader {
    const uint8_t* data;
    uint32_t len;
    uint32_t pos;
    int err;
} c2s2c_reader;

/* Runtime types (sockets, event loop). */

/** Opaque context: owns sockets, buffers and connections. Use it from ONE thread. */
typedef struct c2s2c_ctx c2s2c_ctx;

/** I/O backends (c2s2c_config.backend). AUTO picks the best one of the platform. */
#define C2S2C_BACKEND_AUTO 0
#define C2S2C_BACKEND_POLL 1
#define C2S2C_BACKEND_EPOLL 2
#define C2S2C_BACKEND_KQUEUE 3
#define C2S2C_BACKEND_URING 4

/** Most packet ids one tunnel can drop (see c2s2c_tunnel). */
#define C2S2C_TUNNEL_MAX_DROP 16

/** c2s2c_config.flags: accept packets without checking their payload against the registry. */
#define C2S2C_CFG_NO_VALIDATE 0x1u

/** System events delivered by c2s2c_poll like packets (ids reserved for the library). */
#define C2S2C_SYS_CONNECTED 0x7F01u /* a connection is established; no payload */
#define C2S2C_SYS_DISCONNECTED                                                                     \
    0x7F02u /* a connection is gone; payload = i32 reason (little-endian)                          \
             */

/** Optional custom allocator, used ONLY while creating and destroying a context. */
typedef struct c2s2c_alloc {
    void* (*alloc)(size_t size, void* user);
    void (*release)(void* ptr, void* user);
    void* user;
} c2s2c_alloc;

/**
 * Context settings. Zero in a field means "default". All memory is reserved at c2s2c_create from
 * these values, so after creation the library never allocates.
 * Memory use is about max_conns * (max_payload + 14 + tx_bytes) + rx_ring_bytes.
 */
typedef struct c2s2c_config {
    uint32_t size;          /* sizeof(c2s2c_config): lets the struct grow without breaking ABI */
    uint32_t max_conns;     /* default 256, at most 65535 */
    uint32_t max_payload;   /* largest accepted payload; default 65536, at most C2S2C_MAX_PAYLOAD */
    uint32_t rx_ring_bytes; /* storage of received packets; default 4 MiB, >= 2 * max_payload */
    uint32_t rx_queue;      /* packets that can wait for the application; default 4096 */
    uint32_t tx_bytes;      /* send buffer per connection; default 4 * (max_payload + 14) */
    uint32_t flags;         /* C2S2C_CFG_* */
    uint32_t idle_timeout_ms; /* close silent connections after this long; default 60000 */
    uint32_t backend;         /* C2S2C_BACKEND_*; default AUTO */
    c2s2c_alloc alloc;        /* default malloc/free */
    uint32_t
        max_tunnels; /* relays set up with c2s2c_tunnel; default 16 (added after first release) */
    uint32_t reserved;
} c2s2c_config;

/** A received packet or system event, valid until released with c2s2c_release. */
typedef struct c2s2c_packet {
    uint32_t conn;       /* connection handle */
    uint32_t sequence;   /* sequence number from the header (0 for events) */
    const uint8_t* data; /* payload, inside a library-owned ring (never NULL) */
    uint32_t length;     /* payload size */
    uint16_t packet_id;  /* registry id, or C2S2C_SYS_* for events */
    uint8_t flags;       /* header flags (0 for events) */
    uint8_t reserved;
} c2s2c_packet;

/** A packet to send with c2s2c_send_batch. */
typedef struct c2s2c_outgoing {
    uint32_t conn;
    uint16_t packet_id;
    uint16_t reserved;
    const uint8_t* data;
    uint32_t length;
} c2s2c_outgoing;

#endif
