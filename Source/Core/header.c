/* header.c - validation, encoding and decoding of the 14-byte packet header.
 * SPDX-License-Identifier: MIT */
/*
 * In plain words: every packet starts with a 14-byte "envelope label" (the header) that says
 * what kind of packet it is, how big its content is and which way it travels. This file turns
 * that label into bytes (encode) and bytes back into a label (decode), and refuses any label
 * that breaks the rules. Rules and byte layout: DOCUMENTATION.md sections 3 and 4.
 * It never allocates, does no I/O and keeps no state, so any thread can call it.
 */
#include "../Codec/endian.h"
#include "Duplex/duplex.h"

/*
 * Single validation routine shared by encode and decode, so a header we refuse to read is
 * also a header we refuse to write: both sides of a connection apply the same rules.
 * Checks run cheapest and most fundamental first, so the returned error is the most
 * useful one (a wrong magic means "not our protocol", whatever the other fields say).
 */
static int dx_header_check(const dx_header* h)
{
    /* 1. Not our protocol at all (or a desynchronised stream). */
    if (h->magic != DX_MAGIC) return DX_ERR_BAD_MAGIC;

    /* 2. Only protocol v1 is understood; a newer peer must be rejected, not misparsed. */
    if (h->version != DX_PROTOCOL_VERSION) return DX_ERR_BAD_VERSION;

    /* 3. Reserved flag bits must be 0 so they can get a meaning later without ambiguity:
     *    an old reader sees them set and rejects, instead of silently ignoring them. */
    if (h->flags & DX_FLAGS_RESERVED_MASK) return DX_ERR_BAD_FLAGS;

    /* 4. Direction is stored twice (flag bit0 and id bit 15). They must agree: a mismatch
     *    means a client is sending a server-only packet (or the data is corrupt). */
    if ((h->flags & DX_FLAG_STOC) != ((h->packet_id & DX_ID_STOC_BIT) ? DX_FLAG_STOC : 0u))
        return DX_ERR_BAD_ID;

    /* 5. Cap the payload so a hostile length field cannot make us reserve gigabytes. */
    if (h->length > DX_MAX_PAYLOAD) return DX_ERR_TOO_LARGE;

    return DX_OK;
}

int dx_header_encode(const dx_header* h, uint8_t out[DX_HEADER_SIZE])
{
    if (!h || !out) return DX_ERR_INVALID_ARG;

    /* Validate before writing so `out` never holds a header we would reject on decode. */
    int rc = dx_header_check(h);
    if (rc != DX_OK) return rc;

    /* Field offsets follow the wire table in DOCUMENTATION.md section 2. */
    dx_wr16(out, h->magic);
    out[2] = h->version;
    out[3] = h->flags;
    dx_wr16(out + 4, h->packet_id);
    dx_wr32(out + 6, h->length);
    dx_wr32(out + 10, h->sequence);
    return DX_OK;
}

int dx_header_decode(const uint8_t* in, size_t len, dx_header* out)
{
    if (!in || !out) return DX_ERR_INVALID_ARG;

    /* Bounds check first: every read below touches bytes 0..13 and the input comes from
     * the network, so its length can never be assumed. */
    if (len < DX_HEADER_SIZE) return DX_ERR_TRUNCATED;

    dx_header h = {dx_rd16(in), in[2], in[3], dx_rd16(in + 4), dx_rd32(in + 6), dx_rd32(in + 10)};

    /* Fill the caller's struct only when the header is valid, so a failed decode cannot
     * leave half-trusted data behind that a caller might use by mistake. */
    int rc = dx_header_check(&h);
    if (rc == DX_OK) *out = h;
    return rc;
}
